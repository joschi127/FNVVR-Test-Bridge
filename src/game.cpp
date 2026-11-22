// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
//
// Everything that touches the game or xNVSE. Game addresses are for FalloutNV.exe 1.4.0.525 and are the ones
// documented in xNVSE's public headers and sources (GameAPI.h/.cpp, GameObjects.cpp).

#include "game.h"

#include "log.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

#include <cctype>
#include <cstdio>
#include <string_view>
#include <unordered_map>

#include <windows.h>

// xNVSE's inline helpers (NVSEArrayVarInterface::Element) need the game's heap functions; xNVSE's own
// definitions live in GameAPI.cpp, which a plugin does not compile.
extern const _FormHeap_Allocate FormHeap_Allocate = reinterpret_cast<_FormHeap_Allocate>(0x00401000);
extern const _FormHeap_Free FormHeap_Free = reinterpret_cast<_FormHeap_Free>(0x00401030);
// The same for the HUD message and the message box (notify, ask), at the addresses in xNVSE's GameAPI.cpp
extern const _QueueUIMessage QueueUIMessage = reinterpret_cast<_QueueUIMessage>(0x007052F0);
extern const _ShowMessageBox ShowMessageBox = reinterpret_cast<_ShowMessageBox>(0x00703E80);
extern const _ShowMessageBox_button ShowMessageBox_button = reinterpret_cast<_ShowMessageBox_button>(0x0118C684);

namespace bridge
{
    namespace
    {
        constexpr uintptr_t kPlayerSingleton = 0x011DEA3C;  // PlayerCharacter**
        constexpr uintptr_t kConsoleSingleton = 0x0071B160; // ConsoleManager* (bool canCreate), cdecl
        constexpr uintptr_t kLookupFormById = 0x004F9620;   // TESForm* (UInt32 refID), cdecl
        constexpr size_t kConsoleOutFileOffset = 0x810;     // ConsoleManager::COFileName, char[260]
        constexpr size_t kConsoleOutFileSize = 260;

        NVSEConsoleInterface* g_console = nullptr;
        NVSEScriptInterface* g_script = nullptr;
        // Compiled eval expressions. xNVSE never frees a compiled script, so every new expression costs a little
        // memory for the rest of the session; the cache keeps repeats free and the limit bounds the total.
        constexpr size_t kMaxCompiles = 1024;
        std::unordered_map<std::string, Script*> g_compiled;
        size_t g_compiles = 0;
        std::string g_capturePath;

        // The open question (ask): its text and buttons stay alive while the game shows them
        bool g_asking = false;
        int g_answer = -1;  // -1: none yet
        std::string g_question;
        std::vector<std::string> g_buttons;
        constexpr char kNotifySound[] = "UIPopUpMessageGeneral";

        void AnswerCallback()
        {
            g_answer = *ShowMessageBox_button;
            g_asking = false;
        }

        TESObjectREFR* Player()
        {
            return *reinterpret_cast<TESObjectREFR**>(kPlayerSingleton);
        }

        ConsoleManager* Console()
        {
            using GetSingleton = void* (*)(bool);
            return static_cast<ConsoleManager*>(reinterpret_cast<GetSingleton>(kConsoleSingleton)(true));
        }

        TESObjectREFR* LookupReference(uint32_t refId)
        {
            using Lookup = TESForm* (*)(UInt32);
            TESForm* form = reinterpret_cast<Lookup>(kLookupFormById)(refId);
            if (!form)
                return nullptr;
            UInt8 type = form->typeID;
            bool isRef = type == kFormType_TESObjectREFR || type == kFormType_Character || type == kFormType_Creature;
            return isRef ? static_cast<TESObjectREFR*>(form) : nullptr;
        }

        bool WorldLoaded()
        {
            TESObjectREFR* player = Player();
            return player && player->parentCell;
        }

        // While alive, the game's console also writes everything it prints into our own file (the mechanism
        // behind the "scof" console command). The player's own scof setting is put back afterwards.
        class ConsoleCapture
        {
        public:
            ConsoleCapture()
            {
                m_console = Console();
                if (!m_console || g_capturePath.size() >= kConsoleOutFileSize)
                    return;
                char* name = reinterpret_cast<char*>(m_console) + kConsoleOutFileOffset;
                std::memcpy(m_saved, name, kConsoleOutFileSize);
                DeleteFileA(g_capturePath.c_str());
                std::memset(name, 0, kConsoleOutFileSize);
                std::memcpy(name, g_capturePath.c_str(), g_capturePath.size());
                m_active = true;
            }

            ~ConsoleCapture()
            {
                Restore();
            }

            std::string Finish()
            {
                Restore();
                std::string text;
                if (FILE* f = _fsopen(g_capturePath.c_str(), "rb", _SH_DENYNO))
                {
                    char buffer[4096];
                    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                        text.append(buffer, n);
                    std::fclose(f);
                }
                DeleteFileA(g_capturePath.c_str());
                return text;
            }

        private:
            void Restore()
            {
                if (!m_active)
                    return;
                std::memcpy(reinterpret_cast<char*>(m_console) + kConsoleOutFileOffset, m_saved, kConsoleOutFileSize);
                m_active = false;
            }

            ConsoleManager* m_console = nullptr;
            char m_saved[kConsoleOutFileSize] = {};
            bool m_active = false;
        };

        std::string FormatValue(const NVSEArrayVarInterface::Element& value)
        {
            char out[512];
            switch (value.GetType())
            {
            case NVSEArrayVarInterface::Element::kType_Numeric:
                std::snprintf(out, sizeof(out), "%.9g", value.GetNumber());
                return out;
            case NVSEArrayVarInterface::Element::kType_Form:
                std::snprintf(out, sizeof(out), "form %08X", value.GetFormID());
                return out;
            case NVSEArrayVarInterface::Element::kType_String:
                std::snprintf(out, sizeof(out), "\"%.480s\"", value.GetString() ? value.GetString() : "");
                return out;
            case NVSEArrayVarInterface::Element::kType_Array:
                return "array";
            default:
                return "none";
            }
        }

        // An expression naming a form created at run time (ID FFxxxxxx) is not cached: after a load the id
        // may stand for another form or none.
        bool NamesDynamicForm(const std::string& expression)
        {
            for (size_t i = 0; i + 8 <= expression.size(); ++i)
            {
                bool start = i == 0 || !std::isxdigit(static_cast<unsigned char>(expression[i - 1]));
                bool end = i + 8 == expression.size() || !std::isxdigit(static_cast<unsigned char>(expression[i + 8]));
                if (!start || !end || std::toupper(static_cast<unsigned char>(expression[i])) != 'F' ||
                    std::toupper(static_cast<unsigned char>(expression[i + 1])) != 'F')
                    continue;
                bool hex = true;
                for (size_t k = i; k < i + 8; ++k)
                    hex = hex && std::isxdigit(static_cast<unsigned char>(expression[k]));
                if (hex)
                    return true;
            }
            return false;
        }

        std::string Evaluate(const std::string& expression, TESObjectREFR* on, bool& ok)
        {
            ok = false;
            auto cached = g_compiled.find(expression);
            Script* script = cached != g_compiled.end() ? cached->second : nullptr;
            if (!script)
            {
                if (g_compiles >= kMaxCompiles)
                    return "too many different eval expressions this session (xNVSE never frees compiled scripts)";
                std::string text = "Begin Function {}\n  SetFunctionValue " + expression + "\nEnd\n";
                script = g_script->CompileScript(text.c_str());
                ++g_compiles;
                if (!script)
                    return "does not compile";
                if (!NamesDynamicForm(expression))
                    g_compiled[expression] = script;
            }
            NVSEArrayVarInterface::Element result;
            if (!g_script->CallFunction(script, on, nullptr, &result, 0))
                return "could not run it";
            ok = true;
            return FormatValue(result);
        }

        // The console output file is the bridge's own while it runs a line, so a scof sent through it would be
        // undone right away; refuse it instead
        bool SetsConsoleOutputFile(const std::string& line)
        {
            std::string word;
            for (char c : line)
            {
                if (c == ' ' || c == '\t')
                {
                    if (word.empty())
                        continue;
                    break;
                }
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            size_t dot = word.find('.');
            if (dot != std::string::npos)
                word.erase(0, dot + 1);
            return word == "scof" || word == "setconsoleoutputfile";
        }

        bool RunLine(const std::string& line, TESObjectREFR* on)
        {
            if (g_console->version >= 3 && g_console->RunScriptLine2)
                return g_console->RunScriptLine2(line.c_str(), on, false);
            return g_console->RunScriptLine(line.c_str(), on);
        }
    }

    bool InitGame(const NVSEInterface* nvse)
    {
        g_console = static_cast<NVSEConsoleInterface*>(nvse->QueryInterface(kInterface_Console));
        g_script = static_cast<NVSEScriptInterface*>(nvse->QueryInterface(kInterface_Script));
        char temp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, temp);
        if (n > 0 && n < MAX_PATH)
            g_capturePath = std::string(temp, n) + "FNVVR-Test-Bridge-" + std::to_string(GetCurrentProcessId()) + ".txt";
        else
            Log("no temp folder; console text is not captured");
        return g_console && g_script;
    }

    void ForgetCompiled()
    {
        g_compiled.clear();
    }

    std::vector<std::string> Execute(const Request& request, uint64_t frame)
    {
        switch (request.verb)
        {
        case Verb::Invalid:
            return {Err(request.error)};
        case Verb::Wait:
        case Verb::WaitFrame:
            return {Err("wait is handled by the game loop")};
        case Verb::Memory:
            return {Err("memory is answered by the pipe")};
        case Verb::Ping:
            return {Ok("pong " BRIDGE_VERSION)};
        case Verb::Notify:
        {
            if (!QueueUIMessage(request.text.c_str(), 0, nullptr, kNotifySound, request.seconds, false))
                return {Err("the game did not take the notification")};
            return {Ok()};
        }
        case Verb::Ask:
        {
            if (g_asking)
                return {Err("a question is open; see answer")};
            g_question = request.text;
            g_buttons = request.buttons;
            const char* b[kMaxButtons] = {};
            for (size_t i = 0; i < g_buttons.size() && i < kMaxButtons; ++i)
                b[i] = g_buttons[i].c_str();
            g_answer = -1;
            g_asking = true;
            // The buttons, then NULL (see xNVSE's GameAPI.h)
            bool shown = ShowMessageBox(g_question.c_str(), 0, 0, AnswerCallback, 0, 0x17, 0.0f, 0.0f,
                b[0], b[1], b[2], b[3], nullptr);
            if (!shown)
            {
                g_asking = false;
                return {Err("the game did not show the question")};
            }
            return {Ok()};
        }
        case Verb::Answer:
        {
            if (g_asking)
                return {Ok("pending")};
            if (g_answer < 0 || static_cast<size_t>(g_answer) >= g_buttons.size())
                return {Ok("none")};
            return {Ok(std::to_string(g_answer) + " " + g_buttons[static_cast<size_t>(g_answer)])};
        }
        case Verb::State:
        {
            TESObjectREFR* player = Player();
            ConsoleManager* console = Console();
            char out[128];
            std::snprintf(out, sizeof(out), "frame=%llu world=%d cell=%08X console=%d",
                static_cast<unsigned long long>(frame), WorldLoaded() ? 1 : 0,
                player && player->parentCell ? player->parentCell->refID : 0,
                console && console->IsConsoleOpen() ? 1 : 0);
            return {Ok(out)};
        }
        default:
            break;
        }

        TESObjectREFR* on = nullptr;
        if (request.verb == Verb::RunOn || request.verb == Verb::EvalOn)
        {
            if (!WorldLoaded())
                return {Err("no world loaded")};
            on = LookupReference(request.refId);
            if (!on)
                return {Err("no such reference")};
        }
        bool eval = request.verb == Verb::Eval || request.verb == Verb::EvalOn;
        if (!eval && SetsConsoleOutputFile(request.text))
            return {Err("scof is not available through the bridge (it captures console text itself)")};
        if (eval && !WorldLoaded())
            return {Err("no world loaded")};

        ConsoleCapture capture;
        bool ok = false;
        std::string detail;
        if (eval)
            detail = Evaluate(request.text, on ? on : Player(), ok);
        else
        {
            ok = RunLine(request.text, on);
            detail = ok ? "" : "the console did not take it";
        }
        std::vector<std::string> lines = ConsoleLines(capture.Finish());
        lines.push_back(ok ? Ok(detail) : Err(detail));
        return lines;
    }
}
