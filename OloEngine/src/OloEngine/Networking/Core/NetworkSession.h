#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/NetworkModel.h"

#include <string>
#include <unordered_map>

namespace OloEngine
{
    // Session lifecycle states.
    enum class ESessionState : u8
    {
        None = 0,
        Lobby,   // Pre-game: players joining, ready-up
        Loading, // Transitioning to gameplay (loading assets, spawning)
        InGame,  // Active gameplay
        PostGame // Results screen, returning to lobby
    };

    // Represents a player in the session.
    struct SessionPlayer
    {
        u32 ClientID = 0;
        std::string Name;
        bool IsReady = false;
        bool IsHost = false;
    };

    // Encapsulates a networked game session with a specific networking model,
    // player list, and state machine.
    class NetworkSession
    {
      public:
        NetworkSession();

        // Create a new session with the given model.
        void Create(ENetworkModel model, const std::string& sessionName);

        // Reset the session to its initial state.
        void Reset();

        // Transition to the next state.
        void TransitionTo(ESessionState newState);

        // Add a player to the session.
        void AddPlayer(u32 clientID, const std::string& name, bool isHost = false);

        // Remove a player from the session.
        void RemovePlayer(u32 clientID);

        // Set a player's ready state.
        void SetPlayerReady(u32 clientID, bool ready);

        // Check if all players are ready.
        [[nodiscard]] bool AreAllPlayersReady() const;

        // Get session info
        [[nodiscard]] ENetworkModel GetModel() const;
        [[nodiscard]] ESessionState GetState() const;
        [[nodiscard]] const std::string& GetSessionName() const;
        [[nodiscard]] const std::unordered_map<u32, SessionPlayer>& GetPlayers() const;
        [[nodiscard]] u32 GetPlayerCount() const;

      private:
        ENetworkModel m_Model = ENetworkModel::None;
        ESessionState m_State = ESessionState::None;
        std::string m_SessionName;
        std::unordered_map<u32, SessionPlayer> m_Players;
    };
} // namespace OloEngine
