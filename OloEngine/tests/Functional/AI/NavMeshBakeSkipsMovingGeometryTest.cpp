#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// NavMeshBakeSkipsMovingGeometryTest — Functional Test (found on issue #1559).
//
// Seam: Scene::BakeNavMesh × NavMeshGenerator::CollectSceneGeometry × the
// terrain height field × Rigidbody3DComponent.
//
// A navmesh is static. The bake used to take every collider and mesh as static
// geometry, so a dynamic body (a ball on a lawn, a crate) carved a permanent
// hole where it lay at bake time, and an agent's own body carved one under the
// agent. And it read a terrain's height field only if the RENDERED update had
// built it, so a bake before the first rendered frame (the packaged runtime's
// start, a server) found no ground and failed.
//
// Scenario: a flat terrain that has never been rendered; a 2 m DYNAMIC box on
// one side, the same STATIC box on the other (the control: static geometry
// still carves).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class NavMeshBakeSkipsMovingGeometryTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        Entity ground = GetScene().CreateEntity("Ground");
        ground.GetComponent<TransformComponent>().Translation = { -10.0f, 0.0f, -10.0f };
        auto& terrain = ground.AddComponent<TerrainComponent>();
        terrain.m_WorldSizeX = 20.0f;
        terrain.m_WorldSizeZ = 20.0f;
        terrain.m_HeightScale = 1.0f;
        terrain.m_CollisionEnabled = false;
        ASSERT_FALSE(terrain.m_TerrainData) << "the test is about a terrain no rendered frame has built";

        const auto box = [this](const char* name, const glm::vec3& at, BodyType3D type)
        {
            Entity e = GetScene().CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = at;
            BoxCollider3DComponent shape;
            shape.m_HalfExtents = { 1.0f, 1.0f, 1.0f };
            e.AddComponent<BoxCollider3DComponent>(shape);
            Rigidbody3DComponent body;
            body.m_Type = type;
            e.AddComponent<Rigidbody3DComponent>(body);
        };
        box("DynamicCrate", { -5.0f, 1.0f, 0.0f }, BodyType3D::Dynamic);
        box("StaticCrate", { 5.0f, 1.0f, 0.0f }, BodyType3D::Static);

        Entity bounds = GetScene().CreateEntity("Bounds");
        NavMeshBoundsComponent area;
        area.m_Min = { -9.0f, -1.0f, -9.0f };
        area.m_Max = { 9.0f, 3.0f, 9.0f };
        bounds.AddComponent<NavMeshBoundsComponent>(area);
    }
};

TEST_F(NavMeshBakeSkipsMovingGeometryTest, ADynamicBodyLeavesNoHoleAndAStaticOneStillCarves)
{
    ASSERT_TRUE(GetScene().BakeNavMesh()) << "the bake found no ground on a terrain the renderer never built";
    const NavMeshQuery* query = GetScene().GetNavMeshQuery();
    ASSERT_TRUE(query && query->IsValid());

    EXPECT_TRUE(query->IsPointOnNavMesh({ -5.0f, 0.0f, 0.0f }, 0.5f))
        << "the dynamic crate carved a hole where it lay at bake time";
    EXPECT_FALSE(query->IsPointOnNavMesh({ 5.0f, 0.0f, 0.0f }, 0.5f))
        << "the static crate no longer carves: static geometry must still shape the navmesh";
    EXPECT_TRUE(query->IsPointOnNavMesh({ 0.0f, 0.0f, 6.0f }, 0.5f)) << "open ground is not walkable";
}
