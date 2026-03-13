#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"

using namespace OloEngine;

// ============================================================================
// NetworkSession Tests
// ============================================================================

TEST(NetworkSession, CreateSetsModelAndState)
{
    NetworkSession session;
    session.Create(ENetworkModel::ClientServerAuthoritative, "TestGame");

    EXPECT_EQ(session.GetModel(), ENetworkModel::ClientServerAuthoritative);
    EXPECT_EQ(session.GetState(), ESessionState::Lobby);
    EXPECT_EQ(session.GetSessionName(), "TestGame");
    EXPECT_EQ(session.GetPlayerCount(), 0u);
}

TEST(NetworkSession, ResetClearsEverything)
{
    NetworkSession session;
    session.Create(ENetworkModel::Lockstep, "MySession");
    session.AddPlayer(1, "Alice", true);
    session.Reset();

    EXPECT_EQ(session.GetModel(), ENetworkModel::None);
    EXPECT_EQ(session.GetState(), ESessionState::None);
    EXPECT_TRUE(session.GetSessionName().empty());
    EXPECT_EQ(session.GetPlayerCount(), 0u);
}

TEST(NetworkSession, TransitionToChangesState)
{
    NetworkSession session;
    session.Create(ENetworkModel::PeerToPeer, "P2PGame");
    EXPECT_EQ(session.GetState(), ESessionState::Lobby);

    session.TransitionTo(ESessionState::Loading);
    EXPECT_EQ(session.GetState(), ESessionState::Loading);

    session.TransitionTo(ESessionState::InGame);
    EXPECT_EQ(session.GetState(), ESessionState::InGame);

    session.TransitionTo(ESessionState::PostGame);
    EXPECT_EQ(session.GetState(), ESessionState::PostGame);
}

TEST(NetworkSession, AddRemovePlayer)
{
    NetworkSession session;
    session.Create(ENetworkModel::TurnBased, "TurnGame");

    session.AddPlayer(1, "Host", true);
    session.AddPlayer(2, "Client");

    EXPECT_EQ(session.GetPlayerCount(), 2u);

    auto const& players = session.GetPlayers();
    EXPECT_TRUE(players.at(1).IsHost);
    EXPECT_FALSE(players.at(2).IsHost);
    EXPECT_EQ(players.at(1).Name, "Host");
    EXPECT_EQ(players.at(2).Name, "Client");

    session.RemovePlayer(2);
    EXPECT_EQ(session.GetPlayerCount(), 1u);
}

TEST(NetworkSession, AreAllPlayersReady)
{
    NetworkSession session;
    session.Create(ENetworkModel::ClientServerAuthoritative, "ReadyTest");

    // Empty session — not ready
    EXPECT_FALSE(session.AreAllPlayersReady());

    session.AddPlayer(1, "Alice");
    session.AddPlayer(2, "Bob");

    // No one ready
    EXPECT_FALSE(session.AreAllPlayersReady());

    // Partial ready
    session.SetPlayerReady(1, true);
    EXPECT_FALSE(session.AreAllPlayersReady());

    // All ready
    session.SetPlayerReady(2, true);
    EXPECT_TRUE(session.AreAllPlayersReady());

    // Unready one
    session.SetPlayerReady(1, false);
    EXPECT_FALSE(session.AreAllPlayersReady());
}

TEST(NetworkSession, SessionLifecycle)
{
    NetworkSession session;
    session.Create(ENetworkModel::ClientServerAuthoritative, "Lifecycle");
    session.AddPlayer(1, "Host", true);
    session.AddPlayer(2, "Player");
    session.SetPlayerReady(1, true);
    session.SetPlayerReady(2, true);

    EXPECT_TRUE(session.AreAllPlayersReady());

    session.TransitionTo(ESessionState::Loading);
    EXPECT_EQ(session.GetState(), ESessionState::Loading);

    session.TransitionTo(ESessionState::InGame);
    EXPECT_EQ(session.GetState(), ESessionState::InGame);

    session.TransitionTo(ESessionState::PostGame);
    EXPECT_EQ(session.GetState(), ESessionState::PostGame);

    // Players can leave in post-game
    session.RemovePlayer(2);
    EXPECT_EQ(session.GetPlayerCount(), 1u);
}

// ============================================================================
// NetworkLobby Tests
// ============================================================================

TEST(NetworkLobby, CreateLobby)
{
    NetworkLobby lobby;
    lobby.CreateLobby("My Lobby", 27015, 4);

    EXPECT_TRUE(lobby.IsHosting());
    EXPECT_TRUE(lobby.IsInLobby());
    EXPECT_FALSE(lobby.IsReady());
    EXPECT_EQ(lobby.GetLobbyName(), "My Lobby");
    EXPECT_EQ(lobby.GetPort(), 27015);
    EXPECT_EQ(lobby.GetMaxPlayers(), 4u);
}

TEST(NetworkLobby, CloseLobby)
{
    NetworkLobby lobby;
    lobby.CreateLobby("ToClose", 27015);
    lobby.CloseLobby();

    EXPECT_FALSE(lobby.IsHosting());
    EXPECT_FALSE(lobby.IsInLobby());
    EXPECT_TRUE(lobby.GetLobbyName().empty());
}

TEST(NetworkLobby, JoinLobbyFailsIfAlreadyInLobby)
{
    NetworkLobby lobby;
    lobby.CreateLobby("Already In", 27015);

    LobbyInfo info;
    info.Name = "Other";
    info.HostAddress = "127.0.0.1";
    info.HostPort = 27016;

    EXPECT_FALSE(lobby.JoinLobby(info));
}

TEST(NetworkLobby, JoinLobbySucceeds)
{
    NetworkLobby lobby;

    LobbyInfo info;
    info.Name = "RemoteLobby";
    info.HostAddress = "192.168.1.10";
    info.HostPort = 27020;
    info.MaxPlayers = 6;

    EXPECT_TRUE(lobby.JoinLobby(info));
    EXPECT_TRUE(lobby.IsInLobby());
    EXPECT_FALSE(lobby.IsHosting());
    EXPECT_EQ(lobby.GetLobbyName(), "RemoteLobby");
    EXPECT_EQ(lobby.GetPort(), 27020);
    EXPECT_EQ(lobby.GetMaxPlayers(), 6u);
}

TEST(NetworkLobby, LeaveLobby)
{
    NetworkLobby lobby;
    LobbyInfo info;
    info.Name = "ToLeave";
    info.HostPort = 27015;
    lobby.JoinLobby(info);
    lobby.LeaveLobby();

    EXPECT_FALSE(lobby.IsInLobby());
    EXPECT_FALSE(lobby.IsHosting());
    EXPECT_TRUE(lobby.GetLobbyName().empty());
}

TEST(NetworkLobby, ReadyState)
{
    NetworkLobby lobby;
    lobby.CreateLobby("Ready Test", 27015);

    EXPECT_FALSE(lobby.IsReady());
    lobby.SetReady(true);
    EXPECT_TRUE(lobby.IsReady());
    lobby.SetReady(false);
    EXPECT_FALSE(lobby.IsReady());
}

TEST(NetworkLobby, FindLobbiesReturnsEmptyWhenNoHosts)
{
    NetworkLobby lobby;
    bool called = false;
    lobby.FindLobbies(
        [&](const std::vector<LobbyInfo>& results)
        {
            called = true;
            // In unit tests, Winsock may or may not be initialised.
            // Either way, with no hosts running, results should be empty.
            EXPECT_TRUE(results.empty());
        });
    EXPECT_TRUE(called);
}

TEST(NetworkLobby, FindLobbiesNullCallbackDoesNotCrash)
{
    NetworkLobby lobby;
    // Should not crash with nullptr callback
    lobby.FindLobbies(nullptr);
}

TEST(NetworkLobby, DiscoveryPortConstant)
{
    // Verify the well-known discovery port is set
    EXPECT_EQ(OloEngine::NetworkLobby::kDiscoveryPort, 27099);
}

TEST(NetworkLobby, PollDiscoveryWithoutHostingIsNoOp)
{
    NetworkLobby lobby;
    // Not hosting — should not crash
    lobby.PollDiscovery();
}

TEST(NetworkLobby, CreateLobbyAndPollDiscoveryDoesNotCrash)
{
    NetworkLobby lobby;
    lobby.CreateLobby("Test", 27015, 4);
    // In unit tests, the discovery socket may fail to open.
    // PollDiscovery should handle this gracefully.
    lobby.PollDiscovery();
    lobby.CloseLobby();
}

TEST(NetworkLobby, CloseLobbyClosesDiscoverySocket)
{
    NetworkLobby lobby;
    lobby.CreateLobby("Test", 27015);
    lobby.CloseLobby();
    // After closing, PollDiscovery should be a no-op
    lobby.PollDiscovery();
    EXPECT_FALSE(lobby.IsHosting());
}
