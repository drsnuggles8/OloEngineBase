#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// NavMeshGeneratorBoundsValidationTest — Functional Test.
//
// Subsystem under test:
//   NavMeshGenerator::Generate (public API) — bounds validation guard.
//
// Regression target: Generate fed boundsMin/boundsMax straight into
// rcCalcGridSize with no finiteness / ordering check. A NaN, Inf, or inverted
// (max <= min) bound yields a garbage or enormous grid size and blows up the
// heightfield allocation. The only production caller passes sane bounds, but
// Generate is public — the guard makes bad bounds fail cleanly (nullptr) rather
// than crash or allocate wildly.
//
// The bounds check lives AFTER scene-geometry collection, so the scene must
// contain walkable geometry to reach it — hence the static floor + Physics3D.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

class NavMeshGeneratorBoundsValidationTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.05f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 20.0f, 0.05f, 5.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        EnablePhysics3D();
    }

    Ref<NavMesh> Bake(const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        NavMeshSettings settings;
        return NavMeshGenerator::Generate(&GetScene(), settings, boundsMin, boundsMax);
    }

    static constexpr glm::vec3 kValidMin{ -25.0f, -2.0f, -7.0f };
    static constexpr glm::vec3 kValidMax{ 25.0f, 5.0f, 7.0f };
};

// Control: sane bounds still bake successfully — the guard doesn't reject valid input.
TEST_F(NavMeshGeneratorBoundsValidationTest, ValidBoundsProduceMesh)
{
    auto mesh = Bake(kValidMin, kValidMax);
    ASSERT_TRUE(mesh && mesh->IsValid())
        << "valid, finite, ordered bounds were rejected — the guard is too strict.";
}

TEST_F(NavMeshGeneratorBoundsValidationTest, NaNBoundsRejected)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_EQ(Bake(glm::vec3(nan, -2.0f, -7.0f), kValidMax), nullptr)
        << "NaN in boundsMin was not rejected.";
    EXPECT_EQ(Bake(kValidMin, glm::vec3(25.0f, nan, 7.0f)), nullptr)
        << "NaN in boundsMax was not rejected.";
}

TEST_F(NavMeshGeneratorBoundsValidationTest, InfiniteBoundsRejected)
{
    const f32 inf = std::numeric_limits<f32>::infinity();
    EXPECT_EQ(Bake(glm::vec3(-inf, -2.0f, -7.0f), kValidMax), nullptr)
        << "-Inf in boundsMin was not rejected.";
    EXPECT_EQ(Bake(kValidMin, glm::vec3(inf, 5.0f, 7.0f)), nullptr)
        << "+Inf in boundsMax was not rejected.";
}

TEST_F(NavMeshGeneratorBoundsValidationTest, InvertedBoundsRejected)
{
    // max < min on every axis.
    EXPECT_EQ(Bake(kValidMax, kValidMin), nullptr)
        << "fully inverted bounds (max < min) were not rejected.";
    // Inverted on a single axis is enough to reject.
    EXPECT_EQ(Bake(kValidMin, glm::vec3(-30.0f, 5.0f, 7.0f)), nullptr)
        << "bounds inverted on the x axis were not rejected.";
}

TEST_F(NavMeshGeneratorBoundsValidationTest, DegenerateEqualBoundsRejected)
{
    // A zero-extent axis (max == min) produces a zero-width grid — reject it.
    EXPECT_EQ(Bake(kValidMin, glm::vec3(-25.0f, 5.0f, 7.0f)), nullptr)
        << "zero-extent x axis (max == min) was not rejected.";
}
