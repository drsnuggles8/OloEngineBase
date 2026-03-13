#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Serialization/Archive.h"

#include <chrono>
#include <thread>
#include <atomic>

using namespace OloEngine;

// Helper: poll until a predicate is true, or timeout
static bool WaitUntil(std::function<bool()> predicate, i32 timeoutMs = 3000)
{
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

class NetworkIntegrationTest : public ::testing::Test
{
  protected:
    static constexpr u16 kTestPort = 27099;

    void SetUp() override
    {
        ASSERT_TRUE(NetworkManager::Init());
    }

    void TearDown() override
    {
        NetworkManager::Shutdown();
        // Brief pause to let the OS release the socket
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};

TEST_F(NetworkIntegrationTest, ServerStartStop)
{
    EXPECT_TRUE(NetworkManager::StartServer(kTestPort));
    EXPECT_TRUE(NetworkManager::IsServer());

    NetworkManager::StopServer();
    EXPECT_FALSE(NetworkManager::IsServer());
}

TEST_F(NetworkIntegrationTest, ClientConnectDisconnect)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));

    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    EXPECT_TRUE(NetworkManager::IsClient());

    // Wait for GNS callback to establish the connection
    bool connected = WaitUntil([] { return NetworkManager::IsConnected(); });
    EXPECT_TRUE(connected) << "Client failed to connect within timeout";

    NetworkManager::Disconnect();
    EXPECT_FALSE(NetworkManager::IsConnected());

    NetworkManager::StopServer();
}

TEST_F(NetworkIntegrationTest, ServerTracksConnectedClient)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));
    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    ASSERT_TRUE(WaitUntil([] { return NetworkManager::IsConnected(); }));

    auto const* server = NetworkManager::GetServer();
    ASSERT_NE(server, nullptr);

    // Server should see at least 1 connection
    bool hasClient = WaitUntil(
        [server]
        {
            for (auto const& [h, conn] : server->GetConnections())
            {
                if (conn.GetState() == EConnectionState::Connected)
                {
                    return true;
                }
            }
            return false;
        });
    EXPECT_TRUE(hasClient) << "Server did not register the client connection";

    NetworkManager::Disconnect();
    NetworkManager::StopServer();
}

TEST_F(NetworkIntegrationTest, ClientToServerMessage)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));
    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    ASSERT_TRUE(WaitUntil([] { return NetworkManager::IsConnected(); }));

    // Register handler on the server for a user-defined message
    std::atomic<bool> received{false};
    u32 receivedValue = 0;

    NetworkManager::GetServerDispatcher().RegisterHandler(
        ENetworkMessageType::UserMessage,
        [&](u32 /*sender*/, const u8* data, u32 size)
        {
            if (size >= sizeof(u32))
            {
                std::memcpy(&receivedValue, data, sizeof(u32));
            }
            received.store(true, std::memory_order_release);
        });

    // Client sends a message with a u32 payload
    u32 payload = 12345;
    auto* client = NetworkManager::GetClient();
    ASSERT_NE(client, nullptr);
    client->SendMessage(ENetworkMessageType::UserMessage, reinterpret_cast<const u8*>(&payload), sizeof(payload),
                        k_nSteamNetworkingSend_Reliable);

    // Wait briefly for network delivery, then poll server messages
    bool gotIt = WaitUntil(
        [&]
        {
            NetworkManager::GetServer()->PollMessages();
            return received.load(std::memory_order_acquire);
        });

    EXPECT_TRUE(gotIt) << "Server did not receive client message within timeout";
    EXPECT_EQ(receivedValue, 12345u);

    NetworkManager::Disconnect();
    NetworkManager::StopServer();
}

TEST_F(NetworkIntegrationTest, ServerToClientMessage)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));
    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    ASSERT_TRUE(WaitUntil([] { return NetworkManager::IsConnected(); }));

    // Wait for server to see the connected client
    auto* server = NetworkManager::GetServer();
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(WaitUntil(
        [server]
        {
            for (auto const& [h, conn] : server->GetConnections())
            {
                if (conn.GetState() == EConnectionState::Connected)
                {
                    return true;
                }
            }
            return false;
        }));

    // Register handler on the client
    std::atomic<bool> received{false};
    u32 receivedValue = 0;

    NetworkManager::GetClientDispatcher().RegisterHandler(
        ENetworkMessageType::UserMessage,
        [&](u32 /*sender*/, const u8* data, u32 size)
        {
            if (size >= sizeof(u32))
            {
                std::memcpy(&receivedValue, data, sizeof(u32));
            }
            received.store(true, std::memory_order_release);
        });

    // Server broadcasts a message
    u32 payload = 99999;
    server->BroadcastMessage(ENetworkMessageType::UserMessage, reinterpret_cast<const u8*>(&payload), sizeof(payload),
                             k_nSteamNetworkingSend_Reliable);

    // Poll client messages
    auto* client = NetworkManager::GetClient();
    bool gotIt = WaitUntil(
        [&]
        {
            client->PollMessages();
            return received.load(std::memory_order_acquire);
        });

    EXPECT_TRUE(gotIt) << "Client did not receive server message within timeout";
    EXPECT_EQ(receivedValue, 99999u);

    NetworkManager::Disconnect();
    NetworkManager::StopServer();
}

TEST_F(NetworkIntegrationTest, PingPongBuiltin)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));
    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    ASSERT_TRUE(WaitUntil([] { return NetworkManager::IsConnected(); }));

    // Register a Pong handler on the client — server should reply with Pong when it receives Ping
    std::atomic<bool> pongReceived{false};

    NetworkManager::GetClientDispatcher().RegisterHandler(
        ENetworkMessageType::Pong,
        [&](u32, const u8*, u32) { pongReceived.store(true, std::memory_order_release); });

    // Client sends a Ping message to the server
    auto* client = NetworkManager::GetClient();
    ASSERT_NE(client, nullptr);
    client->SendMessage(ENetworkMessageType::Ping, nullptr, 0, k_nSteamNetworkingSend_Reliable);

    // Poll both sides to process the Ping and Pong
    auto* server = NetworkManager::GetServer();
    bool gotPong = WaitUntil(
        [&]
        {
            server->PollMessages();
            client->PollMessages();
            return pongReceived.load(std::memory_order_acquire);
        });

    EXPECT_TRUE(gotPong) << "Client did not receive Pong reply within timeout";

    NetworkManager::Disconnect();
    NetworkManager::StopServer();
}

TEST_F(NetworkIntegrationTest, StatsTrackSentAndReceived)
{
    ASSERT_TRUE(NetworkManager::StartServer(kTestPort));
    ASSERT_TRUE(NetworkManager::Connect("127.0.0.1", kTestPort));
    ASSERT_TRUE(WaitUntil([] { return NetworkManager::IsConnected(); }));

    auto* client = NetworkManager::GetClient();
    auto* server = NetworkManager::GetServer();
    ASSERT_NE(client, nullptr);
    ASSERT_NE(server, nullptr);

    // Wait for server to see connected client
    ASSERT_TRUE(WaitUntil(
        [server]
        {
            for (auto const& [h, conn] : server->GetConnections())
            {
                if (conn.GetState() == EConnectionState::Connected)
                {
                    return true;
                }
            }
            return false;
        }));

    // Send a message from client
    u32 payload = 42;
    client->SendMessage(ENetworkMessageType::UserMessage, reinterpret_cast<const u8*>(&payload), sizeof(payload),
                        k_nSteamNetworkingSend_Reliable);

    // Client stats should reflect the send
    auto const& clientStats = client->GetStats();
    EXPECT_GE(clientStats.TotalMessagesSent, 1u);
    EXPECT_GT(clientStats.TotalBytesSent, 0u);

    // Poll server and check receive stats
    WaitUntil(
        [&]
        {
            server->PollMessages();
            return server->GetStats().TotalMessagesReceived > 0;
        });

    auto const& serverStats = server->GetStats();
    EXPECT_GE(serverStats.TotalMessagesReceived, 1u);
    EXPECT_GT(serverStats.TotalBytesReceived, 0u);

    NetworkManager::Disconnect();
    NetworkManager::StopServer();
}
