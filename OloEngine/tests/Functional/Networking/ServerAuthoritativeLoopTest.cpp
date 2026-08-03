#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// ServerAuthoritativeLoopTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Networking × Scene/ECS, driven end-to-end over a REAL GameNetworkingSockets
//   loopback: one server and TWO independent clients, each with its own Scene.
//
// Why two clients, and why real sockets. Every existing networking test exercises
// one primitive in isolation, and the whole stack passed them while the loop was
// never assembled at all — nothing observed a client seeing another client. Two
// clients in one process is only possible because the replication drivers are
// objects rather than statics on the NetworkManager singleton (which can hold one
// server and one client); that testability was a design constraint, not a
// by-product.
//
// What is asserted here is the set of things that fail QUIETLY in a multiplayer
// loop — entities that exist but never move, corrections that never converge,
// authority checks that pass because nothing was ever sent:
//   * a player entity per connection, replicated to BOTH clients;
//   * client input moving its own pawn and converging on the other client;
//   * the server refusing input for an entity the sender does not own;
//   * despawn removing the pawn from the remaining client;
//   * interest scoping actually withholding a distant entity;
//   * lag compensation rewinding to where the client saw the world.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Memory/Platform.h" // OLO_ASAN_ENABLED
#include "OloEngine/Networking/Core/ClientReplicationDriver.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/Core/ServerReplicationDriver.h"
#include "OloEngine/Networking/Prediction/NetworkMovementInput.h"
#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace OloEngine;

namespace
{
    // Distinct from NetworkIntegrationTest's port so the two can never collide if
    // one leaves a socket in TIME_WAIT.
    constexpr u16 kTestPort = 27101;

    // The replication tick is time-driven, so a pump step must advance enough
    // simulated time to actually fire one. 1/20 s at the default 20 Hz.
    constexpr f32 kPumpDt = 1.0f / 20.0f;
} // namespace

// One server + two clients, each with its own Scene, driven synchronously.
//
// GNS delivers connection-status changes through a single process-wide callback,
// which NetworkManager normally owns. This fixture installs its own so it can fan
// the callback out to the three transports it created directly — and, importantly,
// so the whole test runs on ONE thread with no NetworkThread involved. A
// replication loop is hard enough to debug without a race in the harness.
class ServerAuthoritativeLoopTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
#if OLO_ASAN_ENABLED
        // GameNetworkingSockets_Init trips a stack-buffer-overflow inside vendored
        // GNS under MSVC ASan (issue #317), exactly as NetworkManager::Init works
        // around. Skip rather than fail: the transport-free networking tests still
        // run under the sanitizer.
        GTEST_SKIP() << "Live GNS sockets are unavailable under AddressSanitizer (issue #317)";
#else
        SteamDatagramErrMsg errMsg;
        ASSERT_TRUE(GameNetworkingSockets_Init(nullptr, errMsg)) << errMsg;

        s_Active = this;
        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&ServerAuthoritativeLoopTest::OnStatusChanged);

        ComponentReplicator::RegisterDefaults();
        ComponentInterpolationRegistry::RegisterDefaults();
        NetworkSpawnRegistry::RegisterDefaults();
        RpcRegistry::Clear();

        m_ServerScene = CreateScope<Scene>();
        m_ClientScenes[0] = CreateScope<Scene>();
        m_ClientScenes[1] = CreateScope<Scene>();

        m_Server = CreateScope<NetworkServer>();
        ASSERT_TRUE(m_Server->Start(kTestPort));

        // Route the messages the server driver owns. These run synchronously from
        // PollMessages inside Pump(), i.e. on this thread — the same game-thread
        // guarantee the production hosts give.
        m_Server->GetDispatcher().RegisterHandler(
            ENetworkMessageType::InputCommand, [this](u32 sender, const u8* data, u32 size)
            { m_ServerDriver.HandleInputCommand(*m_ServerScene, sender, data, size); });
        m_Server->GetDispatcher().RegisterHandler(
            ENetworkMessageType::RPC, [this](u32 sender, const u8* data, u32 size)
            { m_ServerDriver.HandleRpc(*m_ServerScene, sender, data, size); });
        m_Server->GetDispatcher().RegisterHandler(
            ENetworkMessageType::SnapshotAck, [this](u32 sender, const u8* data, u32 size)
            { m_ServerDriver.HandleSnapshotAck(sender, data, size); });

        // Same apply callback on both sides — the only way prediction and the
        // authoritative simulation can agree without a second implementation.
        auto applyMovement = MakeMovementApplyCallback(/*maxStepDistance*/ 1.0f);
        m_ServerDriver.GetInputHandler().SetInputApplyCallback(applyMovement);
        m_ClientDrivers[0].SetInputApplyCallback(applyMovement);
        m_ClientDrivers[1].SetInputApplyCallback(applyMovement);

        // Snapping rather than smoothing: the smoothing blend is a rendering
        // nicety, and leaving it on would make a convergence assertion a test of
        // how many ticks the harness happened to run.
        m_ClientDrivers[0].GetPrediction().SetSmoothingRate(1.0f);
        m_ClientDrivers[1].GetPrediction().SetSmoothingRate(1.0f);
#endif
    }

    void TearDown() override
    {
#if !OLO_ASAN_ENABLED
        for (auto& client : m_Clients)
        {
            if (client)
            {
                client->Disconnect();
            }
        }
        if (m_Server)
        {
            m_Server->Stop();
        }
        m_Clients = {};
        m_Server.reset();

        s_Active = nullptr;
        RpcRegistry::Clear();
        GameNetworkingSockets_Kill();
        // Give the OS a moment to release the listen socket before the next test
        // in this binary binds the same port.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
    }

    static void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
    {
        if (s_Active == nullptr)
        {
            return;
        }
        if (s_Active->m_Server)
        {
            s_Active->m_Server->OnConnectionStatusChanged(info);
        }
        for (auto& client : s_Active->m_Clients)
        {
            if (client)
            {
                client->OnConnectionStatusChanged(info);
            }
        }
    }

    void ConnectClient(sizet index)
    {
        m_Clients[index] = CreateScope<NetworkClient>();
        ASSERT_TRUE(m_Clients[index]->Connect("127.0.0.1", kTestPort));
        m_ClientDrivers[index].AttachTo(*m_Clients[index], *m_ClientScenes[index]);
    }

    // One synchronous frame for every participant, in the production order:
    // callbacks, then server poll + replication tick, then each client's tick
    // (which polls and interpolates).
    void Pump(f32 dt = kPumpDt)
    {
        if (ISteamNetworkingSockets* sockets = SteamNetworkingSockets(); sockets != nullptr)
        {
            sockets->RunCallbacks();
        }

        m_Server->PollMessages();
        m_ServerDriver.Tick(*m_ServerScene, *m_Server, dt);

        for (sizet i = 0; i < m_Clients.size(); ++i)
        {
            if (m_Clients[i])
            {
                m_ClientDrivers[i].Tick(*m_ClientScenes[i], *m_Clients[i], dt);
            }
        }
    }

    // Pump until `predicate` holds or the budget runs out. Real sockets mean real
    // asynchrony even on loopback, so every cross-process assertion needs one of
    // these rather than a fixed pump count.
    [[nodiscard]] bool PumpUntil(const std::function<bool()>& predicate, i32 maxFrames = 400)
    {
        for (i32 frame = 0; frame < maxFrames; ++frame)
        {
            if (predicate())
            {
                return true;
            }
            Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return predicate();
    }

    [[nodiscard]] bool BothClientsAssigned()
    {
        return m_ClientDrivers[0].GetLocalClientID() != 0 && m_ClientDrivers[1].GetLocalClientID() != 0;
    }

    // Every replicated entity the given client scene currently holds.
    [[nodiscard]] std::vector<u64> ReplicatedEntities(sizet clientIndex)
    {
        std::vector<u64> result;
        Scene& scene = *m_ClientScenes[clientIndex];
        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
        for (auto handle : view)
        {
            Entity entity{ handle, &scene };
            result.push_back(static_cast<u64>(entity.GetUUID()));
        }
        return result;
    }

    [[nodiscard]] bool ClientHasEntity(sizet clientIndex, u64 uuid)
    {
        return m_ClientScenes[clientIndex]->TryGetEntityWithUUID(UUID(uuid)).has_value();
    }

    [[nodiscard]] glm::vec3 EntityPosition(Scene& scene, u64 uuid)
    {
        auto entityOpt = scene.TryGetEntityWithUUID(UUID(uuid));
        if (!entityOpt.has_value() || !entityOpt->HasComponent<TransformComponent>())
        {
            return glm::vec3{ std::numeric_limits<f32>::quiet_NaN() };
        }
        return entityOpt->GetComponent<TransformComponent>().Translation;
    }

    void SendMove(sizet clientIndex, u64 entityUUID, const glm::vec3& delta)
    {
        NetworkMovementInput input;
        input.Delta = delta;
        m_ClientDrivers[clientIndex].SendInput(*m_ClientScenes[clientIndex], *m_Clients[clientIndex], entityUUID,
                                               input.Encode());
    }

    static ServerAuthoritativeLoopTest* s_Active;

    Scope<Scene> m_ServerScene;
    std::array<Scope<Scene>, 2> m_ClientScenes;

    Scope<NetworkServer> m_Server;
    std::array<Scope<NetworkClient>, 2> m_Clients;

    ServerReplicationDriver m_ServerDriver;
    std::array<ClientReplicationDriver, 2> m_ClientDrivers;
};

ServerAuthoritativeLoopTest* ServerAuthoritativeLoopTest::s_Active = nullptr;

TEST_F(ServerAuthoritativeLoopTest, EachConnectionGetsAPawnThatBothClientsSee)
{
    ConnectClient(0);
    ConnectClient(1);

    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }))
        << "the server never assigned client ids";

    const u64 pawn0 = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[0].GetLocalClientID());
    const u64 pawn1 = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[1].GetLocalClientID());
    ASSERT_NE(pawn0, 0ull) << "no pawn was spawned for client 0";
    ASSERT_NE(pawn1, 0ull) << "no pawn was spawned for client 1";
    ASSERT_NE(pawn0, pawn1);

    // The interesting assertion is not "my pawn arrived" but "the OTHER player's
    // pawn arrived" — that is the one a broadcast-to-self loop would still fail.
    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, pawn0) && ClientHasEntity(0, pawn1) &&
                                   ClientHasEntity(1, pawn0) && ClientHasEntity(1, pawn1); }))
        << "clients did not both receive both pawns";

    EXPECT_GE(ReplicatedEntities(0).size(), 2u);
    EXPECT_GE(ReplicatedEntities(1).size(), 2u);
}

TEST_F(ServerAuthoritativeLoopTest, ClientInputMovesItsPawnOnTheServerAndOnTheOtherClient)
{
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    const u64 pawn0 = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[0].GetLocalClientID());
    ASSERT_NE(pawn0, 0ull);
    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(1, pawn0); }));

    const glm::vec3 startOnServer = EntityPosition(*m_ServerScene, pawn0);

    // Ten 0.25-unit steps: well inside the server's 1.0 per-command clamp, and far
    // enough that a "moved" assertion cannot be interpolation noise.
    constexpr i32 kSteps = 10;
    constexpr f32 kStep = 0.25f;
    for (i32 i = 0; i < kSteps; ++i)
    {
        SendMove(0, pawn0, { kStep, 0.0f, 0.0f });
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const f32 expectedX = startOnServer.x + kSteps * kStep;

    // Wait for the server to have PROCESSED every input, not merely for the pawn to
    // have moved somewhere. A "has it moved past halfway" wait returns early, and
    // the exact-position assertion that follows would then be racing the last few
    // commands still in flight.
    ASSERT_TRUE(PumpUntil([&]
                          { return m_ServerDriver.GetInputHandler().GetLastProcessedTick(
                                       m_ClientDrivers[0].GetLocalClientID()) >= static_cast<u32>(kSteps); }))
        << "the server never processed all " << kSteps << " inputs";
    EXPECT_NEAR(EntityPosition(*m_ServerScene, pawn0).x, expectedX, 0.01f);

    // And the move reaches the observer. Interpolation runs ~100 ms behind the
    // newest snapshot, so the tolerance covers the render delay rather than
    // pretending the two clocks are the same.
    ASSERT_TRUE(PumpUntil([&]
                          { return EntityPosition(*m_ClientScenes[1], pawn0).x > startOnServer.x + 0.5f * kSteps * kStep; }))
        << "client 1 never saw client 0's pawn move";
}

TEST_F(ServerAuthoritativeLoopTest, ServerRejectsInputAimedAtAnotherClientsPawn)
{
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    const u64 pawn1 = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[1].GetLocalClientID());
    ASSERT_NE(pawn1, 0ull);
    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, pawn1); }));

    const glm::vec3 before = EntityPosition(*m_ServerScene, pawn1);

    // Client 0 forges input for client 1's pawn. The existing AuthorityRejectionTest
    // covers this against ServerInputHandler in isolation; the point here is that
    // the check still holds when the command travels the live loop.
    for (i32 i = 0; i < 10; ++i)
    {
        SendMove(0, pawn1, { 0.5f, 0.0f, 0.0f });
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (i32 i = 0; i < 10; ++i)
    {
        Pump();
    }

    const glm::vec3 after = EntityPosition(*m_ServerScene, pawn1);
    EXPECT_NEAR(after.x, before.x, 1e-4f) << "the server let client 0 drive client 1's pawn";
    EXPECT_NEAR(after.y, before.y, 1e-4f);
    EXPECT_NEAR(after.z, before.z, 1e-4f);
}

TEST_F(ServerAuthoritativeLoopTest, ServerRejectsInputForAServerAuthoritativeEntity)
{
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    // A server-owned entity that the client is nonetheless recorded as "owning".
    // Ownership alone must not be enough — the authority mode is a separate gate,
    // and this is the case where a client tries to drive world-simulated state.
    constexpr u64 kServerEntity = 606060ull;
    Entity serverOwned = m_ServerScene->CreateEntityWithUUID(UUID(kServerEntity), "WorldObject");
    serverOwned.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
    auto& nic = serverOwned.AddComponent<NetworkIdentityComponent>();
    nic.IsReplicated = true;
    nic.Authority = ENetworkAuthority::Server;
    nic.OwnerClientID = m_ClientDrivers[0].GetLocalClientID();

    for (i32 i = 0; i < 10; ++i)
    {
        SendMove(0, kServerEntity, { 0.5f, 0.0f, 0.0f });
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (i32 i = 0; i < 10; ++i)
    {
        Pump();
    }

    EXPECT_NEAR(EntityPosition(*m_ServerScene, kServerEntity).x, 0.0f, 1e-4f)
        << "a client drove a server-authoritative entity";
}

TEST_F(ServerAuthoritativeLoopTest, PredictionAndReconciliationConvergeOnTheServerPosition)
{
    // The end-to-end prediction contract: the client applies its own input
    // immediately, the server applies the same commands authoritatively, the ack
    // retires them, and the replay leaves the two agreeing. Every one of the
    // prediction bugs this change fixed shows up here as a growing gap rather than
    // as a crash.
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    const u64 pawn = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[0].GetLocalClientID());
    ASSERT_NE(pawn, 0ull);
    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, pawn); }));

    constexpr i32 kSteps = 20;
    constexpr f32 kStep = 0.2f;
    for (i32 i = 0; i < kSteps; ++i)
    {
        SendMove(0, pawn, { kStep, 0.0f, 0.0f });
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Prediction applied locally straight away, so the client is already there
    // rather than waiting a round trip.
    EXPECT_GT(EntityPosition(*m_ClientScenes[0], pawn).x, 0.5f * kSteps * kStep)
        << "the client never predicted its own movement";

    // Let every input be acknowledged and reconciled.
    ASSERT_TRUE(PumpUntil([&]
                          { return m_ServerDriver.GetInputHandler().GetLastProcessedTick(
                                       m_ClientDrivers[0].GetLocalClientID()) >= static_cast<u32>(kSteps); }))
        << "the server did not process every input — a stuck input tick would look exactly like this";

    for (i32 i = 0; i < 30; ++i)
    {
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const f32 serverX = EntityPosition(*m_ServerScene, pawn).x;
    const f32 clientX = EntityPosition(*m_ClientScenes[0], pawn).x;

    EXPECT_NEAR(serverX, static_cast<f32>(kSteps) * kStep, 0.01f);
    // Converged, not merely "moved": a reconciliation that replays on top of its
    // own drift passes a "did it move" check and fails this one.
    EXPECT_NEAR(clientX, serverX, 0.05f)
        << "client (" << clientX << ") did not converge on the server (" << serverX << ")";
}

TEST_F(ServerAuthoritativeLoopTest, DisconnectDespawnsThePawnOnTheRemainingClient)
{
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    const u64 pawn1 = m_ServerDriver.GetPlayerEntity(m_ClientDrivers[1].GetLocalClientID());
    ASSERT_NE(pawn1, 0ull);
    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, pawn1); }));

    m_Clients[1]->Disconnect();
    m_Clients[1].reset();

    ASSERT_TRUE(PumpUntil([&]
                          { return !ClientHasEntity(0, pawn1); }))
        << "client 0 still holds the departed player's pawn";

    // And the server forgot it too, rather than leaking an orphan every join/leave.
    EXPECT_FALSE(m_ServerScene->TryGetEntityWithUUID(UUID(pawn1)).has_value());
}

TEST_F(ServerAuthoritativeLoopTest, ClientOnlyDestroysEntitiesTheSpawnPathCreated)
{
    // A scene-authored entity that leaves relevance must stop updating, NOT be
    // deleted — otherwise walking away from the level would dismantle it.
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    constexpr u64 kAuthoredUUID = 909090ull;
    Entity authoredOnClient = m_ClientScenes[0]->CreateEntityWithUUID(UUID(kAuthoredUUID), "LevelProp");
    authoredOnClient.AddComponent<NetworkIdentityComponent>();

    // The server despawns the same uuid; the client must ignore it because its own
    // driver never created that entity.
    const auto despawnPayload = EntityLifecycle::EncodeDespawn(kAuthoredUUID);
    m_ClientDrivers[0].HandleDespawn(*m_ClientScenes[0], despawnPayload.data(),
                                     static_cast<u32>(despawnPayload.size()));

    EXPECT_TRUE(ClientHasEntity(0, kAuthoredUUID))
        << "a despawn destroyed a scene-authored entity the client did not spawn";
}

TEST_F(ServerAuthoritativeLoopTest, InterestScopingWithholdsADistantEntity)
{
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    // A replicated entity far outside a small relevance radius. (Named `distant`,
    // not `far` — the Windows SDK still defines `far`/`near` as empty macros.)
    constexpr u64 kFarUUID = 700700ull;
    Entity distant = m_ServerScene->CreateEntityWithUUID(UUID(kFarUUID), "FarProp");
    distant.GetComponent<TransformComponent>().Translation = { 5000.0f, 0.0f, 0.0f };
    distant.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    distant.AddComponent<NetworkInterestComponent>().RelevanceRadius = 10.0f;

    // A replicated entity right on top of the player.
    constexpr u64 kNearUUID = 700701ull;
    Entity adjacent = m_ServerScene->CreateEntityWithUUID(UUID(kNearUUID), "NearProp");
    adjacent.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
    adjacent.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    adjacent.AddComponent<NetworkInterestComponent>().RelevanceRadius = 10.0f;

    ASSERT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, kNearUUID); }))
        << "the in-range entity never replicated, so the out-of-range assertion would be vacuous";

    // Give the loop plenty of further ticks: the far entity must STILL be absent.
    for (i32 i = 0; i < 40; ++i)
    {
        Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(ClientHasEntity(0, kFarUUID)) << "interest scoping did not withhold the distant entity";
}

TEST_F(ServerAuthoritativeLoopTest, InterestScopingDisabledReplicatesEverything)
{
    // The control for the test above: with scoping off the same far entity DOES
    // arrive, which proves the exclusion came from the interest rules and not from
    // the entity simply never being replicated at all.
    m_ServerDriver.SetInterestScopingEnabled(false);

    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    constexpr u64 kFarUUID = 700702ull;
    Entity distant = m_ServerScene->CreateEntityWithUUID(UUID(kFarUUID), "FarProp");
    distant.GetComponent<TransformComponent>().Translation = { 5000.0f, 0.0f, 0.0f };
    distant.AddComponent<NetworkIdentityComponent>().IsReplicated = true;
    distant.AddComponent<NetworkInterestComponent>().RelevanceRadius = 10.0f;

    EXPECT_TRUE(PumpUntil([&]
                          { return ClientHasEntity(0, kFarUUID); }))
        << "with interest scoping disabled every replicated entity should arrive";
}

TEST_F(ServerAuthoritativeLoopTest, LagCompensationRewindsToWhereTheClientSawTheWorld)
{
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    // A server-driven target that moves every tick, so "now" and "then" differ.
    constexpr u64 kTargetUUID = 800800ull;
    Entity target = m_ServerScene->CreateEntityWithUUID(UUID(kTargetUUID), "Target");
    target.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
    target.AddComponent<NetworkIdentityComponent>().IsReplicated = true;

    // Build history: 30 replication ticks with the target advancing 1 unit each.
    for (i32 i = 0; i < 30; ++i)
    {
        auto entityOpt = m_ServerScene->TryGetEntityWithUUID(UUID(kTargetUUID));
        ASSERT_TRUE(entityOpt.has_value());
        entityOpt->GetComponent<TransformComponent>().Translation.x += 1.0f;
        Pump();
    }

    const f32 presentX = EntityPosition(*m_ServerScene, kTargetUUID).x;
    ASSERT_GT(presentX, 20.0f) << "the target did not accumulate enough history to rewind through";

    f32 rewoundX = presentX;
    const bool performed = m_ServerDriver.PerformLagCompensatedCheck(
        *m_ServerScene, *m_Server, m_ClientDrivers[0].GetLocalClientID(),
        [&](Scene& scene)
        {
            auto entityOpt = scene.TryGetEntityWithUUID(UUID(kTargetUUID));
            if (entityOpt.has_value())
            {
                rewoundX = entityOpt->GetComponent<TransformComponent>().Translation.x;
            }
        });

    ASSERT_TRUE(performed) << "lag compensation refused to rewind";

    // The check saw the PAST, not the present — that difference is the entire
    // point, and a rewind that silently no-ops would look identical without it.
    EXPECT_LT(rewoundX, presentX) << "the rewind produced the present-day position";

    // ...and the world was put back afterwards.
    EXPECT_FLOAT_EQ(EntityPosition(*m_ServerScene, kTargetUUID).x, presentX)
        << "lag compensation did not restore the current state";
}

TEST_F(ServerAuthoritativeLoopTest, PingQueriesDoNotDeadlockAgainstConnectionIteration)
{
    // Regression guard for a shipped deadlock: GetClientPingMs takes the same
    // non-recursive mutex ForEachConnection holds for the whole callback, so asking
    // for a ping from inside the iteration hung the caller. The editor's Network
    // Debug panel did exactly that — opening it against a live server froze the
    // editor — as did the server console's `players` command and
    // NetworkManager::GetClientPingMs.
    //
    // This test hangs rather than fails if the deadlock returns, which is the
    // honest signal: a deadlock is not an assertion failure. Both supported shapes
    // are exercised below.
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    // Shape 1: snapshot, then query outside the iteration (what the panel and the
    // console command now do).
    const auto snapshot = m_Server->GetConnectionSnapshot();
    EXPECT_GE(snapshot.size(), 2u);
    for (const auto& info : snapshot)
    {
        const i32 ping = m_Server->GetClientPingMs(info.Handle);
        EXPECT_GE(ping, -1) << "ping query returned an impossible value";
    }

    // Shape 2: resolve-and-query by client id in one call (what the replication
    // driver's lag compensation uses).
    for (const auto& info : snapshot)
    {
        EXPECT_GE(m_Server->GetClientPingMsById(info.ClientID), -1);
    }

    // An id nobody holds resolves to "unknown" rather than blocking on a search.
    EXPECT_EQ(m_Server->GetClientPingMsById(999999u), -1);
}

TEST_F(ServerAuthoritativeLoopTest, ServerRpcFromAClientRunsOnTheServerWithTheSendersIdentity)
{
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    u32 observedSender = 0;
    i64 observedArg = 0;
    bool ranOnServer = false;

    RpcDescriptor descriptor;
    descriptor.Name = "Test.ServerPing";
    descriptor.Target = ERpcTarget::Server;
    descriptor.RequiresOwnership = false;
    descriptor.Handler = [&](const RpcContext& context, const RpcArgList& args)
    {
        ranOnServer = context.IsServer;
        observedSender = context.SenderClientID;
        if (!args.empty())
        {
            observedArg = args[0].AsInt;
        }
    };
    RpcRegistry::Register(descriptor);

    auto registered = RpcRegistry::FindByName("Test.ServerPing");
    ASSERT_TRUE(registered.has_value());

    RpcArgList args;
    args.push_back(RpcArg::MakeInt(1337));
    ASSERT_TRUE(m_ClientDrivers[0].InvokeRpc(*m_Clients[0], *registered, /*entityUUID*/ 0, args));

    ASSERT_TRUE(PumpUntil([&]
                          { return ranOnServer; }))
        << "the server never ran the client's RPC";

    EXPECT_EQ(observedSender, m_ClientDrivers[0].GetLocalClientID());
    EXPECT_EQ(observedArg, 1337);
}

TEST_F(ServerAuthoritativeLoopTest, MulticastRpcReachesBothClientsAndTheServer)
{
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    i32 runs = 0;
    bool sawServer = false;
    std::string payload;

    RpcDescriptor descriptor;
    descriptor.Name = "Test.Announce";
    descriptor.Target = ERpcTarget::Multicast;
    descriptor.Handler = [&](const RpcContext& context, const RpcArgList& args)
    {
        ++runs;
        sawServer = sawServer || context.IsServer;
        if (!args.empty())
        {
            payload = args[0].AsString;
        }
    };
    RpcRegistry::Register(descriptor);

    auto registered = RpcRegistry::FindByName("Test.Announce");
    ASSERT_TRUE(registered.has_value());

    RpcArgList args;
    args.push_back(RpcArg::MakeString("match starting"));
    ASSERT_TRUE(m_ServerDriver.InvokeRpc(*m_ServerScene, *m_Server, *registered, 0, 0, args));

    // Server-local execution is synchronous; the two client deliveries are not.
    ASSERT_TRUE(PumpUntil([&]
                          { return runs >= 3; }))
        << "multicast reached " << runs << " of 3 expected recipients (server + 2 clients)";

    EXPECT_TRUE(sawServer) << "multicast did not run on the server itself";
    EXPECT_EQ(payload, "match starting");
}

TEST_F(ServerAuthoritativeLoopTest, ClientCannotOriginateAMulticastRpc)
{
    ConnectClient(0);
    ASSERT_TRUE(PumpUntil([this]
                          { return m_ClientDrivers[0].GetLocalClientID() != 0; }));

    bool ran = false;
    RpcDescriptor descriptor;
    descriptor.Name = "Test.ClientForgedBroadcast";
    descriptor.Target = ERpcTarget::Multicast;
    descriptor.Handler = [&ran](const RpcContext&, const RpcArgList&)
    { ran = true; };
    RpcRegistry::Register(descriptor);

    auto registered = RpcRegistry::FindByName("Test.ClientForgedBroadcast");
    ASSERT_TRUE(registered.has_value());

    // Refused before it ever reaches the wire.
    EXPECT_FALSE(m_ClientDrivers[0].InvokeRpc(*m_Clients[0], *registered, 0, {}));

    for (i32 i = 0; i < 20; ++i)
    {
        Pump();
    }
    EXPECT_FALSE(ran) << "a client managed to originate a multicast RPC";
}

TEST_F(ServerAuthoritativeLoopTest, ClientTargetRpcReachesOnlyTheAddressedClient)
{
    ConnectClient(0);
    ConnectClient(1);
    ASSERT_TRUE(PumpUntil([this]
                          { return BothClientsAssigned(); }));

    i32 runs = 0;
    RpcDescriptor descriptor;
    descriptor.Name = "Test.PrivateMessage";
    descriptor.Target = ERpcTarget::Client;
    descriptor.Handler = [&runs](const RpcContext&, const RpcArgList&)
    { ++runs; };
    RpcRegistry::Register(descriptor);

    auto registered = RpcRegistry::FindByName("Test.PrivateMessage");
    ASSERT_TRUE(registered.has_value());

    const u32 target = m_ClientDrivers[1].GetLocalClientID();
    ASSERT_TRUE(m_ServerDriver.InvokeRpc(*m_ServerScene, *m_Server, *registered, 0, target, {}));

    ASSERT_TRUE(PumpUntil([&]
                          { return runs >= 1; }))
        << "the addressed client never ran the RPC";

    for (i32 i = 0; i < 20; ++i)
    {
        Pump();
    }

    // Exactly one recipient: a Client-target RPC that broadcast would be a privacy
    // and bandwidth bug that still "works" from the addressee's point of view.
    EXPECT_EQ(runs, 1) << "a client-target RPC ran " << runs << " times";
}
