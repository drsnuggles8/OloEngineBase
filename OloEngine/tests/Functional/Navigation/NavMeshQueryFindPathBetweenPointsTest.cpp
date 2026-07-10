#include "OloEnginePCH.h"

// =============================================================================
// NavMeshQueryFindPathBetweenPointsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   NavMeshGenerator (Recast bake) × Scene::SetNavMesh (auto-creates query) ×
//   NavMeshQuery::FindPath (Detour string-pulling). The query layer sits
//   between gameplay (NavAgent / scripting) and the navmesh asset; if it
//   regresses, agents stop pathing even when the bake is valid. This test
//   isolates the QUERY API by calling FindPath directly with two known
//   walkable points, removing the NavigationSystem from the picture.
//
// Scenario: same thin-floor navmesh as NavAgentReachesTargetTest, but
// without any NavAgentComponent. After the bake:
//   - Scene::GetNavMeshQuery() is non-null and IsValid() is true.
//   - FindPath(start, end) returns true and yields ≥2 corner points.
//   - The first corner is close to `start` (FindPath clamps to nearest
//     poly), the last is close to `end`.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class NavMeshQueryFindPathBetweenPointsTest : public FunctionalTest
{
  protected:
    static constexpr glm::vec3 kStart{ -4.0f, 0.2f, 0.0f };
    static constexpr glm::vec3 kEnd{ 4.0f, 0.2f, 0.0f };

    void BuildScene() override
    {
        // Thin floor (same recipe as NavAgentReachesTargetTest — thicker
        // boxes confuse Recast's ledge-span filter and produce 0 polys).
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.05f, 0.0f };
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        BoxCollider3DComponent col;
        col.m_HalfExtents = { 20.0f, 0.05f, 5.0f };
        floor.AddComponent<BoxCollider3DComponent>(col);
        floor.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();

        NavMeshSettings settings;
        const auto navMesh = NavMeshGenerator::Generate(
            &GetScene(), settings,
            /*boundsMin=*/glm::vec3(-25.0f, -2.0f, -7.0f),
            /*boundsMax=*/glm::vec3(25.0f, 5.0f, 7.0f));
        ASSERT_TRUE(navMesh && navMesh->IsValid())
            << "NavMeshGenerator failed on a thin-floor scene — pre-condition broken.";
        GetScene().SetNavMesh(navMesh);
    }
};

TEST_F(NavMeshQueryFindPathBetweenPointsTest, DirectFindPathReturnsPolylineConnectingStartAndEnd)
{
    auto* query = GetScene().GetNavMeshQuery();
    ASSERT_NE(query, nullptr)
        << "Scene::SetNavMesh did not auto-create a NavMeshQuery.";
    ASSERT_TRUE(query->IsValid())
        << "NavMeshQuery::IsValid is false despite Generate having succeeded — "
           "NavMeshQuery::Initialize was not called or dtNavMeshQuery::init failed.";

    std::vector<glm::vec3> path;
    const FindPathResult result = query->FindPath(kStart, kEnd, path);
    ASSERT_EQ(result, FindPathResult::Complete)
        << "FindPath did not report a complete path on a known-walkable straight-shot path.";
    ASSERT_GE(path.size(), 2u)
        << "FindPath reported Complete but produced fewer than 2 corners — "
           "an empty/degenerate path that no agent can follow.";

    const glm::vec3 first = path.front();
    const glm::vec3 last = path.back();

    auto distXZ = [](const glm::vec3& a, const glm::vec3& b)
    {
        const f32 dx = a.x - b.x;
        const f32 dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    };

    EXPECT_LT(distXZ(first, kStart), 1.0f)
        << "first path corner is far from the start query point — Detour's "
           "string-pulling didn't anchor to the requested origin.";
    EXPECT_LT(distXZ(last, kEnd), 1.0f)
        << "last path corner is far from the end query point — the path "
           "terminates somewhere else (poly clamping is broken or the "
           "navmesh has a hole the end falls into).";
}
