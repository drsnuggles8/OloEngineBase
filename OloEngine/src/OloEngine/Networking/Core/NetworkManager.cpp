#include "OloEnginePCH.h"
#include "NetworkManager.h"
#include "OloEngine/Debug/Profiler.h"

// GameNetworkingSockets
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    bool NetworkManager::s_Initialized = false;
    bool NetworkManager::s_IsServer    = false;
    bool NetworkManager::s_IsConnected = false;

    // -------------------------------------------------------------------------
    // GNS debug output callback
    // -------------------------------------------------------------------------
    static void GNSDebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg)
    {
        switch (eType)
        {
            case k_ESteamNetworkingSocketsDebugOutputType_Bug:
            case k_ESteamNetworkingSocketsDebugOutputType_Error:
                OLO_CORE_ERROR("[NetworkManager] {}", pszMsg);
                break;
            case k_ESteamNetworkingSocketsDebugOutputType_Important:
            case k_ESteamNetworkingSocketsDebugOutputType_Warning:
                OLO_CORE_WARN("[NetworkManager] {}", pszMsg);
                break;
            case k_ESteamNetworkingSocketsDebugOutputType_Msg:
                OLO_CORE_TRACE("[NetworkManager] {}", pszMsg);
                break;
            case k_ESteamNetworkingSocketsDebugOutputType_Verbose:
            case k_ESteamNetworkingSocketsDebugOutputType_Debug:
            case k_ESteamNetworkingSocketsDebugOutputType_Everything:
                // Suppress overly verbose output by default
                break;
            default:
                OLO_CORE_TRACE("[NetworkManager] {}", pszMsg);
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Init / Shutdown
    // -------------------------------------------------------------------------

    bool NetworkManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized)
        {
            OLO_CORE_WARN("[NetworkManager] Already initialized.");
            return true;
        }

        OLO_CORE_TRACE("[NetworkManager] Initializing GameNetworkingSockets.");

        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            OLO_CORE_ERROR("[NetworkManager] GameNetworkingSockets_Init failed: {}", errMsg);
            return false;
        }

        // Register the debug output callback
        SteamNetworkingUtils()->SetDebugOutputFunction(
            k_ESteamNetworkingSocketsDebugOutputType_Msg, GNSDebugOutput);

        s_Initialized = true;
        OLO_CORE_TRACE("[NetworkManager] Initialized successfully.");
        return true;
    }

    void NetworkManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            return;
        }

        OLO_CORE_TRACE("[NetworkManager] Shutting down.");

        // Disconnect/stop any active session before killing the library
        if (s_IsConnected || s_IsServer)
        {
            Disconnect();
            StopServer();
        }

        GameNetworkingSockets_Kill();

        s_Initialized = false;
        s_IsServer    = false;
        s_IsConnected = false;

        OLO_CORE_TRACE("[NetworkManager] Shutdown complete.");
    }

    bool NetworkManager::IsInitialized()
    {
        return s_Initialized;
    }

    // -------------------------------------------------------------------------
    // High-level connection API
    // -------------------------------------------------------------------------

    bool NetworkManager::StartServer(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("[NetworkManager] StartServer called before Init.");
            return false;
        }

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (!pInterface)
        {
            OLO_CORE_ERROR("[NetworkManager] SteamNetworkingSockets() returned null.");
            return false;
        }

        SteamNetworkingIPAddr serverLocalAddr;
        serverLocalAddr.Clear();
        serverLocalAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        HSteamListenSocket listenSocket = pInterface->CreateListenSocketIP(serverLocalAddr, 1, &opt);
        if (listenSocket == k_HSteamListenSocket_Invalid)
        {
            OLO_CORE_ERROR("[NetworkManager] Failed to create listen socket on port {}.", port);
            return false;
        }

        // Close immediately — the full lifecycle is managed by NetworkServer.
        // This call only validates that GNS is operational.
        pInterface->CloseListenSocket(listenSocket);

        s_IsServer = true;
        OLO_CORE_TRACE("[NetworkManager] Server mode active on port {}.", port);
        return true;
    }

    void NetworkManager::StopServer()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_IsServer)
        {
            return;
        }

        s_IsServer    = false;
        s_IsConnected = false;
        OLO_CORE_TRACE("[NetworkManager] Server stopped.");
    }

    bool NetworkManager::Connect(const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("[NetworkManager] Connect called before Init.");
            return false;
        }

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (!pInterface)
        {
            OLO_CORE_ERROR("[NetworkManager] SteamNetworkingSockets() returned null.");
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        serverAddr.ParseString(address.c_str());
        serverAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        HSteamNetConnection conn = pInterface->ConnectByIPAddress(serverAddr, 1, &opt);
        if (conn == k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_ERROR("[NetworkManager] Failed to initiate connection to {}:{}.", address, port);
            return false;
        }

        // Close immediately — the full lifecycle is managed by NetworkClient.
        // This call only validates that GNS is operational.
        pInterface->CloseConnection(conn, 0, "Validation only", false);

        s_IsConnected = true;
        s_IsServer    = false;
        OLO_CORE_TRACE("[NetworkManager] Client mode: connecting to {}:{}.", address, port);
        return true;
    }

    void NetworkManager::Disconnect()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_IsConnected)
        {
            return;
        }

        s_IsConnected = false;
        OLO_CORE_TRACE("[NetworkManager] Disconnected.");
    }

    bool NetworkManager::IsServer()
    {
        return s_Initialized && s_IsServer;
    }

    bool NetworkManager::IsClient()
    {
        return s_Initialized && !s_IsServer;
    }

    bool NetworkManager::IsConnected()
    {
        return s_Initialized && s_IsConnected;
    }

} // namespace OloEngine
