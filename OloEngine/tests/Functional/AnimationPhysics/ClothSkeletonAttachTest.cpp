#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// ClothSkeletonAttachTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Cloth (ClothComponent skeleton attachment) × Animation skeleton (bone
//   model-space transform) × Physics3D (Jolt soft-body kinematic vertex drive),
//   driven by the real Scene::OnPhysics3DStart / OnUpdateRuntime path
//   (issue #460, cape slice — the final acceptance criterion of the cloth epic:
//   "a cape attached to an animated character moves believably").
//
//   A ClothComponent can now name an attachment entity + bone; at physics start
//   its pinned vertices (the TopEdge set) are welded to that bone, and each tick
//   Scene::DriveClothAttachments drives them kinematically from the bone's world
//   transform (JoltScene::DriveClothAttachment) while the free vertices keep
//   simulating under gravity. These tests pin the wiring:
//     (a) the pinned edge FOLLOWS a moving bone (the cape tracks the character),
//     (b) the free part still sags below the pinned edge (it isn't rigidly frozen),
//     (c) an unattached cloth's pinned edge does NOT follow the bone (control:
//         the effect is driven by the attachment fields, not "a bone exists").
//
// Headless: the soft body + vertex readback + bone transform are all GPU-free, so
// no GL context is needed. The on-screen "moves believably" judgement is covered
// by the editor visual pass / ClothCapeVisualEvidenceTest.
//
// The "animated character" is modelled without a skinned-mesh asset: an entity
// carrying a SkeletonComponent whose single bone's model-space transform
// (skeleton->m_GlobalTransforms[0]) is advanced by hand each tick — exactly what
// AnimationSystem::Update writes for a real clip. This exercises the production
// bone-name → index → global-transform → world lookup without a model dependency.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // The pinned set for ClothAttachment::TopEdge is grid row 0 — particle indices
    // [0, columns). Average their world position to track the cape's attached edge.
    glm::vec3 TopEdgeAverage(const std::vector<glm::vec3>& positions, u32 columns)
    {
        glm::vec3 sum(0.0f);
        u32 n = 0;
        const u32 count = std::min<u32>(columns, static_cast<u32>(positions.size()));
        for (u32 i = 0; i < count; ++i)
        {
            sum += positions[i];
            ++n;
        }
        return (n > 0) ? sum / static_cast<f32>(n) : glm::vec3(0.0f);
    }

    f32 MinY(const std::vector<glm::vec3>& positions)
    {
        f32 minY = std::numeric_limits<f32>::max();
        for (const glm::vec3& p : positions)
            minY = std::min(minY, p.y);
        return minY;
    }

    bool AllFinite(const std::vector<glm::vec3>& positions)
    {
        for (const glm::vec3& p : positions)
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                return false;
        return true;
    }
} // namespace

class ClothSkeletonAttachTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    { /* each test authors its own scene before EnablePhysics3D() */
    }

    // A one-bone skeleton, bone at model-space origin (identity global transform).
    static Ref<Skeleton> MakeSingleBoneSkeleton(const std::string& boneName)
    {
        Ref<Skeleton> skeleton = Ref<Skeleton>::Create();
        skeleton->m_BoneNames = { boneName };
        skeleton->m_ParentIndices = { -1 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms = { glm::mat4(1.0f) };
        return skeleton;
    }

    // Author a pinned cape hanging at (0, kClothY, 0), optionally welded to `boneEntity`'s
    // bone. Returns the cloth entity. columns/rows kept modest so the sim settles quickly.
    Entity MakeCape(const char* name, UUID attachEntity, const std::string& boneName)
    {
        Entity e = GetScene().CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = { 0.0f, kClothY, 0.0f };
        auto& cloth = e.AddComponent<ClothComponent>();
        cloth.m_Columns = 10;
        cloth.m_Rows = 10;
        cloth.m_Width = 2.0f;
        cloth.m_Height = 2.0f;
        cloth.m_Mass = 1.0f;
        cloth.m_Attachment = ClothAttachment::TopEdge;
        cloth.m_AttachmentEntity = attachEntity;
        cloth.m_AttachmentBone = boneName;
        cloth.m_Enabled = true;
        return e;
    }

    static constexpr f32 kClothY = 6.0f;

    Ref<Skeleton> m_Skeleton;
    Entity m_Character;
};

// -----------------------------------------------------------------------------
// The pinned edge of an attached cape follows its bone as the bone moves, while
// the free part keeps sagging below it — the core "cape tracks the character" seam.
// -----------------------------------------------------------------------------
TEST_F(ClothSkeletonAttachTest, PinnedEdgeFollowsMovingBoneWhileFreePartSags)
{
    m_Skeleton = MakeSingleBoneSkeleton("Chest");
    m_Character = GetScene().CreateEntity("Character");
    m_Character.AddComponent<SkeletonComponent>(m_Skeleton);

    Entity cape = MakeCape("Cape", m_Character.GetUUID(), "Chest");
    const UUID capeID = cape.GetUUID();

    EnablePhysics3D();

    // Settle briefly so the free part drapes and the pinned edge welds to its rest.
    TickFor(0.5f);

    const std::vector<glm::vec3>* rest = GetScene().GetClothVertexPositions(capeID);
    ASSERT_NE(rest, nullptr) << "cape soft body was not created";
    const glm::vec3 restTop = TopEdgeAverage(*rest, 10);
    const f32 restMinY = MinY(*rest);

    // The pinned edge starts near the spawn height; the free part already sags below it.
    ASSERT_NEAR(restTop.y, kClothY, 0.6f) << "pinned edge did not start near the cloth spawn height";
    ASSERT_LT(restMinY, restTop.y - 0.4f) << "free part did not sag below the pinned edge";

    // "Animate" the bone: walk it +X and +Y over ~1.5 s, as a clip would move a chest bone.
    const glm::vec3 boneTarget(3.0f, 1.5f, 0.0f);
    const u32 frames = 90;
    for (u32 f = 0; f < frames; ++f)
    {
        const f32 t = static_cast<f32>(f + 1) / static_cast<f32>(frames);
        m_Skeleton->m_GlobalTransforms[0] = glm::translate(glm::mat4(1.0f), boneTarget * t);
        RunFrames(1);
    }
    // Let the cape catch up + re-settle at the new bone pose.
    TickFor(0.75f);

    const std::vector<glm::vec3>* moved = GetScene().GetClothVertexPositions(capeID);
    ASSERT_NE(moved, nullptr);
    EXPECT_TRUE(AllFinite(*moved)) << "cape vertices contain NaN/Inf after following the bone";

    const glm::vec3 movedTop = TopEdgeAverage(*moved, 10);

    // The pinned edge tracked the bone: it shifted by ~boneTarget from its rest position.
    EXPECT_NEAR(movedTop.x - restTop.x, boneTarget.x, 0.6f)
        << "pinned edge did not follow the bone in X; restX=" << restTop.x << " movedX=" << movedTop.x;
    EXPECT_NEAR(movedTop.y - restTop.y, boneTarget.y, 0.6f)
        << "pinned edge did not follow the bone in Y; restY=" << restTop.y << " movedY=" << movedTop.y;

    // The free part is NOT rigidly frozen to the edge — it still hangs below it under gravity.
    const f32 movedMinY = MinY(*moved);
    EXPECT_LT(movedMinY, movedTop.y - 0.4f)
        << "free part stopped sagging (cape went rigid); movedMinY=" << movedMinY << " movedTopY=" << movedTop.y;
}

// -----------------------------------------------------------------------------
// Control: the SAME bone motion must NOT drag an unattached cloth's pinned edge.
// Proves the follow is driven by the attachment fields, not merely "a bone exists".
// -----------------------------------------------------------------------------
TEST_F(ClothSkeletonAttachTest, UnattachedClothPinnedEdgeIgnoresBoneMotion)
{
    m_Skeleton = MakeSingleBoneSkeleton("Chest");
    m_Character = GetScene().CreateEntity("Character");
    m_Character.AddComponent<SkeletonComponent>(m_Skeleton);

    // No attachment entity → the pinned edge stays welded to the world.
    Entity cloth = MakeCape("WorldPinnedCloth", 0, "");
    const UUID clothID = cloth.GetUUID();

    EnablePhysics3D();
    TickFor(0.5f);

    const std::vector<glm::vec3>* rest = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(rest, nullptr);
    const glm::vec3 restTop = TopEdgeAverage(*rest, 10);

    // Drive the bone hard sideways — an attached cape would follow, this one must not.
    for (u32 f = 0; f < 90; ++f)
    {
        const f32 t = static_cast<f32>(f + 1) / 90.0f;
        m_Skeleton->m_GlobalTransforms[0] = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.5f, 0.0f) * t);
        RunFrames(1);
    }
    TickFor(0.75f);

    const std::vector<glm::vec3>* after = GetScene().GetClothVertexPositions(clothID);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(AllFinite(*after));
    const glm::vec3 afterTop = TopEdgeAverage(*after, 10);

    // The world-pinned edge held its position despite the bone flying away.
    EXPECT_NEAR(afterTop.x, restTop.x, 0.3f)
        << "world-pinned cloth drifted in X (followed a bone it isn't attached to); restX="
        << restTop.x << " afterX=" << afterTop.x;
    EXPECT_NEAR(afterTop.y, restTop.y, 0.3f)
        << "world-pinned cloth drifted in Y; restY=" << restTop.y << " afterY=" << afterTop.y;
}
