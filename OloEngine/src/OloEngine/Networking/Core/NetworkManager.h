#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @class NetworkManager
    // @brief Central networking subsystem following the engine's static Init/Shutdown pattern.
    //
    // Wraps GameNetworkingSockets (GNS) initialization and exposes high-level connection
    // helpers used by NetworkServer, NetworkClient, and the scripting layer.
    //
    // Usage:
    //   NetworkManager::Init();          // called from Application constructor
    //   NetworkManager::StartServer(7777);
    //   NetworkManager::Shutdown();      // called from Application destructor
    class NetworkManager
    {
      public:
        static bool Init();
        static void Shutdown();

        [[nodiscard]] static bool IsInitialized();

        // --- High-level connection API (used by scripting layer) ---

        static bool StartServer(u16 port);
        static void StopServer();

        static bool Connect(const std::string& address, u16 port);
        static void Disconnect();

        [[nodiscard]] static bool IsServer();
        [[nodiscard]] static bool IsClient();
        [[nodiscard]] static bool IsConnected();

      private:
        static bool s_Initialized;
        static bool s_IsServer;
        static bool s_IsConnected;
    };

} // namespace OloEngine
