#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    NetworkSession::NetworkSession() = default;

    void NetworkSession::Create(ENetworkModel model, const std::string& sessionName)
    {
        m_Model = model;
        m_SessionName = sessionName;
        m_State = ESessionState::Lobby;
        m_Players.clear();
    }

    void NetworkSession::Reset()
    {
        m_Model = ENetworkModel::None;
        m_State = ESessionState::None;
        m_SessionName.clear();
        m_Players.clear();
    }

    void NetworkSession::TransitionTo(ESessionState newState)
    {
        // Validate state transitions
        bool valid = false;
        switch (m_State)
        {
            case ESessionState::None:
                valid = (newState == ESessionState::Lobby);
                break;
            case ESessionState::Lobby:
                valid = (newState == ESessionState::Loading || newState == ESessionState::None);
                break;
            case ESessionState::Loading:
                valid = (newState == ESessionState::InGame || newState == ESessionState::Lobby);
                break;
            case ESessionState::InGame:
                valid = (newState == ESessionState::PostGame || newState == ESessionState::None);
                break;
            case ESessionState::PostGame:
                valid = (newState == ESessionState::Lobby || newState == ESessionState::None);
                break;
        }

        if (!valid)
        {
            OLO_CORE_WARN("[NetworkSession] Invalid state transition from {} to {}",
                          static_cast<int>(m_State), static_cast<int>(newState));
            return;
        }

        m_State = newState;
    }

    void NetworkSession::AddPlayer(u32 clientID, const std::string& name, bool isHost)
    {
        SessionPlayer player;
        player.ClientID = clientID;
        player.Name = name;
        player.IsReady = false;
        player.IsHost = isHost;
        m_Players[clientID] = player;
    }

    void NetworkSession::RemovePlayer(u32 clientID)
    {
        m_Players.erase(clientID);
    }

    void NetworkSession::SetPlayerReady(u32 clientID, bool ready)
    {
        if (auto it = m_Players.find(clientID); it != m_Players.end())
        {
            it->second.IsReady = ready;
        }
    }

    bool NetworkSession::AreAllPlayersReady() const
    {
        if (m_Players.empty())
        {
            return false;
        }
        for (const auto& [id, player] : m_Players)
        {
            if (!player.IsReady)
            {
                return false;
            }
        }
        return true;
    }

    ENetworkModel NetworkSession::GetModel() const
    {
        return m_Model;
    }

    ESessionState NetworkSession::GetState() const
    {
        return m_State;
    }

    const std::string& NetworkSession::GetSessionName() const
    {
        return m_SessionName;
    }

    const std::unordered_map<u32, SessionPlayer>& NetworkSession::GetPlayers() const
    {
        return m_Players;
    }

    u32 NetworkSession::GetPlayerCount() const
    {
        return static_cast<u32>(m_Players.size());
    }
} // namespace OloEngine
