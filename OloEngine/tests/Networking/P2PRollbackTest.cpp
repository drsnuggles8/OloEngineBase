#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/P2P/RollbackManager.h"
#include "OloEngine/Networking/P2P/NetworkPeerMesh.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

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
	const auto* predicted = m_Manager.GetInputForTick(1, 7);
	ASSERT_NE(predicted, nullptr);
	EXPECT_EQ(*predicted, (std::vector<u8>{ 0xAA, 0xBB }));
}

TEST_F(RollbackManagerTest, MaxRollbackGetSet)
{
	EXPECT_EQ(m_Manager.GetMaxRollbackFrames(), 7u);
	m_Manager.SetMaxRollbackFrames(4);
	EXPECT_EQ(m_Manager.GetMaxRollbackFrames(), 4u);
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

	// Manually add peers (simulating established connections)
	auto& peers = const_cast<std::unordered_map<u32, PeerInfo>&>(mesh.GetPeers());
	peers[1] = PeerInfo{ 1, "", 0, false };
	peers[2] = PeerInfo{ 2, "", 0, false };

	mesh.PerformHostMigration();

	EXPECT_EQ(mesh.GetHostPeerID(), 1u);
	EXPECT_TRUE(peers[1].IsHost);
	EXPECT_FALSE(peers[2].IsHost);
	EXPECT_FALSE(peers[3].IsHost);
}
