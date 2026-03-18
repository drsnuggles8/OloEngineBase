#include <gtest/gtest.h>
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Asset/AssetTypes.h"

using namespace OloEngine;

// ============================================================================
// NavMeshSettings
// ============================================================================

TEST(NavMeshSettingsTest, DefaultValues)
{
    NavMeshSettings settings;
    EXPECT_FLOAT_EQ(settings.CellSize, 0.3f);
    EXPECT_FLOAT_EQ(settings.CellHeight, 0.2f);
    EXPECT_FLOAT_EQ(settings.AgentRadius, 0.5f);
    EXPECT_FLOAT_EQ(settings.AgentHeight, 2.0f);
    EXPECT_FLOAT_EQ(settings.AgentMaxClimb, 0.9f);
    EXPECT_FLOAT_EQ(settings.AgentMaxSlope, 45.0f);
    EXPECT_EQ(settings.RegionMinSize, 8);
    EXPECT_EQ(settings.RegionMergeSize, 20);
    EXPECT_FLOAT_EQ(settings.EdgeMaxLen, 12.0f);
    EXPECT_FLOAT_EQ(settings.EdgeMaxError, 1.3f);
    EXPECT_EQ(settings.VertsPerPoly, 6);
    EXPECT_FLOAT_EQ(settings.DetailSampleDist, 6.0f);
    EXPECT_FLOAT_EQ(settings.DetailSampleMaxError, 1.0f);
}

// ============================================================================
// NavMesh asset
// ============================================================================

TEST(NavMeshAssetTest, StaticType)
{
    EXPECT_EQ(NavMesh::GetStaticType(), AssetType::NavMesh);
}

TEST(NavMeshAssetTest, DefaultConstructorIsInvalid)
{
    NavMesh mesh;
    EXPECT_FALSE(mesh.IsValid());
    EXPECT_EQ(mesh.GetDetourNavMesh(), nullptr);
    EXPECT_EQ(mesh.GetPolyCount(), 0);
}

TEST(NavMeshAssetTest, AssetTypeConsistency)
{
    NavMesh mesh;
    EXPECT_EQ(mesh.GetAssetType(), AssetType::NavMesh);
}

TEST(NavMeshAssetTest, SettingsRoundTrip)
{
    NavMesh mesh;
    NavMeshSettings s;
    s.CellSize = 0.5f;
    s.AgentRadius = 1.0f;
    mesh.SetSettings(s);
    EXPECT_FLOAT_EQ(mesh.GetSettings().CellSize, 0.5f);
    EXPECT_FLOAT_EQ(mesh.GetSettings().AgentRadius, 1.0f);
}

TEST(NavMeshAssetTest, SerializeEmptyMeshFails)
{
    NavMesh mesh;
    std::vector<u8> data;
    EXPECT_FALSE(mesh.Serialize(data));
}

TEST(NavMeshAssetTest, DeserializeGarbageDataFails)
{
    NavMesh mesh;
    std::vector<u8> garbage = { 0xFF, 0xFE, 0x00, 0x01, 0x02, 0x03 };
    EXPECT_FALSE(mesh.Deserialize(garbage));
}

TEST(NavMeshAssetTest, MoveConstructor)
{
    NavMesh a;
    NavMeshSettings s;
    s.CellSize = 0.7f;
    a.SetSettings(s);
    NavMesh b(std::move(a));
    EXPECT_FLOAT_EQ(b.GetSettings().CellSize, 0.7f);
}

// ============================================================================
// NavMeshQuery
// ============================================================================

TEST(NavMeshQueryTest, DefaultConstructorIsInvalid)
{
    NavMeshQuery query;
    EXPECT_FALSE(query.IsValid());
}

TEST(NavMeshQueryTest, FindPathWithoutNavMeshReturnsFalse)
{
    NavMeshQuery query;
    std::vector<glm::vec3> path;
    EXPECT_FALSE(query.FindPath({ 0, 0, 0 }, { 10, 0, 10 }, path));
    EXPECT_TRUE(path.empty());
}

TEST(NavMeshQueryTest, FindNearestPointWithoutNavMeshReturnsFalse)
{
    NavMeshQuery query;
    glm::vec3 nearest;
    EXPECT_FALSE(query.FindNearestPoint({ 0, 0, 0 }, 5.0f, nearest));
}

TEST(NavMeshQueryTest, RaycastWithoutNavMeshReturnsFalse)
{
    NavMeshQuery query;
    glm::vec3 hit;
    EXPECT_FALSE(query.Raycast({ 0, 0, 0 }, { 10, 0, 10 }, hit));
}

TEST(NavMeshQueryTest, IsPointOnNavMeshWithoutNavMeshReturnsFalse)
{
    NavMeshQuery query;
    EXPECT_FALSE(query.IsPointOnNavMesh({ 0, 0, 0 }));
}

// ============================================================================
// CrowdManager
// ============================================================================

TEST(CrowdManagerTest, DefaultConstructorIsInvalid)
{
    CrowdManager crowd;
    EXPECT_FALSE(crowd.IsValid());
}

TEST(CrowdManagerTest, AddAgentWithoutNavMeshReturnsNegative)
{
    CrowdManager crowd;
    NavAgentComponent agent;
    EXPECT_LT(crowd.AddAgent({ 0, 0, 0 }, agent), 0);
}

// ============================================================================
// Components
// ============================================================================

TEST(NavMeshBoundsComponentTest, DefaultValues)
{
    NavMeshBoundsComponent comp;
    EXPECT_EQ(comp.m_Min, glm::vec3(-100.0f, -10.0f, -100.0f));
    EXPECT_EQ(comp.m_Max, glm::vec3(100.0f, 50.0f, 100.0f));
}

TEST(NavAgentComponentTest, DefaultValues)
{
    NavAgentComponent comp;
    EXPECT_FLOAT_EQ(comp.m_Radius, 0.5f);
    EXPECT_FLOAT_EQ(comp.m_Height, 2.0f);
    EXPECT_FLOAT_EQ(comp.m_MaxSpeed, 3.5f);
    EXPECT_FLOAT_EQ(comp.m_Acceleration, 8.0f);
    EXPECT_FLOAT_EQ(comp.m_StoppingDistance, 0.1f);
    EXPECT_EQ(comp.m_AvoidancePriority, 50);
    EXPECT_FALSE(comp.m_HasTarget);
    EXPECT_FALSE(comp.m_HasPath);
    EXPECT_EQ(comp.m_CrowdAgentId, -1);
}

// ============================================================================
// AssetType conversion
// ============================================================================

TEST(AssetTypeTest, NavMeshToString)
{
    EXPECT_STREQ(AssetUtils::AssetTypeToString(AssetType::NavMesh), "NavMesh");
}

TEST(AssetTypeTest, NavMeshFromString)
{
    EXPECT_EQ(AssetUtils::AssetTypeFromString("NavMesh"), AssetType::NavMesh);
}
