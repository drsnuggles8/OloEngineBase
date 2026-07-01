#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/P2P/RollbackManager.h"
#include "OloEngine/Networking/P2P/NetworkPeerMesh.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cstring>
#include <string>

using namespace OloEngine;

namespace
{
    // ── Cross-peer rollback-determinism smoke-test helpers (issue #452 AC3) ──
    //
    // These model a tiny two-avatar lockstep world where each peer's per-tick
    // input is a deterministic X-axis move. The rollback re-simulation must
    // reconstruct exactly what a straight-through peer computed — that bit-for-bit
    // agreement is what keeps lockstep peers in sync, and it only holds because
    // the simulation (fixed-step + seeded RNG, #452) is deterministic.

    // A peer's avatar lives at a fixed UUID so both peers' worlds and every
    // rollback snapshot address the same entity.
    [[nodiscard]] UUID PeerEntityUUID(u32 peerID)
    {
        return UUID(1000 + peerID);
    }

    [[nodiscard]] std::vector<u8> EncodeDelta(i32 delta)
    {
        std::vector<u8> data(sizeof(i32));
        std::memcpy(data.data(), &delta, sizeof(i32));
        return data;
    }

    // The deterministic "simulation step" for one peer's input: move that peer's
    // avatar along X. Used as BOTH the forward step and the rollback re-sim
    // callback, so the two paths mutate state identically.
    void ApplyPeerInput(Scene& scene, u32 peerID, const u8* data, u32 size)
    {
        if (size < sizeof(i32))
        {
            return;
        }
        i32 delta = 0;
        std::memcpy(&delta, data, sizeof(i32));
        if (Entity e = scene.GetEntityByUUID(PeerEntityUUID(peerID)); e)
        {
            e.GetComponent<TransformComponent>().Translation.x += static_cast<f32>(delta);
        }
    }

    // A two-avatar world; both avatars are replicated so EntitySnapshot captures
    // them for save / rollback.
    [[nodiscard]] Ref<Scene> BuildLockstepWorld()
    {
        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);
        for (u32 peerID = 0; peerID < 2; ++peerID)
        {
            Entity e = scene->CreateEntityWithUUID(PeerEntityUUID(peerID), "Peer" + std::to_string(peerID));
            e.AddComponent<NetworkIdentityComponent>(); // IsReplicated = true by default
        }
        return scene;
    }

    // Per-tick inputs, deliberately varying so a correct re-simulation must
    // replay the exact sequence rather than just land on a constant.
    [[nodiscard]] i32 Peer0Delta(u32 tick)
    {
        return static_cast<i32>(tick);
    }
    [[nodiscard]] i32 Peer1Delta(u32 tick)
    {
        return static_cast<i32>(tick) * 3;
    }
} // namespace

// ── RollbackManager Tests ────────────────────────────────────────────

class RollbackManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = CreateScope<Scene>();
        m_Manager = RollbackManager();
        m_AppliedInputs.clear();

        m_Manager.SetInputApplyCallback(
            [this](Scene& /*s*/, u32 peerID, const u8* data, u32 size)
            {
                m_AppliedInputs.emplace_back(peerID, std::vector<u8>(data, data + size));
            });
    }

    Scope<Scene> m_Scene;
    RollbackManager m_Manager;
    std::vector<std::pair<u32, std::vector<u8>>> m_AppliedInputs;
};

TEST_F(RollbackManagerTest, SaveAndRestoreState)
{
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Player");
    auto& tc = e.GetComponent<TransformComponent>();
    tc.Translation = { 1.0f, 2.0f, 3.0f };

    m_Manager.SaveState(1, *m_Scene);

    // Modify the entity
    tc.Translation = { 10.0f, 20.0f, 30.0f };

    // Submit inputs and trigger rollback
    m_Manager.SetCurrentTick(3);
    m_Manager.SubmitLocalInput(1, 1, { 0x01 });
    m_Manager.SubmitLocalInput(1, 2, { 0x02 });
    m_Manager.SubmitLocalInput(1, 3, { 0x03 });

    // Save states for re-sim
    m_Manager.SaveState(2, *m_Scene);
    m_Manager.SaveState(3, *m_Scene);

    // Receive late remote input for tick 1 — should trigger rollback
    u32 depth = m_Manager.ReceiveRemoteInput(2, 1, { 0xAA }, *m_Scene);
    EXPECT_EQ(depth, 2u); // Rolled back from tick 3 to tick 1

    // Verify the input-application callback was invoked during re-sim
    ASSERT_FALSE(m_AppliedInputs.empty());
    // Re-sim covers ticks 2 and 3, so expect at least 2 callback invocations
    EXPECT_GE(m_AppliedInputs.size(), 2u);

    // Entity should be at the restored position after re-sim
    EXPECT_EQ(m_Manager.GetRollbackCount(), 1u);
}

TEST_F(RollbackManagerTest, NoRollbackForCurrentTick)
{
    m_Manager.SetCurrentTick(5);
    u32 depth = m_Manager.ReceiveRemoteInput(2, 5, { 0x01 }, *m_Scene);
    EXPECT_EQ(depth, 0u);
    EXPECT_EQ(m_Manager.GetRollbackCount(), 0u);
}

TEST_F(RollbackManagerTest, NoRollbackForFutureTick)
{
    m_Manager.SetCurrentTick(3);
    u32 depth = m_Manager.ReceiveRemoteInput(2, 5, { 0x01 }, *m_Scene);
    EXPECT_EQ(depth, 0u);
}

TEST_F(RollbackManagerTest, ExcessiveRollbackDropped)
{
    m_Manager.SetMaxRollbackFrames(3);
    m_Manager.SetCurrentTick(10);

    // Input 7 frames late (> max 3)
    u32 depth = m_Manager.ReceiveRemoteInput(2, 3, { 0x01 }, *m_Scene);
    EXPECT_EQ(depth, 0u);
    EXPECT_EQ(m_Manager.GetRollbackCount(), 0u);
}

TEST_F(RollbackManagerTest, InputPredictionRepeatsLastKnown)
{
    m_Manager.SubmitLocalInput(1, 5, { 0xAA, 0xBB });

    // Request tick 7 — no real input, should get tick 5's input (most recent before 7)
    const auto predicted = m_Manager.GetInputForTick(1, 7);
    ASSERT_TRUE(predicted.has_value());
    EXPECT_EQ(*predicted, (std::vector<u8>{ 0xAA, 0xBB }));
}

TEST_F(RollbackManagerTest, MaxRollbackGetSet)
{
    EXPECT_EQ(m_Manager.GetMaxRollbackFrames(), 7u);
    m_Manager.SetMaxRollbackFrames(4);
    EXPECT_EQ(m_Manager.GetMaxRollbackFrames(), 4u);
}

// ── Cross-peer rollback determinism (issue #452 acceptance criterion 3) ──

// What this validates (and what it deliberately does not): the property that
// keeps lockstep peers in sync is *deterministic reconstruction* — restoring a
// snapshot at tick T and replaying the agreed inputs T+1..N must reproduce the
// exact state a peer that never rolled back computed. That is what this test
// pins: any non-determinism in EntitySnapshot capture/restore or in the apply
// re-sim (unstable iteration order, unseeded RNG, wall-clock reads) would make
// the rolled-back peer diverge from the straight-through peer.
//
// It does NOT exercise a *mispredicted* input being corrected to a different
// value: the late input here carries the SAME value already applied in the
// forward pass, so the re-sim replays an identical sequence. That is intentional
// — RollbackManager::Rollback restores the snapshot AT toTick (which already
// folded in toTick's input) and re-applies from toTick+1, so a corrected toTick
// value would not be re-applied; testing a divergent correction would assert
// against that simplified semantic rather than determinism. Pinning the
// reconstruction property is the part that matters for "stays in sync"; a
// divergent-correction test belongs with a RollbackManager semantics change
// (issue #452 follow-up), not here.
TEST(P2PRollbackDeterminismTest, RollbackResimReconstructsIdenticalCrossPeerState)
{
    constexpr u32 kTicks = 6;
    constexpr u32 kLateTick = 2; // peer 1's input for this tick is delivered out of order

    // Drive a peer's world straight through with in-order inputs, saving a
    // rollback snapshot each tick. Identical for both peers up to the network
    // reordering below.
    auto runForward = [](Scene& scene, RollbackManager& mgr)
    {
        mgr.SetInputApplyCallback(ApplyPeerInput);
        for (u32 t = 1; t <= kTicks; ++t)
        {
            mgr.SetCurrentTick(t);
            const auto in0 = EncodeDelta(Peer0Delta(t));
            const auto in1 = EncodeDelta(Peer1Delta(t));
            mgr.SubmitLocalInput(0, t, in0);
            mgr.SubmitLocalInput(1, t, in1);
            ApplyPeerInput(scene, 0, in0.data(), static_cast<u32>(in0.size()));
            ApplyPeerInput(scene, 1, in1.data(), static_cast<u32>(in1.size()));
            mgr.SaveState(t, scene);
        }
    };

    // Peer A — clean delivery, straight through. The authoritative reference.
    Ref<Scene> sceneA = BuildLockstepWorld();
    RollbackManager mgrA;
    runForward(*sceneA, mgrA);

    // Peer B — same inputs, but peer 1's kLateTick input arrives out of order.
    Ref<Scene> sceneB = BuildLockstepWorld();
    RollbackManager mgrB;
    runForward(*sceneB, mgrB);

    // The out-of-order (earlier-tick) input forces a rollback + re-simulation
    // forward to the current tick.
    const u32 depth = mgrB.ReceiveRemoteInput(1, kLateTick, EncodeDelta(Peer1Delta(kLateTick)), *sceneB);
    EXPECT_EQ(depth, kTicks - kLateTick) << "rollback should span every tick after the late input";
    EXPECT_EQ(mgrB.GetRollbackCount(), 1u) << "an out-of-order input must trigger exactly one rollback";

    // The core "stays in sync" guarantee: after the rollback re-simulation, the
    // two peers' replicated worlds hold identical entity state. Compare the
    // LOGICAL per-entity transforms (order-independent, float-tolerant), NOT the
    // raw EntitySnapshot bytes: the serialized form carries ComponentReplicator
    // fields that are not guaranteed byte-stable across two independently-built
    // registries (that layer is issue #462's domain), so a byte compare is both
    // fragile and not the property under test.
    for (u32 peerID = 0; peerID < 2; ++peerID)
    {
        const glm::vec3 a = sceneA->GetEntityByUUID(PeerEntityUUID(peerID)).GetComponent<TransformComponent>().Translation;
        const glm::vec3 b = sceneB->GetEntityByUUID(PeerEntityUUID(peerID)).GetComponent<TransformComponent>().Translation;
        EXPECT_FLOAT_EQ(a.x, b.x) << "peer " << peerID << " X diverged after rollback re-simulation";
        EXPECT_FLOAT_EQ(a.y, b.y) << "peer " << peerID << " Y diverged after rollback re-simulation";
        EXPECT_FLOAT_EQ(a.z, b.z) << "peer " << peerID << " Z diverged after rollback re-simulation";
    }

    // And the reconstructed positions are exactly the summed per-tick deltas.
    i32 expected0 = 0;
    i32 expected1 = 0;
    for (u32 t = 1; t <= kTicks; ++t)
    {
        expected0 += Peer0Delta(t);
        expected1 += Peer1Delta(t);
    }
    EXPECT_FLOAT_EQ(sceneB->GetEntityByUUID(PeerEntityUUID(0)).GetComponent<TransformComponent>().Translation.x,
                    static_cast<f32>(expected0));
    EXPECT_FLOAT_EQ(sceneB->GetEntityByUUID(PeerEntityUUID(1)).GetComponent<TransformComponent>().Translation.x,
                    static_cast<f32>(expected1));
}

// ── NetworkPeerMesh Tests ────────────────────────────────────────────

TEST(PeerMeshTest, CreateSessionMakesHost)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);

    EXPECT_TRUE(mesh.IsHost());
    EXPECT_TRUE(mesh.IsInSession());
    EXPECT_EQ(mesh.GetLocalPeerID(), 1u);
    EXPECT_EQ(mesh.GetHostPeerID(), 1u);
    EXPECT_EQ(mesh.GetPeers().size(), 1u);
}

TEST(PeerMeshTest, JoinSessionNotHost)
{
    NetworkPeerMesh mesh;
    mesh.JoinSession(2, "127.0.0.1", 27015);

    EXPECT_TRUE(mesh.IsInSession());
    EXPECT_FALSE(mesh.IsHost());
    EXPECT_EQ(mesh.GetLocalPeerID(), 2u);
}

TEST(PeerMeshTest, LeaveSessionClearsState)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);
    mesh.LeaveSession();

    EXPECT_FALSE(mesh.IsInSession());
    EXPECT_FALSE(mesh.IsHost());
    EXPECT_TRUE(mesh.GetPeers().empty());
}

TEST(PeerMeshTest, HostMigrationSelectsLowestID)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(3);

    // Add peers to topology (no transport needed)
    mesh.AddPeer(1);
    mesh.AddPeer(2);

    mesh.PerformHostMigration();

    EXPECT_EQ(mesh.GetHostPeerID(), 1u);
    auto peers = mesh.GetPeers();
    EXPECT_TRUE(peers[1].IsHost);
    EXPECT_FALSE(peers[2].IsHost);
    EXPECT_FALSE(peers[3].IsHost);
}

// ── NetworkPeerMesh Transport Tests (GNS not initialised) ────────────

TEST(PeerMeshTest, StartListeningFailsWithoutGNS)
{
    // GNS is not initialised in unit tests, so StartListening should
    // return false gracefully rather than crashing.
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);
    EXPECT_FALSE(mesh.StartListening(27015));
}

TEST(PeerMeshTest, ConnectToPeerFailsWithoutGNS)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);
    EXPECT_FALSE(mesh.ConnectToPeer(2, "127.0.0.1", 27016));
}

TEST(PeerMeshTest, SendToPeerNoTransportIsNoOp)
{
    // SendToPeer should warn but not crash when no transport exists
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);

    mesh.AddPeer(2, "127.0.0.1", 27016);

    u8 const payload = 0xAA;
    // Should not crash — just logs a warning
    mesh.SendToPeer(2, ENetworkMessageType::RPC, &payload, 1, 0);
}

TEST(PeerMeshTest, BroadcastWithoutTransportIsNoOp)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);

    mesh.AddPeer(2, "127.0.0.1", 27016);
    mesh.AddPeer(3, "127.0.0.1", 27017);

    u8 const payload = 0xBB;
    // Should not crash
    mesh.BroadcastToPeers(ENetworkMessageType::RPC, &payload, 1, 0);
}

TEST(PeerMeshTest, PollMessagesWithoutGNSIsNoOp)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);
    // Should not crash
    mesh.PollMessages();
}

TEST(PeerMeshTest, LeaveSessionCleansUpTransportState)
{
    NetworkPeerMesh mesh;
    mesh.CreateSession(1);
    // Even though StartListening fails, LeaveSession should be safe
    mesh.StartListening(27015);
    mesh.LeaveSession();

    EXPECT_FALSE(mesh.IsInSession());
    EXPECT_TRUE(mesh.GetPeers().empty());
}

TEST(PeerMeshTest, MessageCallbackIsStored)
{
    NetworkPeerMesh mesh;
    bool called = false;

    mesh.SetMessageCallback(
        [&](u32 /*sender*/, ENetworkMessageType /*type*/, const u8* /*data*/, u32 /*size*/)
        { called = true; });

    // Callback is stored but not invoked here (no incoming messages)
    EXPECT_FALSE(called);
}
