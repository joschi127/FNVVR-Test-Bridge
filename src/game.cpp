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

#include <cstdio>
#include <string_view>
#include <unordered_map>

#include <windows.h>

// xNVSE's inline helpers (NVSEArrayVarInterface::Element) need the game's heap functions; xNVSE's own
// definitions live in GameAPI.cpp, which a plugin does not compile.
extern const _FormHeap_Allocate FormHeap_Allocate = reinterpret_cast<_FormHeap_Allocate>(0x00401000);
extern const _FormHeap_Free FormHeap_Free = reinterpret_cast<_FormHeap_Free>(0x00401030);

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
        std::unordered_map<std::string, Script*> g_compiled;  // expression -> compiled function (game owns them)
        std::string g_capturePath;

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

        std::string Evaluate(const std::string& expression, TESObjectREFR* on, bool& ok)
        {
            ok = false;
            Script*& script = g_compiled[expression];
            if (!script)
            {
                std::string text = "Begin Function {}\n  SetFunctionValue " + expression + "\nEnd\n";
                script = g_script->CompileScript(text.c_str());
                if (!script)
                {
                    g_compiled.erase(expression);
                    return "does not compile";
                }
            }
            NVSEArrayVarInterface::Element result;
            if (!g_script->CallFunction(script, on, nullptr, &result, 0))
                return "could not run it";
            ok = true;
            return FormatValue(result);
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
        g_capturePath = std::string(temp, n) + "FNVVR-Test-Bridge-" + std::to_string(GetCurrentProcessId()) + ".txt";
        return g_console && g_script;
    }

    std::vector<std::string> Execute(const Request& request)
    {
        switch (request.verb)
        {
        case Verb::Invalid:
            return {Err(request.error)};
        case Verb::Ping:
            return {Ok("pong " BRIDGE_VERSION)};
        case Verb::State:
        {
            TESObjectREFR* player = Player();
            ConsoleManager* console = Console();
            char out[96];
            std::snprintf(out, sizeof(out), "world=%d cell=%08X console=%d", WorldLoaded() ? 1 : 0,
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
