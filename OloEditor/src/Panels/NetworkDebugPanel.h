#pragma once

#include "OloEngine/Networking/Core/NetworkManager.h"

namespace OloEngine
{
    // @class NetworkDebugPanel
    // @brief ImGui panel showing networking status, peer list, and statistics.
    //
    // Provides:
    //   - Connection state (Disconnected / Connecting / Connected / Server Listening)
    //   - Local mode (Server / Client / None)
    //   - Buttons: Start Server, Connect, Disconnect
    //   - Network statistics placeholder
    class NetworkDebugPanel
    {
      public:
        void OnImGuiRender();

      private:
        // UI state for server start
        u16  m_ServerPort  = 7777;

        // UI state for client connect
        char m_ConnectAddress[128] = "127.0.0.1";
        u16  m_ConnectPort = 7777;
    };

} // namespace OloEngine
