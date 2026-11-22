// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
//
// xNVSE plugin entry. Off unless Data\NVSE\Plugins\FNVVR-Test-Bridge.ini says Enabled=1; when off it does not
// even listen to the game loop, so it costs nothing while playing.

#include "game.h"
#include "log.h"
#include "pipe_server.h"
#include "protocol.h"

#include "nvse/PluginAPI.h"

#include <string>

#include <windows.h>

namespace
{
    constexpr int kJobsPerFrame = 8;

    PluginHandle g_handle = static_cast<PluginHandle>(kPluginHandle_Invalid);
    bridge::PipeServer g_pipe;
    uint64_t g_frame = 0;                       // game frames since the plugin loaded
    std::shared_ptr<bridge::Job> g_waiting;     // a "wait"/"waitframe" until g_waitUntil
    uint64_t g_waitUntil = 0;

    std::string FrameAnswer()
    {
        return bridge::Ok("frame=" + std::to_string(g_frame));
    }

    std::string ReadIni(const std::string& path, const char* key, const char* fallback)
    {
        char value[256];
        GetPrivateProfileStringA("Bridge", key, fallback, value, sizeof(value), path.c_str());
        return value;
    }

    void OnMessage(NVSEMessagingInterface::Message* message)
    {
        switch (message->type)
        {
        case NVSEMessagingInterface::kMessage_MainGameLoop:
            ++g_frame;
            if (g_waiting && g_frame >= g_waitUntil)
            {
                g_waiting->reply.set_value({FrameAnswer()});
                g_waiting = nullptr;
            }
            for (int i = 0; i < kJobsPerFrame && !g_waiting; ++i)
            {
                std::shared_ptr<bridge::Job> job = g_pipe.Poll();
                if (!job)
                    break;
                bridge::Request request = bridge::Parse(job->line);
                if (request.verb == bridge::Verb::Wait || request.verb == bridge::Verb::WaitFrame)
                {
                    g_waitUntil = request.verb == bridge::Verb::Wait ? g_frame + request.count : request.count;
                    if (g_frame >= g_waitUntil)
                        job->reply.set_value({FrameAnswer()});
                    else
                        g_waiting = job;
                    continue;
                }
                job->reply.set_value(bridge::Execute(request, g_frame));
            }
            break;
        case NVSEMessagingInterface::kMessage_ExitGame:
        case NVSEMessagingInterface::kMessage_ExitGame_Console:
            g_pipe.Stop();
            break;
        default:
            break;
        }
    }
}

extern "C"
{
    bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "FNVVR-Test-Bridge";
        info->version = BRIDGE_VERSION_NUMBER;
        return !nvse->isEditor && nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;
    }

    bool NVSEPlugin_Load(NVSEInterface* nvse)
    {
        g_handle = nvse->GetPluginHandle();
        std::string root = nvse->GetRuntimeDirectory();
        std::string ini = root + "Data\\NVSE\\Plugins\\FNVVR-Test-Bridge.ini";
        if (ReadIni(ini, "Enabled", "0") != "1")
            return true;  // installed but off: no log, no pipe, no game-loop listener

        bridge::OpenLog(root + "FNVVR-Test-Bridge.log");
        bridge::Log("FNVVR-Test-Bridge " BRIDGE_VERSION " loaded (xNVSE %08X)", nvse->nvseVersion);
        if (!bridge::InitGame(nvse))
        {
            bridge::Log("xNVSE lacks the console or script interface; off");
            return true;
        }
        auto* messaging = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
        if (!messaging || !messaging->RegisterListener(g_handle, "NVSE", OnMessage))
        {
            bridge::Log("could not listen to xNVSE messages; off");
            return true;
        }
        std::string name = ReadIni(ini, "PipeName", "FNVVR-Test-Bridge");
        std::wstring path = g_pipe.Start(std::wstring(name.begin(), name.end()));
        if (!path.empty())
            bridge::Log("listening on %ls (this Windows user, this PC only)", path.c_str());
        return true;
    }
}
