#include <gtest/gtest.h>
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

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

// ============================================================================
// Positive round-trip: build → serialize → deserialize → query
// ============================================================================

// Helper: build a minimal flat-plane navmesh via Recast/Detour
static Ref<NavMesh> BuildFlatPlaneNavMesh()
{
    // A 40x40 flat quad at y=0 (large enough for agent radius erosion)
    // clang-format off
    f32 verts[] = {
        -20.0f, 0.0f, -20.0f,
         20.0f, 0.0f, -20.0f,
         20.0f, 0.0f,  20.0f,
        -20.0f, 0.0f,  20.0f,
    };
    i32 tris[] = { 0, 2, 1,  0, 3, 2 };
    // clang-format on

    constexpr i32 nverts = 4;
    constexpr i32 ntris  = 2;

    NavMeshSettings settings;
    settings.CellSize  = 0.3f;
    settings.CellHeight = 0.2f;

    rcConfig cfg{};
    cfg.cs = settings.CellSize;
    cfg.ch = settings.CellHeight;
    cfg.walkableSlopeAngle = settings.AgentMaxSlope;
    cfg.walkableHeight = static_cast<i32>(std::ceilf(settings.AgentHeight / cfg.ch));
    cfg.walkableClimb  = static_cast<i32>(std::floorf(settings.AgentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<i32>(std::ceilf(settings.AgentRadius / cfg.cs));
    cfg.maxEdgeLen     = static_cast<i32>(settings.EdgeMaxLen / cfg.cs);
    cfg.maxSimplificationError = settings.EdgeMaxError;
    cfg.minRegionArea   = settings.RegionMinSize * settings.RegionMinSize;
    cfg.mergeRegionArea = settings.RegionMergeSize * settings.RegionMergeSize;
    cfg.maxVertsPerPoly = settings.VertsPerPoly;
    cfg.detailSampleDist     = settings.CellSize * settings.DetailSampleDist;
    cfg.detailSampleMaxError = settings.CellHeight * settings.DetailSampleMaxError;

    cfg.bmin[0] = -20.0f; cfg.bmin[1] = -1.0f; cfg.bmin[2] = -20.0f;
    cfg.bmax[0] =  20.0f; cfg.bmax[1] =  1.0f; cfg.bmax[2] =  20.0f;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext ctx;

    auto* solid = rcAllocHeightfield();
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    { rcFreeHeightField(solid); return nullptr; }

    std::vector<u8> triAreas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris, ntris, triAreas.data());
    rcRasterizeTriangles(&ctx, verts, nverts, tris, triAreas.data(), ntris, *solid, cfg.walkableClimb);

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    auto* chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
    { rcFreeHeightField(solid); rcFreeCompactHeightfield(chf); return nullptr; }
    rcFreeHeightField(solid);

    rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
    rcBuildDistanceField(&ctx, *chf);
    rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);

    auto* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); return nullptr; }

    auto* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); return nullptr; }

    auto* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); rcFreePolyMeshDetail(dmesh); return nullptr; }

    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    for (i32 i = 0; i < pmesh->npolys; ++i)
    {
        if (pmesh->areas[i] != RC_NULL_AREA)
        { pmesh->areas[i] = RC_WALKABLE_AREA; pmesh->flags[i] = 1; }
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = settings.AgentHeight;
    params.walkableRadius = settings.AgentRadius;
    params.walkableClimb  = settings.AgentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    u8* navData = nullptr;
    i32 navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
    { rcFreePolyMesh(pmesh); rcFreePolyMeshDetail(dmesh); return nullptr; }

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    auto* navMesh = dtAllocNavMesh();
    if (!navMesh) { dtFree(navData); return nullptr; }

    if (dtStatusFailed(navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA)))
    { dtFree(navData); dtFreeNavMesh(navMesh); return nullptr; }

    auto result = Ref<NavMesh>::Create();
    result->SetDetourNavMesh(navMesh);
    result->SetSettings(settings);
    return result;
}

TEST(NavMeshRoundTripTest, SerializeDeserializePreservesPathfinding)
{
    auto original = BuildFlatPlaneNavMesh();
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->IsValid());
    ASSERT_GT(original->GetPolyCount(), 0);

    // Query on original
    NavMeshQuery origQuery(original);
    ASSERT_TRUE(origQuery.IsValid());

    std::vector<glm::vec3> origPath;
    EXPECT_TRUE(origQuery.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, origPath));
    EXPECT_FALSE(origPath.empty());

    // Serialize
    std::vector<u8> blob;
    ASSERT_TRUE(original->Serialize(blob));
    EXPECT_GT(blob.size(), 0u);

    // Deserialize into fresh NavMesh
    NavMesh restored;
    ASSERT_TRUE(restored.Deserialize(blob));
    ASSERT_TRUE(restored.IsValid());
    EXPECT_EQ(restored.GetPolyCount(), original->GetPolyCount());
    EXPECT_FLOAT_EQ(restored.GetSettings().CellSize, original->GetSettings().CellSize);

    // Query on restored
    auto restoredRef = Ref<NavMesh>::Create();
    *restoredRef = std::move(restored);
    NavMeshQuery restoredQuery(restoredRef);
    ASSERT_TRUE(restoredQuery.IsValid());

    std::vector<glm::vec3> restoredPath;
    EXPECT_TRUE(restoredQuery.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, restoredPath));
    EXPECT_EQ(restoredPath.size(), origPath.size());
}

// ============================================================================
// Positive tests with real navmesh
// ============================================================================

TEST(NavMeshQueryPositiveTest, FindNearestPointOnPlane)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);
    NavMeshQuery query(navMesh);
    ASSERT_TRUE(query.IsValid());

    glm::vec3 nearest{};
    EXPECT_TRUE(query.FindNearestPoint({ 0.0f, 1.0f, 0.0f }, 5.0f, nearest));
    EXPECT_NEAR(nearest.y, 0.0f, 0.5f);
}

TEST(NavMeshQueryPositiveTest, RaycastUnobstructed)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);
    NavMeshQuery query(navMesh);
    ASSERT_TRUE(query.IsValid());

    glm::vec3 hitPoint{};
    // Raycast across the flat plane — no obstacle, so Raycast returns false (no hit)
    EXPECT_FALSE(query.Raycast({ -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, hitPoint));
    // hitPoint should be set to end position when unobstructed
    EXPECT_NEAR(hitPoint.x, 5.0f, 0.5f);
}

TEST(NavMeshQueryPositiveTest, IsPointOnNavMeshCenter)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);
    NavMeshQuery query(navMesh);
    ASSERT_TRUE(query.IsValid());

    EXPECT_TRUE(query.IsPointOnNavMesh({ 0.0f, 0.0f, 0.0f }));
}

TEST(NavMeshQueryPositiveTest, IsPointOffNavMeshFarAway)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);
    NavMeshQuery query(navMesh);
    ASSERT_TRUE(query.IsValid());

    EXPECT_FALSE(query.IsPointOnNavMesh({ 1000.0f, 0.0f, 1000.0f }));
}

TEST(NavMeshQueryPositiveTest, CustomQueryBudget)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);
    NavMeshQuery query(navMesh, 512);
    ASSERT_TRUE(query.IsValid());

    std::vector<glm::vec3> path;
    EXPECT_TRUE(query.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, path));
    EXPECT_FALSE(path.empty());
}

TEST(CrowdManagerPositiveTest, AddAgentWithValidNavMesh)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);

    CrowdManager crowd;
    crowd.Initialize(navMesh);
    ASSERT_TRUE(crowd.IsValid());

    NavAgentComponent agent;
    i32 id = crowd.AddAgent({ 0.0f, 0.0f, 0.0f }, agent);
    EXPECT_GE(id, 0);
    EXPECT_TRUE(crowd.IsAgentActive(id));
    EXPECT_EQ(crowd.GetActiveAgentCount(), 1);
}

TEST(CrowdManagerPositiveTest, GetAgentPositionAfterAdd)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);

    CrowdManager crowd;
    crowd.Initialize(navMesh);

    NavAgentComponent agent;
    i32 id = crowd.AddAgent({ 3.0f, 0.0f, 3.0f }, agent);
    ASSERT_GE(id, 0);

    glm::vec3 pos{};
    EXPECT_TRUE(crowd.GetAgentPosition(id, pos));
    EXPECT_NEAR(pos.x, 3.0f, 1.0f);
    EXPECT_NEAR(pos.z, 3.0f, 1.0f);
}

TEST(CrowdManagerPositiveTest, RemoveAgentDecrementsCount)
{
    auto navMesh = BuildFlatPlaneNavMesh();
    ASSERT_NE(navMesh, nullptr);

    CrowdManager crowd;
    crowd.Initialize(navMesh);

    NavAgentComponent agent;
    i32 id = crowd.AddAgent({ 0.0f, 0.0f, 0.0f }, agent);
    ASSERT_GE(id, 0);
    EXPECT_EQ(crowd.GetActiveAgentCount(), 1);

    crowd.RemoveAgent(id);
    EXPECT_EQ(crowd.GetActiveAgentCount(), 0);
    EXPECT_FALSE(crowd.IsAgentActive(id));
}

TEST(NavAgentComponentTest, CopyOnlySerializedFields)
{
    NavAgentComponent original;
    original.m_MaxSpeed = 10.0f;
    original.m_HasTarget = true;
    original.m_HasPath = true;
    original.m_CrowdAgentId = 42;

    NavAgentComponent copy(original);
    EXPECT_FLOAT_EQ(copy.m_MaxSpeed, 10.0f);
    EXPECT_FALSE(copy.m_HasTarget);
    EXPECT_FALSE(copy.m_HasPath);
    EXPECT_EQ(copy.m_CrowdAgentId, -1);
}

TEST(NavAgentComponentTest, CopyAssignmentResetsRuntime)
{
    NavAgentComponent original;
    original.m_Acceleration = 20.0f;
    original.m_HasTarget = true;
    original.m_CrowdAgentId = 7;

    NavAgentComponent dest;
    dest.m_HasTarget = true;
    dest.m_CrowdAgentId = 99;

    dest = original;
    EXPECT_FLOAT_EQ(dest.m_Acceleration, 20.0f);
    EXPECT_FALSE(dest.m_HasTarget);
    EXPECT_EQ(dest.m_CrowdAgentId, -1);
}

TEST(NavAgentComponentTest, MoveConstructorResetsRuntime)
{
    NavAgentComponent original;
    original.m_Radius = 1.5f;
    original.m_HasPath = true;
    original.m_CrowdAgentId = 10;

    NavAgentComponent moved(std::move(original));
    EXPECT_FLOAT_EQ(moved.m_Radius, 1.5f);
    EXPECT_FALSE(moved.m_HasPath);
    EXPECT_EQ(moved.m_CrowdAgentId, -1);
}
