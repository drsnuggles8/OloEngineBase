#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// ClothSimulationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Cloth (ClothComponent → Jolt SoftBodySharedSettings / soft body) × Physics3D
//   (gravity integration + soft-body-vs-rigid collision) driven by the real
//   Scene::OnPhysics3DStart / OnUpdateRuntime path (issue #460, first slice).
//
//   Before #460 Jolt's soft-body shape was rejected outright ("defaulting to Box").
//   These tests pin the new wiring — a ClothComponent produces a Jolt soft body that:
//     (a) falls under gravity and rests ON a static floor (no fall-through), and
//     (b) hangs from a pinned edge (the pinned row holds while the free part sags).
//
// Headless: the soft body + vertex readback are GPU-free (JoltScene::GetClothVertices),
// so no GL context is needed. The on-screen drape is covered by the editor visual pass.
//
// A third test (ClothWindTest, below) pins the wind-coupling slice (issue #460, second
// slice): ClothWindSystem samples WindSystem::GetWindAtPoint and queues it as a uniform
// per-body force on the cloth's Jolt soft body (JoltScene::ApplyClothWindForce), scaled by
// ClothComponent::m_WindInfluence. Two pinned cloths sit side by side under the same
// scene-level WindSettings, one with m_WindInfluence = 0 (a no-wind control) and one with
// m_WindInfluence = 1 — proving the effect is driven by the field, not just "wind exists".
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    struct VertBounds
    {
        f32 minY = std::numeric_limits<f32>::max();
        f32 maxY = std::numeric_limits<f32>::lowest();
        f32 avgY = 0.0f;
        bool allFinite = true;
        sizet count = 0;
    };

    VertBounds ComputeBounds(const std::vector<glm::vec3>& positions)
    {
        VertBounds b;
        b.count = positions.size();
        f32 sum = 0.0f;
        for (const glm::vec3& p : positions)
        {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                b.allFinite = false;
            b.minY = std::min(b.minY, p.y);
            b.maxY = std::max(b.maxY, p.y);
            sum += p.y;
        }
        if (!positions.empty())
            b.avgY = sum / static_cast<f32>(positions.size());
        return b;
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A free (unpinned) cloth dropped above a static floor. Proves the soft body
// falls under gravity and collides with rigid geometry instead of passing through.
// ─────────────────────────────────────────────────────────────────────────────
class ClothFallsOntoFloorTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor: a wide, thin box whose top surface sits at kFloorTop.
        m_Floor = GetScene().CreateEntity("Floor");
        m_Floor.GetComponent<TransformComponent>().Translation = { 0.0f, kFloorTop - kFloorHalfY, 0.0f };
        auto& floorBody = m_Floor.AddComponent<Rigidbody3DComponent>();
        floorBody.m_Type = BodyType3D::Static;
        auto& floorCol = m_Floor.AddComponent<BoxCollider3DComponent>();
        floorCol.m_HalfExtents = { 10.0f, kFloorHalfY, 10.0f };

        // Free cloth suspended above the floor centre.
        m_Cloth = GetScene().CreateEntity("Cloth");
        m_Cloth.GetComponent<TransformComponent>().Translation = { 0.0f, kClothY, 0.0f };
        auto& cloth = m_Cloth.AddComponent<ClothComponent>();
        cloth.m_Columns = 12;
        cloth.m_Rows = 12;
        cloth.m_Width = 3.0f;
        cloth.m_Height = 3.0f;
        cloth.m_Mass = 1.0f;
        cloth.m_Attachment = ClothAttachment::None;
        cloth.m_Enabled = true;

        EnablePhysics3D();
    }

    static constexpr f32 kFloorTop = 1.0f;
    static constexpr f32 kFloorHalfY = 0.5f;
    static constexpr f32 kClothY = 6.0f;

    Entity m_Floor;
    Entity m_Cloth;
};

TEST_F(ClothFallsOntoFloorTest, FreeClothFallsAndRestsOnFloor)
{
    const UUID clothID = m_Cloth.GetUUID();

    // The cloth is live and starts well above the floor.
    const std::vector<glm::vec3>* initial = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(initial, nullptr) << "cloth soft body was not created / has no readback";
    const VertBounds start = ComputeBounds(*initial);
    ASSERT_GT(start.count, 0u);
    ASSERT_NEAR(start.avgY, kClothY, 0.25f) << "cloth should spawn at its entity Y";

    // Let it fall and settle on the static floor.
    TickFor(4.0f);

    const std::vector<glm::vec3>* settled = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(settled, nullptr);
    const VertBounds end = ComputeBounds(*settled);

    EXPECT_TRUE(end.allFinite) << "cloth vertices contain NaN/Inf after settling";
    EXPECT_LT(end.avgY, start.avgY - 2.0f) << "cloth did not fall under gravity";
    // Did NOT tunnel through the floor: the lowest particle rests at (not below) the
    // floor top, within a small penetration/vertex-radius budget.
    EXPECT_GT(end.minY, kFloorTop - 0.25f) << "cloth fell through the floor; minY=" << end.minY;
    // And it actually reached the floor rather than hanging in mid-air.
    EXPECT_LT(end.minY, kFloorTop + 0.5f) << "cloth did not settle onto the floor; minY=" << end.minY;
}

// ─────────────────────────────────────────────────────────────────────────────
// A cloth pinned along its top edge with no floor. Proves the attachment holds
// the pinned particles in place while the rest of the sheet sags under gravity.
// ─────────────────────────────────────────────────────────────────────────────
class ClothHangsFromPinnedEdgeTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Cloth = GetScene().CreateEntity("Cloth");
        m_Cloth.GetComponent<TransformComponent>().Translation = { 0.0f, kClothY, 0.0f };
        auto& cloth = m_Cloth.AddComponent<ClothComponent>();
        cloth.m_Columns = 12;
        cloth.m_Rows = 12;
        cloth.m_Width = 3.0f;
        cloth.m_Height = 3.0f;
        cloth.m_Mass = 1.0f;
        cloth.m_Attachment = ClothAttachment::TopEdge;
        cloth.m_Enabled = true;

        EnablePhysics3D();
    }

    static constexpr f32 kClothY = 8.0f;

    Entity m_Cloth;
};

TEST_F(ClothHangsFromPinnedEdgeTest, PinnedEdgeHoldsWhileFreePartSags)
{
    const UUID clothID = m_Cloth.GetUUID();

    const std::vector<glm::vec3>* initial = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(initial, nullptr);
    const VertBounds start = ComputeBounds(*initial);
    ASSERT_GT(start.count, 0u);

    TickFor(3.0f);

    const std::vector<glm::vec3>* draped = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(draped, nullptr);
    const VertBounds end = ComputeBounds(*draped);

    EXPECT_TRUE(end.allFinite) << "cloth vertices contain NaN/Inf after draping";
    // The pinned top edge stays at (roughly) the spawn height — the highest particle
    // never drops far below where the whole sheet started.
    EXPECT_GT(end.maxY, kClothY - 0.5f) << "pinned edge fell; maxY=" << end.maxY;
    // ...while the free part swings down well below the pinned edge under gravity.
    EXPECT_LT(end.minY, kClothY - 1.0f) << "cloth did not sag under gravity; minY=" << end.minY;
    // A hanging sheet has NOT free-fallen: it stays anchored, so its lowest point is
    // bounded by roughly the cloth height below the pin (not off to -infinity).
    EXPECT_GT(end.minY, kClothY - 6.0f) << "cloth appears to have detached / free-fallen; minY=" << end.minY;
}

// ─────────────────────────────────────────────────────────────────────────────
// Two pinned cloths side by side (separated along Z so their soft bodies never
// touch) under the same strong, steady sideways wind. One has m_WindInfluence = 0
// (a no-wind control — same WindSettings apply to the whole scene, but the field
// should make this cloth ignore it), the other m_WindInfluence = 1. Proves wind
// coupling is real: the full-influence cloth billows sideways in the wind
// direction while the zero-influence control hangs straight down under gravity
// alone (issue #460, wind-coupling slice).
// ─────────────────────────────────────────────────────────────────────────────
class ClothWindTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        WindSettings& wind = GetScene().GetWindSettings();
        wind.Enabled = true;
        wind.Direction = { 1.0f, 0.0f, 0.0f };
        wind.Speed = 40.0f;
        wind.GustStrength = 0.0f; // deterministic — no sinusoidal modulation

        auto makeCloth = [this](const char* name, f32 zOffset, f32 windInfluence) -> Entity
        {
            Entity e = GetScene().CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = { 0.0f, kClothY, zOffset };
            auto& cloth = e.AddComponent<ClothComponent>();
            cloth.m_Columns = 12;
            cloth.m_Rows = 12;
            cloth.m_Width = 3.0f;
            cloth.m_Height = 3.0f;
            cloth.m_Mass = 1.0f;
            cloth.m_Attachment = ClothAttachment::TopEdge;
            cloth.m_WindInfluence = windInfluence;
            cloth.m_Enabled = true;
            return e;
        };

        m_NoWindCloth = makeCloth("NoWindCloth", -4.0f, 0.0f);
        m_WindCloth = makeCloth("WindCloth", 4.0f, 1.0f);

        EnablePhysics3D();
    }

    static f32 AverageX(const std::vector<glm::vec3>& positions)
    {
        f32 sum = 0.0f;
        for (const glm::vec3& p : positions)
            sum += p.x;
        return positions.empty() ? 0.0f : sum / static_cast<f32>(positions.size());
    }

    static constexpr f32 kClothY = 6.0f;

    Entity m_NoWindCloth;
    Entity m_WindCloth;
};

TEST_F(ClothWindTest, WindInfluenceDrivesSidewaysBillowRelativeToNoWindControl)
{
    const UUID noWindID = m_NoWindCloth.GetUUID();
    const UUID windID = m_WindCloth.GetUUID();

    TickFor(4.0f);

    const std::vector<glm::vec3>* noWindVerts = GetScene().GetClothVertexPositions(noWindID);
    const std::vector<glm::vec3>* windVerts = GetScene().GetClothVertexPositions(windID);
    ASSERT_NE(noWindVerts, nullptr) << "no-wind control cloth has no live soft body";
    ASSERT_NE(windVerts, nullptr) << "wind-driven cloth has no live soft body";

    const VertBounds noWindBounds = ComputeBounds(*noWindVerts);
    const VertBounds windBounds = ComputeBounds(*windVerts);
    EXPECT_TRUE(noWindBounds.allFinite) << "no-wind cloth vertices contain NaN/Inf";
    EXPECT_TRUE(windBounds.allFinite) << "wind-driven cloth vertices contain NaN/Inf";

    // Both pinned edges should still hold near the spawn height regardless of wind.
    EXPECT_GT(noWindBounds.maxY, kClothY - 0.5f) << "no-wind cloth's pinned edge fell";
    EXPECT_GT(windBounds.maxY, kClothY - 0.5f) << "wind-driven cloth's pinned edge fell";

    const f32 noWindAvgX = AverageX(*noWindVerts);
    const f32 windAvgX = AverageX(*windVerts);

    // The zero-influence control hangs straight down under a symmetric pinned edge —
    // its average X should stay near its spawn X (0).
    EXPECT_NEAR(noWindAvgX, 0.0f, 0.5f) << "m_WindInfluence = 0 cloth still drifted sideways; avgX=" << noWindAvgX;
    // The full-influence cloth must billow measurably downwind (+X) relative to the control.
    EXPECT_GT(windAvgX - noWindAvgX, 0.35f)
        << "m_WindInfluence = 1 cloth did not billow sideways relative to the no-wind control; "
        << "windAvgX=" << windAvgX << " noWindAvgX=" << noWindAvgX;
}
