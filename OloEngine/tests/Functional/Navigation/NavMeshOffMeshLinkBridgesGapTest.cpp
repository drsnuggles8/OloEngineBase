#include "OloEnginePCH.h"

// =============================================================================
// NavMeshOffMeshLinkBridgesGapTest — Functional Test.
//
// Cross-subsystem seam under test:
//   NavMeshGenerator (Recast bake + Detour off-mesh connection wiring) ×
//   NavMeshQuery::FindPath (Detour A* + string-pulling across the connection).
//   An off-mesh link is the only mechanism by which an agent can cross a gap the
//   walkable surface can't span. If the bake-time wiring of
//   dtNavMeshCreateParams::offMeshCon* regresses, links silently vanish and
//   multi-level / jump navigation breaks while every other nav test stays green.
//
// Scenario: two thin floor platforms separated by a gap wide enough that, after
// agent-radius erosion, they form two disconnected navmesh islands. We bake the
// SAME geometry twice and contrast:
//   - WITHOUT a link: FindPath across the gap yields only a PARTIAL path — its
//     last corner is stranded on the start platform, far from the goal.
//   - WITH a bidirectional link spanning the gap: FindPath now reaches the goal
//     (last corner lands on the destination platform).
//
// The contrast is the assertion: the link, and only the link, closes the gap.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/OffMeshLink.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class NavMeshOffMeshLinkBridgesGapTest : public FunctionalTest
{
  protected:
    // Two platforms at y≈0 with a gap in x ∈ [-2, 2]. After radius erosion the
    // walkable areas are roughly [-9.5, -2.5] and [2.5, 9.5] — disconnected.
    static constexpr glm::vec3 kStart{ -8.0f, 0.2f, 0.0f }; // on platform 1
    static constexpr glm::vec3 kEnd{ 8.0f, 0.2f, 0.0f };    // on platform 2

    // Link endpoints sit clearly over each platform (inside the eroded area) and
    // span the gap between them.
    static constexpr glm::vec3 kLinkA{ -3.5f, 0.2f, 0.0f };
    static constexpr glm::vec3 kLinkB{ 3.5f, 0.2f, 0.0f };

    void BuildScene() override
    {
        // Thin floors (the same recipe the other nav tests use — thicker boxes
        // confuse Recast's ledge-span filter and produce 0 polys).
        auto addPlatform = [this](const char* name, f32 centerX)
        {
            auto p = GetScene().CreateEntity(name);
            p.GetComponent<TransformComponent>().Translation = { centerX, -0.05f, 0.0f };
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Static;
            BoxCollider3DComponent col;
            col.m_HalfExtents = { 4.0f, 0.05f, 5.0f };
            p.AddComponent<BoxCollider3DComponent>(col);
            p.AddComponent<Rigidbody3DComponent>(body);
        };
        addPlatform("Platform1", -6.0f); // spans x ∈ [-10, -2]
        addPlatform("Platform2", 6.0f);  // spans x ∈ [2, 10]

        EnablePhysics3D();
    }

    // Bake the two-platform scene, optionally with a single bidirectional link.
    Ref<NavMesh> Bake(const std::vector<OffMeshLink>& links) const
    {
        NavMeshSettings settings;
        return NavMeshGenerator::Generate(
            const_cast<Scene*>(&GetScene()), settings,
            /*boundsMin=*/glm::vec3(-15.0f, -2.0f, -7.0f),
            /*boundsMax=*/glm::vec3(15.0f, 5.0f, 7.0f), links);
    }

    static f32 DistXZ(const glm::vec3& a, const glm::vec3& b)
    {
        const f32 dx = a.x - b.x;
        const f32 dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }
};

TEST_F(NavMeshOffMeshLinkBridgesGapTest, LinkClosesGapThatIsImpassableWithoutIt)
{
    // ---- Baseline: no link → the gap is impassable, path is partial. ----
    auto noLinkMesh = Bake({});
    ASSERT_TRUE(noLinkMesh && noLinkMesh->IsValid())
        << "bake of the two-platform scene failed — pre-condition broken.";

    NavMeshQuery noLinkQuery(noLinkMesh);
    ASSERT_TRUE(noLinkQuery.IsValid());

    std::vector<glm::vec3> noLinkPath;
    const bool noLinkFound = noLinkQuery.FindPath(kStart, kEnd, noLinkPath);
    // Detour returns a (partial) path to the nearest reachable poly even when the
    // goal is unreachable, so the bool alone isn't the signal — the path must NOT
    // arrive at the goal.
    const bool noLinkReachedGoal = noLinkFound && !noLinkPath.empty() && DistXZ(noLinkPath.back(), kEnd) < 1.5f;
    EXPECT_FALSE(noLinkReachedGoal)
        << "without an off-mesh link the two platforms must be disconnected, but "
           "FindPath reported a path reaching the goal — the gap isn't actually a gap, "
           "so this test can't prove the link does anything.";

    // ---- With link: the same geometry is now traversable end-to-end. ----
    std::vector<OffMeshLink> links;
    links.emplace_back(kLinkA, kLinkB, /*radius=*/0.6f, /*bidirectional=*/true);

    auto linkedMesh = Bake(links);
    ASSERT_TRUE(linkedMesh && linkedMesh->IsValid())
        << "bake with an off-mesh link failed — link wiring broke the Detour build.";

    NavMeshQuery linkedQuery(linkedMesh);
    ASSERT_TRUE(linkedQuery.IsValid());

    std::vector<glm::vec3> linkedPath;
    const bool linkedFound = linkedQuery.FindPath(kStart, kEnd, linkedPath);
    ASSERT_TRUE(linkedFound)
        << "FindPath returned false with an off-mesh link present.";
    ASSERT_GE(linkedPath.size(), 2u);

    EXPECT_LT(DistXZ(linkedPath.front(), kStart), 1.5f)
        << "path does not start near the requested origin.";
    EXPECT_LT(DistXZ(linkedPath.back(), kEnd), 1.5f)
        << "off-mesh link is present but FindPath still didn't reach the goal — the "
           "dtOffMeshConnection wasn't baked in, wasn't connected to a poly (endpoint "
           "too far from the walkable area, vertically or horizontally), or its flags/"
           "area excluded it from the default query filter.";

    // The route must actually go through the link: some corner lands near each
    // endpoint. (Detour emits off-mesh connection endpoints as straight-path corners.)
    bool nearA = false;
    bool nearB = false;
    for (const auto& corner : linkedPath)
    {
        if (DistXZ(corner, kLinkA) < 1.0f)
            nearA = true;
        if (DistXZ(corner, kLinkB) < 1.0f)
            nearB = true;
    }
    EXPECT_TRUE(nearA && nearB)
        << "path reached the goal but not via the link endpoints — the contrast with "
           "the no-link case would be meaningless.";
}
