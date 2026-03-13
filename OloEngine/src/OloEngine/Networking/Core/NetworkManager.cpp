#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkThread.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    bool NetworkManager::s_Initialized = false;
    Scope<NetworkServer> NetworkManager::s_Server = nullptr;
    Scope<NetworkClient> NetworkManager::s_Client = nullptr;

    static void GNSDebugOutput(ESteamNetworkingSocketsDebugOutputType eType, char const* pszMsg)
    {
        switch (eType)
        {
            case k_ESteamNetworkingSocketsDebugOutputType_Bug:
            case k_ESteamNetworkingSocketsDebugOutputType_Error:
                OLO_CORE_ERROR("[GNS] {}", pszMsg);
                break;
            case k_ESteamNetworkingSocketsDebugOutputType_Important:
            case k_ESteamNetworkingSocketsDebugOutputType_Warning:
                OLO_CORE_WARN("[GNS] {}", pszMsg);
                break;
            default:
                OLO_CORE_TRACE("[GNS] {}", pszMsg);
                break;
        }
    }

    static void GNSConnectionStatusCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        NetworkManager::OnConnectionStatusChanged(pInfo);
    }

    bool NetworkManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized)
        {
            OLO_CORE_WARN("NetworkManager::Init() called when already initialized");
            return true;
        }

        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            OLO_CORE_ERROR("GameNetworkingSockets_Init failed: {}", errMsg);
            return false;
        }

        SteamNetworkingUtils()->SetDebugOutputFunction(
            k_ESteamNetworkingSocketsDebugOutputType_Msg,
            GNSDebugOutput);

        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            GNSConnectionStatusCallback);

        NetworkThread::Start(60);

        s_Initialized = true;
        OLO_CORE_INFO("NetworkManager initialized (GameNetworkingSockets)");
        return true;
    }

    void NetworkManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            return;
        }

        StopServer();
        Disconnect();
        NetworkThread::Stop();

        GameNetworkingSockets_Kill();
        s_Initialized = false;
        OLO_CORE_INFO("NetworkManager shut down");
    }

    bool NetworkManager::IsInitialized()
    {
        return s_Initialized;
    }

    bool NetworkManager::StartServer(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("NetworkManager not initialized");
            return false;
        }

        if (s_Server)
        {
            OLO_CORE_WARN("Server already running");
            return false;
        }

        s_Server = CreateScope<NetworkServer>();
        if (!s_Server->Start(port))
        {
            s_Server.reset();
            return false;
        }

        return true;
    }

    void NetworkManager::StopServer()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Server)
        {
            s_Server->Stop();
            s_Server.reset();
        }
    }

    bool NetworkManager::IsServer()
    {
        return s_Server != nullptr && s_Server->IsRunning();
    }

    bool NetworkManager::Connect(const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("NetworkManager not initialized");
            return false;
        }

        if (s_Client)
        {
            OLO_CORE_WARN("Already connected or connecting");
            return false;
        }

        s_Client = CreateScope<NetworkClient>();
        if (!s_Client->Connect(address, port))
        {
            s_Client.reset();
            return false;
        }

        return true;
    }

    void NetworkManager::Disconnect()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Client)
        {
            s_Client->Disconnect();
            s_Client.reset();
        }
    }

    bool NetworkManager::IsClient()
    {
        return s_Client != nullptr;
    }

    bool NetworkManager::IsConnected()
    {
        return s_Client != nullptr && s_Client->IsConnected();
    }

    void NetworkManager::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        if (s_Server)
        {
            s_Server->OnConnectionStatusChanged(pInfo);
        }

        if (s_Client)
        {
            s_Client->OnConnectionStatusChanged(pInfo);
        }
    }
} // namespace OloEngine
