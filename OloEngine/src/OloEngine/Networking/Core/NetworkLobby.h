#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    // Info about a discoverable LAN lobby.
    struct LobbyInfo
    {
        std::string Name;
        std::string HostAddress;
        u16 HostPort = 0;
        u32 PlayerCount = 0;
        u32 MaxPlayers = 0;
    };

    // Simple LAN lobby manager.
    // CreateLobby hosts a lobby, FindLobbies scans for them (via future UDP broadcast),
    // JoinLobby connects to a discovered lobby.
    class NetworkLobby
    {
      public:
        NetworkLobby();

        // Host a new lobby.
        void CreateLobby(const std::string& name, u16 port, u32 maxPlayers = 8);

        // Stop hosting the current lobby.
        void CloseLobby();

        // Discover lobbies on LAN (stub — real implementation would use
        // UDP broadcast on a well-known port).
        void FindLobbies(std::function<void(const std::vector<LobbyInfo>&)> callback);

        // Request to join a discovered lobby.  Returns true if the join
        // request was initiated (actual confirmation is async via server).
        bool JoinLobby(const LobbyInfo& lobby);

        // Leave the lobby we joined.
        void LeaveLobby();

        // Set our ready state in the current lobby.
        void SetReady(bool ready);

        // Query
        [[nodiscard]] bool IsHosting() const;
        [[nodiscard]] bool IsInLobby() const;
        [[nodiscard]] bool IsReady() const;
        [[nodiscard]] const std::string& GetLobbyName() const;
        [[nodiscard]] u32 GetMaxPlayers() const;
        [[nodiscard]] u16 GetPort() const;

      private:
        bool m_Hosting = false;
        bool m_InLobby = false;
        bool m_Ready = false;
        std::string m_LobbyName;
        u16 m_Port = 0;
        u32 m_MaxPlayers = 0;
    };
} // namespace OloEngine
