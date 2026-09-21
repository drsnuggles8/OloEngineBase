#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// AnimatedSurfaceLODTest — Functional Test (issue #1227).
//
// Cross-subsystem seam under test:
//   Scene's frame boundary × conventional mesh LOD × the deformation history.
//
//   Before this issue, conventional LOD could not reach a skinned or morphing
//   mesh at all, and it took FOUR separate refusals to keep it out: the three
//   generators in MeshOptimization each returned early for a source with a
//   skeleton, morph targets or a bone table; ModelImporter::EnsureAutoLODGroup
//   refused the same; and the animated draw paths never called SelectLODMesh.
//   Every imported character drew at LOD 0 however far away it was, and nothing
//   said so.
//
//   Lifting that has a consequence the bone half already learned the hard way.
//   A level switch replaces the whole surface — different vertex count,
//   different topology, different morph deltas — so the previous frame's pose
//   belongs to a mesh that is no longer being drawn. Reprojecting through it
//   produces a velocity measured between two unrelated surfaces, which TAA and
//   motion blur smear. The switch therefore has to be an explicit, attributed
//   history rejection, and that is what these tests pin.
//
//   Driven through the real Scene::OnUpdateRuntime, and with no graphics device:
//   the level is chosen from the entity's projected size, so moving the ENTITY
//   away from the default view position is the same measurement as moving the
//   camera, and needs no renderer.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u32 kGridSize = 32; // 2048 triangles — enough to simplify several times

    // A dense skinned + morphing grid. Every vertex is weighted to bone 0 with a
    // real weight, so MeshSource::HasBoneInfluences() reports true and a level
    // that dropped the stream is distinguishable from one that kept it.
    [[nodiscard]] Ref<MeshSource> MakeSkinnedMorphingGrid()
    {
        const u32 side = kGridSize + 1;
        std::vector<Vertex> vertices;
        vertices.reserve(static_cast<sizet>(side) * side);
        for (u32 z = 0; z < side; ++z)
        {
            for (u32 x = 0; x < side; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kGridSize);
                const f32 fz = static_cast<f32>(z) / static_cast<f32>(kGridSize);
                // A gentle dome rather than a plane: a flat grid simplifies to two
                // triangles in one step and the chain has nowhere to go.
                const f32 fy = 0.5f * std::sin(fx * 3.14159f) * std::sin(fz * 3.14159f);
                vertices.emplace_back(glm::vec3(fx * 4.0f - 2.0f, fy, fz * 4.0f - 2.0f),
                                      glm::vec3(0.0f, 1.0f, 0.0f),
                                      glm::vec2(fx, fz));
            }
        }

        std::vector<u32> indices;
        indices.reserve(static_cast<sizet>(kGridSize) * kGridSize * 6);
        for (u32 z = 0; z < kGridSize; ++z)
        {
            for (u32 x = 0; x < kGridSize; ++x)
            {
                const u32 i0 = z * side + x;
                const u32 i1 = i0 + 1;
                const u32 i2 = i0 + side;
                const u32 i3 = i2 + 1;
                indices.insert(indices.end(), { i0, i1, i2, i1, i3, i2 });
            }
        }

        auto source = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

        source->SetSkeleton(Fixtures::MakeSingleBoneSkeleton());
        auto& bones = source->GetBoneInfluences();
        bones.SetNum(source->GetVertices().Num());
        for (i32 i = 0; i < bones.Num(); ++i)
        {
            BoneInfluence influence;
            influence.m_BoneIDs[0] = 0u;
            influence.m_Weights[0] = 1.0f;
            bones[i] = influence;
        }

        auto targets = Ref<MorphTargetSet>::Create();
        MorphTarget bulge("Bulge", static_cast<u32>(source->GetVertices().Num()));
        for (i32 i = 0; i < source->GetVertices().Num(); ++i)
        {
            bulge.Vertices[static_cast<sizet>(i)].DeltaPosition = glm::vec3(0.0f, 0.25f, 0.0f);
        }
        targets->AddTarget(bulge);
        source->SetMorphTargets(targets);

        // A REAL submesh spanning the whole mesh. LOD selection projects
        // Mesh::GetBoundingBox(), which is the SUBMESH's box — a default-constructed
        // Submesh spans zero vertices, so its box is empty, the projected pixel size
        // is 0 at every distance, and every level's `pixelSize * Error` sits under
        // the threshold: the selector then picks the coarsest level from one metre
        // away and never changes it again.
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_VertexCount = static_cast<u32>(source->GetVertices().Num());
        submesh.m_IndexCount = static_cast<u32>(source->GetIndices().Num());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = true;
        source->AddSubmesh(submesh);

        // MeshSource::Build() computes bounds only AFTER its graphics-device check,
        // and there is no device here, so nothing would ever fill them in.
        source->CalculateBounds();
        source->CalculateSubmeshBounds();

        return source;
    }
} // namespace

class AnimatedSurfaceLODTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // GenerateAutoLODGroup registers each level as a memory-only asset and
        // SelectLODMesh resolves it back, so the levels need a live asset manager.
        EnableAssetManager({});

        m_Entity = GetScene().CreateEntity("SkinnedSubject");

        Ref<MeshSource> source = MakeSkinnedMorphingGrid();
        m_Entity.AddComponent<MeshComponent>(source);
        m_Entity.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());
        m_Entity.AddComponent<MorphTargetComponent>();

        auto baseMesh = Ref<Mesh>::Create(source, 0);
        const AssetHandle baseHandle = AssetManager::AddMemoryOnlyAsset(baseMesh);

        auto& lod = m_Entity.AddComponent<LODGroupComponent>();
        lod.m_LODGroup = MeshOptimization::GenerateAutoLODGroup(*source, baseHandle);
        lod.m_AutoGenerated = true;
    }

    void PlaceAt(f32 distance)
    {
        m_Entity.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, distance };
    }

    [[nodiscard]] LODGroupComponent& Lod()
    {
        return m_Entity.GetComponent<LODGroupComponent>();
    }

    [[nodiscard]] SkeletonData& Skeleton()
    {
        return *m_Entity.GetComponent<SkeletonComponent>().m_Skeleton;
    }

    Entity m_Entity;
};

TEST_F(AnimatedSurfaceLODTest, ASkinnedSourceGetsAnLODChainAtAll)
{
    // The headline of the LOD half. Before #1227 every generator refused a source
    // with a skeleton or morph targets, so this chain could not exist.
    ASSERT_GT(Lod().m_LODGroup.Levels.Num(), 1u)
        << "no LOD chain was generated for a skinned + morphing source — conventional LOD "
           "cannot reach the animated surface while that is true";

    // And every level has to carry the streams, or drawing one renders an
    // unskinned, undeformable mesh.
    for (sizet level = 1; level < Lod().m_LODGroup.Levels.Num(); ++level)
    {
        auto lodMesh = AssetManager::GetAsset<Mesh>(Lod().m_LODGroup.Levels[level].MeshHandle);
        ASSERT_TRUE(lodMesh) << "level " << level << " does not resolve to a mesh";
        ASSERT_TRUE(lodMesh->GetMeshSource());
        EXPECT_TRUE(lodMesh->GetMeshSource()->HasBoneInfluences())
            << "level " << level
            << " has no bone influences; the shared producer reads all-zero weights as an "
               "UNSKINNED vertex, so the character snaps to its rest pose at this level";
        EXPECT_TRUE(lodMesh->GetMeshSource()->HasMorphTargets())
            << "level " << level << " lost its morph deltas, so the face stops expressing at range";
    }
}

TEST_F(AnimatedSurfaceLODTest, TheSelectedLevelCoarsensWithDistance)
{
    PlaceAt(3.0f);
    RunFrames(3);
    const i32 nearLevel = Lod().m_ActiveAnimatedLOD;

    ASSERT_GE(nearLevel, 0)
        << "no level was resolved for an animated entity at all — Scene::SelectAnimatedSurfaceLOD "
           "is not reaching it, and the morph pass and the draw loop are free to disagree about "
           "which mesh the surface is";

    PlaceAt(400.0f);
    RunFrames(3);
    const i32 farLevel = Lod().m_ActiveAnimatedLOD;

    EXPECT_GT(farLevel, nearLevel)
        << "the animated surface stayed at level " << nearLevel << " from 3 units to 400 — a "
                                                                   "skinned entity is still pinned to whatever level it started on";
}

TEST_F(AnimatedSurfaceLODTest, ALevelSwitchRejectsDeformationHistory)
{
    PlaceAt(3.0f);
    RunFrames(6);
    ASSERT_TRUE(Skeleton().HasBoneHistory()) << "precondition: history established while stationary";

    const auto& stats = Animation::SkeletalDeformationSystem::GetStats();
    const u32 before = stats.HistoryResetsMeshTopologyChanged;
    const i32 nearLevel = Lod().m_ActiveAnimatedLOD;

    // TWO frames, not one, and that is a property of the design rather than slack.
    // Scene::GetWorldTransform reads WorldTransformComponent::WorldMatrix, a cache
    // that PropagateWorldTransforms refreshes inside the TICK — so the frame
    // boundary, which runs before the tick, selects from where the entity was at
    // the end of the previous frame. That is the position the previous frame
    // actually DREW, which is the right frame of reference for a level; it just
    // means a teleport takes one frame to be reflected.
    PlaceAt(400.0f);
    RunFrames(2);

    ASSERT_NE(Lod().m_ActiveAnimatedLOD, nearLevel) << "precondition: the level actually switched";

    EXPECT_GT(stats.HistoryResetsMeshTopologyChanged, before)
        << "an LOD switch on an animated entity was not recorded as a topology discontinuity — the "
           "previous pose belongs to a mesh that is no longer drawn, and reprojecting through it "
           "emits a velocity between two unrelated surfaces";
    EXPECT_FALSE(Skeleton().HasBoneHistory())
        << "the bone history survived a level switch, so the shaders were handed a previous pose "
           "measured on a different mesh";
}

TEST_F(AnimatedSurfaceLODTest, ALevelSwitchDropsTheCachedBaseSurface)
{
    auto& morph = m_Entity.GetComponent<MorphTargetComponent>();
    morph.SetWeight("Bulge", 1.0f);

    PlaceAt(3.0f);
    RunFrames(6);
    ASSERT_TRUE(morph.HasCachedBaseSurface()) << "precondition: a base surface was cached";
    const u32 nearVertexCount = morph.BaseCacheVertexCount;

    PlaceAt(400.0f);
    RunFrames(4);

    // The coarse level is a different mesh with its own vertex array. Reusing the
    // fine level's cached base surface writes the wrong rest positions into it —
    // in range, silent, and wrong.
    EXPECT_NE(morph.BaseCacheVertexCount, nearVertexCount)
        << "the base-surface cache was carried across a level switch unchanged";
    EXPECT_TRUE(morph.HasCachedBaseSurface())
        << "the cache was dropped at the switch but never rebuilt for the level now being drawn";
}
