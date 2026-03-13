#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/NetworkInterestManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

class InterestManagerTest : public ::testing::Test
{
  protected:
	void SetUp() override
	{
		m_Scene = CreateScope<Scene>();
		m_Manager = NetworkInterestManager();
	}

	Scope<Scene> m_Scene;
	NetworkInterestManager m_Manager;
};

TEST_F(InterestManagerTest, EntitiesWithoutInterestComponentAreAlwaysRelevant)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "NoInterest");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;

	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, ZeroRadiusIsAlwaysRelevant)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "ZeroRadius");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;
	auto& interest = e.AddComponent<NetworkInterestComponent>();
	interest.RelevanceRadius = 0.0f;

	m_Manager.SetClientPosition(1, { 9999.0f, 0.0f, 0.0f });
	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, DistanceFilteringWorks)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Near");
	auto& tc = e.GetComponent<TransformComponent>();
	tc.Translation = { 10.0f, 0.0f, 0.0f };
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;
	auto& interest = e.AddComponent<NetworkInterestComponent>();
	interest.RelevanceRadius = 5.0f;

	// Client is at origin — entity is 10 units away, radius is 5
	m_Manager.SetClientPosition(1, { 0.0f, 0.0f, 0.0f });
	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_FALSE(found); // Too far

	// Move client closer
	m_Manager.SetClientPosition(1, { 8.0f, 0.0f, 0.0f });
	relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_TRUE(found); // Within 5 units
}

TEST_F(InterestManagerTest, GroupFilteringWorks)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "GroupEntity");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;
	auto& interest = e.AddComponent<NetworkInterestComponent>();
	interest.InterestGroup = 5;

	// Client not subscribed to group 5
	m_Manager.SetClientInterestGroups(1, { 1, 2 });
	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_FALSE(found);

	// Subscribe client to group 5
	m_Manager.SetClientInterestGroups(1, { 1, 5 });
	relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, DefaultGroupIsAlwaysIncluded)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "DefaultGroup");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;
	auto& interest = e.AddComponent<NetworkInterestComponent>();
	interest.InterestGroup = 0; // Default group

	// Client has no explicit group subscriptions
	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_TRUE(found);
}

TEST_F(InterestManagerTest, NonReplicatedEntitiesAreExcluded)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "NoReplicate");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = false;

	auto relevant = m_Manager.GetRelevantEntities(1, *m_Scene);
	bool found = std::find(relevant.begin(), relevant.end(), 100u) != relevant.end();
	EXPECT_FALSE(found);
}

TEST_F(InterestManagerTest, IsEntityRelevantMatchesGetRelevantEntities)
{
	Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Test");
	auto& nic = e.AddComponent<NetworkIdentityComponent>();
	nic.IsReplicated = true;

	bool relevant = m_Manager.IsEntityRelevant(1, 100, *m_Scene);
	EXPECT_TRUE(relevant);
}
