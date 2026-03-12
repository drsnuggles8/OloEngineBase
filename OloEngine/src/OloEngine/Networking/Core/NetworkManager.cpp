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

        // NetworkManager::StartServer sets the mode flag and tracks the port.
        // The actual listen socket lifecycle is managed by NetworkServer, which is
        // owned by the application layer (game code or editor).
        s_IsServer = true;
        OLO_CORE_TRACE("[NetworkManager] Server mode enabled for port {}.", port);
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

        // NetworkManager::Connect sets the client mode flag and tracks the target.
        // The actual connection lifecycle is managed by NetworkClient, which is
        // owned by the application layer (game code or editor).
        s_IsConnected = true;
        s_IsServer    = false;
        OLO_CORE_TRACE("[NetworkManager] Client mode enabled: {}:{}.", address, port);
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
