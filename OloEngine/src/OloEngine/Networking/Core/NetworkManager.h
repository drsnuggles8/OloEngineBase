#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

struct SteamNetConnectionStatusChangedCallback_t;

namespace OloEngine
{
    class NetworkServer;
    class NetworkClient;

    class NetworkManager
    {
      public:
        static bool Init();
        static void Shutdown();

        [[nodiscard]] static bool IsInitialized();

        // Server API
        static bool StartServer(u16 port);
        static void StopServer();
        [[nodiscard]] static bool IsServer();

        // Client API
        static bool Connect(const std::string& address, u16 port);
        static void Disconnect();
        [[nodiscard]] static bool IsClient();
        [[nodiscard]] static bool IsConnected();

        // Connection status callback (called by GNS)
        static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

      private:
        static bool s_Initialized;
        static Scope<NetworkServer> s_Server;
        static Scope<NetworkClient> s_Client;
    };
} // namespace OloEngine
