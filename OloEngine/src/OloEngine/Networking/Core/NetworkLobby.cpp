#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    NetworkLobby::NetworkLobby() = default;

    void NetworkLobby::CreateLobby(const std::string& name, u16 port, u32 maxPlayers)
    {
        m_LobbyName = name;
        m_Port = port;
        m_MaxPlayers = maxPlayers;
        m_Hosting = true;
        m_InLobby = true;
        m_Ready = false;
    }

    void NetworkLobby::CloseLobby()
    {
        m_Hosting = false;
        m_InLobby = false;
        m_Ready = false;
        m_LobbyName.clear();
        m_Port = 0;
        m_MaxPlayers = 0;
    }

    void NetworkLobby::FindLobbies(std::function<void(const std::vector<LobbyInfo>&)> callback)
    {
        // Stub: real implementation would broadcast a discovery
        // packet on LAN and collect responses.  For now, return
        // an empty result set so callers can build UI without
        // the transport layer being wired up.
        OLO_CORE_WARN("[NetworkLobby] FindLobbies is a stub — no LAN discovery implemented yet");
        if (callback)
        {
            callback({});
        }
    }

    bool NetworkLobby::JoinLobby(const LobbyInfo& lobby)
    {
        if (m_InLobby)
        {
            return false;
        }
        m_LobbyName = lobby.Name;
        m_Port = lobby.HostPort;
        m_MaxPlayers = lobby.MaxPlayers;
        m_InLobby = true;
        m_Hosting = false;
        m_Ready = false;
        return true;
    }

    void NetworkLobby::LeaveLobby()
    {
        m_InLobby = false;
        m_Hosting = false;
        m_Ready = false;
        m_LobbyName.clear();
        m_Port = 0;
        m_MaxPlayers = 0;
    }

    void NetworkLobby::SetReady(bool ready)
    {
        m_Ready = ready;
    }

    bool NetworkLobby::IsHosting() const
    {
        return m_Hosting;
    }

    bool NetworkLobby::IsInLobby() const
    {
        return m_InLobby;
    }

    bool NetworkLobby::IsReady() const
    {
        return m_Ready;
    }

    const std::string& NetworkLobby::GetLobbyName() const
    {
        return m_LobbyName;
    }

    u32 NetworkLobby::GetMaxPlayers() const
    {
        return m_MaxPlayers;
    }

    u16 NetworkLobby::GetPort() const
    {
        return m_Port;
    }
} // namespace OloEngine
