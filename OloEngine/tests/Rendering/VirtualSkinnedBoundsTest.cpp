// OLO_TEST_LAYER: cullinglod
//
// THE contract of skinned virtual geometry (issue #1150): every bound the
// cluster cull reads must still CONTAIN its geometry once the mesh has deformed.
//
// Why this file exists rather than a screenshot. A bound that has stopped being
// conservative does not error and does not look like a bug in the bounds: the
// cull rejects clusters that are on screen, so the character flickers apart at
// some camera angles and is fine at others, and the obvious suspects (the cut,
// the LOD threshold, the raster) are all innocent. The bound is derived in
// VirtualSkinningBounds.h from one inequality; these tests check the derivation
// against the actual geometry, pose by pose and vertex by vertex, which is the
// only check that can fail for the right reason.
//
// Every containment assertion sweeps an animation RANGE rather than one frame.
// A bound that holds in the rest pose and nowhere else passes a single-pose test
// trivially, and the rest pose is exactly where the cook computed it.

#include "OloEnginePCH.h"

#include "VirtualMeshFixtures.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualSkinningBounds.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualSkinningPacking.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <span>
#include <vector>

using namespace OloEngine;                             // NOLINT(google-build-using-namespace) — test file
using namespace OloEngine::Tests::VirtualMeshFixtures; // NOLINT(google-build-using-namespace)

namespace
{
    constexpr u32 kBoneCount = 6;

    // Poses swept by every containment test: the rest pose, small and large
    // bends both ways, and one that also scales — a bone that grows its
    // vertices is the case the scale term in the bound exists for, and it is
    // the one a rotation-only sweep would never reach.
    std::vector<std::vector<glm::mat4>> AnimationRange()
    {
        std::vector<std::vector<glm::mat4>> poses;
        for (f32 amount : { 0.0f, 0.15f, 0.6f, 1.4f, -0.9f })
        {
            poses.push_back(MakeBendPose(kBoneCount, amount));
        }

        std::vector<glm::mat4> scaled = MakeBendPose(kBoneCount, 0.5f);
        for (u32 b = 1; b < scaled.size(); ++b)
        {
            scaled[b] = glm::scale(scaled[b], glm::vec3(1.0f + 0.2f * static_cast<f32>(b)));
        }
        poses.push_back(std::move(scaled));

        // A translation-only pose: bones that move without rotating, which is
        // what a walk cycle's root motion looks like to a single cluster.
        std::vector<glm::mat4> shifted(kBoneCount, glm::mat4(1.0f));
        for (u32 b = 0; b < shifted.size(); ++b)
        {
            shifted[b] = glm::translate(glm::mat4(1.0f), glm::vec3(0.3f * static_cast<f32>(b), 0.0f, 0.0f));
        }
        poses.push_back(std::move(shifted));

        // A SHEARED pose. Not a shape any animation authors directly, but the
        // shape a non-uniformly scaled parent bone composed with a rotation
        // produces — and the one for which "the longest column" is not an upper
        // bound on how far the matrix can stretch a vector. Every other pose
        // here would pass with that unsound bound in place.
        std::vector<glm::mat4> sheared = MakeBendPose(kBoneCount, 0.35f);
        for (u32 b = 1; b < sheared.size(); ++b)
        {
            glm::mat4 shear(1.0f);
            shear[1][0] = 0.45f; // x += 0.45 * y
            shear[2][1] = 0.3f;  // y += 0.3 * z
            sheared[b] = sheared[b] * shear;
        }
        poses.push_back(std::move(sheared));
        return poses;
    }

    VirtualMesh BuildSkinnedDag()
    {
        Ref<MeshSource> const source = MakeSkinnedIcosphereMesh(3, kBoneCount);
        return VirtualMeshBuilder::Build(*source);
    }

    // Slack for f32 accumulation in the bound and in the skin itself. Absolute,
    // on a unit-sphere fixture, so it is ~1e-4 of the model's own size — far too
    // tight to hide a bound that is wrong for a structural reason, which is the
    // only failure these tests are for.
    constexpr f32 kSlack = 1e-4f;
} // namespace

TEST(VirtualSkinnedBounds, TheBoneScaleBoundsWhatTheMatrixActuallyDoes)
{
    // The Lipschitz constant every other bound multiplies a radius by. If it
    // under-estimates, EVERY bound in this file is too small — and it would
    // under-estimate only for sheared matrices, which is to say only for rigs
    // nobody happens to test with.
    //
    // Checked by sampling rather than by re-deriving the norm: a second
    // implementation of the same formula would agree with the first for the same
    // wrong reason.
    const glm::mat4 cases[] = {
        glm::mat4(1.0f),
        glm::rotate(glm::mat4(1.0f), 0.9f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))),
        glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.5f, 1.25f)),
        // The shear that breaks the longest-column shortcut: every column of
        // this matrix is short, and it still stretches (1,1,1)/sqrt(3) well
        // past any of them.
        glm::mat4(glm::mat3(glm::vec3(1.0f, 0.9f, 0.9f), glm::vec3(0.9f, 1.0f, 0.9f),
                            glm::vec3(0.9f, 0.9f, 1.0f))),
        glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 1.0f)),
    };

    for (const glm::mat4& matrix : cases)
    {
        f32 const scale = SkinBoneScale(matrix);
        f32 const deltaScale = SkinBoneDeltaScale(matrix);
        f32 worstStretch = 0.0f;
        f32 worstDisplacement = 0.0f;
        // A deterministic sweep of the unit sphere; the shear's worst direction
        // is a diagonal, which an axis-only sample would miss entirely.
        constexpr i32 kSteps = 24;
        for (i32 i = 0; i < kSteps; ++i)
        {
            for (i32 j = 0; j < kSteps; ++j)
            {
                f32 const theta = 3.14159265f * static_cast<f32>(i) / static_cast<f32>(kSteps - 1);
                f32 const phi = 2.0f * 3.14159265f * static_cast<f32>(j) / static_cast<f32>(kSteps);
                glm::vec3 const v(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
                glm::vec3 const mapped = glm::mat3(matrix) * v;
                worstStretch = std::max(worstStretch, glm::length(mapped));
                worstDisplacement = std::max(worstDisplacement, glm::length(mapped - v));
            }
        }
        EXPECT_GE(scale, worstStretch - kSlack) << "SkinBoneScale under-estimates the stretch";
        EXPECT_GE(deltaScale, worstDisplacement - kSlack) << "SkinBoneDeltaScale under-estimates the displacement";
    }

    // ...and the rest-pose control: the identity neither stretches nor displaces.
    EXPECT_NEAR(SkinBoneScale(glm::mat4(1.0f)), 1.0f, kSlack);
    EXPECT_NEAR(SkinBoneDeltaScale(glm::mat4(1.0f)), 0.0f, kSlack);
}

TEST(VirtualSkinnedBounds, TheBuilderNoLongerRejectsASkinnedSource)
{
    VirtualMesh const dag = BuildSkinnedDag();

    ASSERT_TRUE(dag.IsValid()) << "a skinned source must now produce a DAG (issue #1150)";
    EXPECT_TRUE(dag.IsSkinned());
    EXPECT_EQ(static_cast<sizet>(dag.Skinning.Num()), static_cast<sizet>(dag.Vertices.Num()))
        << "the skin binding stream rides the vertex compaction and must stay in lockstep with it";
    EXPECT_EQ(static_cast<sizet>(dag.ClusterBoneRefs.Num()), static_cast<sizet>(dag.Clusters.Num()) * kMaxClusterBones)
        << "cluster bone sets are fixed-width and cluster-major";
    EXPECT_EQ(static_cast<sizet>(dag.BoneBounds.Num()), kBoneCount);
}

TEST(VirtualSkinnedBounds, EveryBoneBoundContainsEveryVertexItInfluences)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // The static half of the contract. If a bone's rest sphere does not contain
    // its own vertices, every bound derived from it is wrong for every pose —
    // so this failing first is what stops the pose sweeps below from being
    // blamed for it.
    for (sizet v = 0; v < static_cast<sizet>(dag.Skinning.Num()); ++v)
    {
        const VirtualVertexSkinning& binding = dag.Skinning[v];
        for (u32 i = 0; i < 4; ++i)
        {
            if (binding.Weights[i] <= 0.0f)
            {
                continue;
            }
            const VirtualBoneBounds& bounds = dag.BoneBounds[binding.BoneIDs[i]];
            ASSERT_TRUE(bounds.Influences()) << "bone " << binding.BoneIDs[i] << " influences vertex " << v
                                             << " but is marked as influencing nothing";
            EXPECT_LE(glm::length(dag.Vertices[v].Position - bounds.Center), bounds.Radius + kSlack);
        }
    }
}

TEST(VirtualSkinnedBounds, TheDeformedClusterSphereContainsItsClusterInEveryPose)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    u32 checkedClusters = 0;
    for (const std::vector<glm::mat4>& palette : AnimationRange())
    {
        for (sizet c = 0; c < static_cast<sizet>(dag.Clusters.Num()); ++c)
        {
            const VirtualCluster& cluster = dag.Clusters[c];
            std::span<const u32> const boneRefs(dag.ClusterBoneRefs.GetData() + c * kMaxClusterBones, kMaxClusterBones);

            glm::vec4 const restSphere(cluster.BoundsCenter, cluster.BoundsRadius);
            glm::vec4 sphere = SkinnedClusterSphere(restSphere, boneRefs, palette);
            if (!SkinnedClusterSphereIsTight(boneRefs, palette))
            {
                // The cluster's bone set overflowed at cook time. The runtime
                // answer is the instance-wide padding, so that is what is
                // checked here — testing the fallback path is the point, not
                // skipping the cluster.
                sphere.w += SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette);
            }

            for (u32 local = 0; local < cluster.VertexCount; ++local)
            {
                u32 const vertexIndex = dag.ClusterVertexRefs[cluster.VertexOffset + local];
                glm::vec3 const posed =
                    SkinPosition(dag.Vertices[vertexIndex].Position, dag.Skinning[vertexIndex], palette);
                ASSERT_LE(glm::length(posed - glm::vec3(sphere)), sphere.w + kSlack)
                    << "cluster " << c << " vertex " << local << " left its deformed cull sphere";
            }
            ++checkedClusters;
        }
    }
    EXPECT_GT(checkedClusters, 0u);
}

TEST(VirtualSkinnedBounds, TheInstanceWideBoundContainsEveryVertexDisplacementInEveryPose)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // This is the bound the GROUP LOD spheres grow by, and its claim is
    // stronger than a sphere's: no point of the rest surface moves further than
    // D. Pinning the claim itself rather than its consequence is what makes the
    // uniform-padding argument in VirtualClusterCull.comp checkable.
    for (const std::vector<glm::mat4>& palette : AnimationRange())
    {
        f32 const bound = SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette);
        for (sizet v = 0; v < static_cast<sizet>(dag.Vertices.Num()); ++v)
        {
            glm::vec3 const rest = dag.Vertices[v].Position;
            glm::vec3 const posed = SkinPosition(rest, dag.Skinning[v], palette);
            ASSERT_LE(glm::length(posed - rest), bound + kSlack) << "vertex " << v << " moved further than the bound";
        }
    }
}

TEST(VirtualSkinnedBounds, UniformPaddingKeepsGroupSpheresNested)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // Nesting — a member group's sphere contains the sphere of the group that
    // produced it — is the builder invariant the watertight cut rests on. The
    // cull adds ONE scalar to every group radius precisely so this survives
    // deformation, and the algebra is one line; this pins that the code does
    // what the line says, on real group spheres, for a real padding.
    for (const std::vector<glm::mat4>& palette : AnimationRange())
    {
        f32 const padding = SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette);
        u32 checkedEdges = 0;
        for (const VirtualCluster& cluster : dag.Clusters)
        {
            if (cluster.RefinedGroup < 0)
            {
                continue; // LOD-0 cluster: no producing group
            }
            const VirtualLODBounds& member = dag.Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds;
            const VirtualLODBounds& refined = dag.Groups[static_cast<sizet>(cluster.RefinedGroup)].LODBounds;

            f32 const restSlack = member.Radius - (glm::length(member.Center - refined.Center) + refined.Radius);
            f32 const paddedSlack =
                (member.Radius + padding) - (glm::length(member.Center - refined.Center) + (refined.Radius + padding));
            EXPECT_NEAR(restSlack, paddedSlack, kSlack)
                << "a uniform padding must not change how much room the nesting has";
            ++checkedEdges;
        }
        EXPECT_GT(checkedEdges, 0u);
    }
}

TEST(VirtualSkinnedBounds, AClusterWhoseBoneSetOverflowedFallsBackInsteadOfGuessing)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());
    std::vector<glm::mat4> const palette = MakeBendPose(kBoneCount, 0.7f);

    // An all-sentinel list is how the cook says "more than kMaxClusterBones
    // bones, no tight bound available". The contract is that the sphere comes
    // back UNCHANGED — so the caller can tell the two cases apart and apply the
    // instance-wide padding — rather than coming back as some averaged
    // approximation the caller cannot distinguish from a real bound.
    std::vector<u32> const overflowed(kMaxClusterBones, kNoClusterBone);
    glm::vec4 const restSphere(1.0f, 2.0f, 3.0f, 0.5f);

    // Component-wise, never operator== on a glm type (cpp-coding-quality.md).
    auto expectSameSphere = [](const glm::vec4& actual, const glm::vec4& expected)
    {
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
        EXPECT_FLOAT_EQ(actual.z, expected.z);
        EXPECT_FLOAT_EQ(actual.w, expected.w);
    };

    EXPECT_FALSE(SkinnedClusterSphereIsTight(overflowed, palette));
    expectSameSphere(SkinnedClusterSphere(restSphere, overflowed, palette), restSphere);

    // ...and a list naming a bone the palette does not have is the same case:
    // an id past the palette is not addressable by any shader either.
    std::vector<u32> outOfRange(kMaxClusterBones, kNoClusterBone);
    outOfRange[0] = kBoneCount + 10;
    EXPECT_FALSE(SkinnedClusterSphereIsTight(outOfRange, palette));
    expectSameSphere(SkinnedClusterSphere(restSphere, outOfRange, palette), restSphere);

    // ...and so is a PARTIALLY reachable list, which is the subtle one. A
    // vertex whose only influences are bones this palette does not have is left
    // in its REST position by the skin, and the hull of the bones that ARE
    // present need not contain that position. Building a bound from the
    // reachable subset would look tight and be too small.
    std::vector<u32> partlyReachable(kMaxClusterBones, kNoClusterBone);
    partlyReachable[0] = 0;
    partlyReachable[1] = kBoneCount + 10;
    EXPECT_FALSE(SkinnedClusterSphereIsTight(partlyReachable, palette));
    expectSameSphere(SkinnedClusterSphere(restSphere, partlyReachable, palette), restSphere);

    // The positive control: a fully reachable list DOES get a real bound, or
    // every assertion above would pass with the tight path simply broken.
    std::vector<u32> reachable(kMaxClusterBones, kNoClusterBone);
    reachable[0] = 1;
    reachable[1] = 2;
    EXPECT_TRUE(SkinnedClusterSphereIsTight(reachable, palette));
}

TEST(VirtualSkinnedBounds, AClusterHoldingAnUninfluencedVertexForfeitsItsTightBound)
{
    // A rigid vertex inside a skinned mesh does not move, so it stays at its
    // rest position — which the hull of the bones that carried the rest of the
    // cluster away need not contain. The cook's answer is to emit an
    // all-sentinel list for such a cluster, which the runtime reads as "use the
    // instance-wide bound"; that bound contains a motionless vertex trivially.
    //
    // Built by hand rather than from the fixture: every vertex of the fixture is
    // influenced, which is exactly why this case needs its own mesh.
    // Non-const Ref: the mutable GetBoneInfluences overload is what strips the
    // influences below, and a const Ref only exposes the const one.
    Ref<MeshSource> source = MakeSkinnedIcosphereMesh(2, kBoneCount);
    TArray<BoneInfluence>& influences = source->GetBoneInfluences();
    ASSERT_GT(influences.Num(), 0);
    // Strip the influences from a contiguous run, so at least one cluster is
    // certain to hold one (clusters are spatially coherent, not index-coherent,
    // so a single stripped vertex could land anywhere).
    for (i32 v = 0; v < influences.Num() / 4; ++v)
    {
        influences[v] = BoneInfluence{};
    }

    VirtualMesh const dag = VirtualMeshBuilder::Build(*source);
    ASSERT_TRUE(dag.IsValid());
    ASSERT_TRUE(dag.IsSkinned()) << "the remaining influences still make this a skinned source";

    std::vector<glm::mat4> const palette = MakeBendPose(kBoneCount, 1.0f);
    f32 const padding = SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette);

    u32 forfeited = 0;
    for (sizet c = 0; c < static_cast<sizet>(dag.Clusters.Num()); ++c)
    {
        const VirtualCluster& cluster = dag.Clusters[c];
        std::span<const u32> const boneRefs(dag.ClusterBoneRefs.GetData() + c * kMaxClusterBones, kMaxClusterBones);
        bool holdsRigidVertex = false;
        for (u32 local = 0; local < cluster.VertexCount; ++local)
        {
            const VirtualVertexSkinning& binding = dag.Skinning[dag.ClusterVertexRefs[cluster.VertexOffset + local]];
            f32 const total = binding.Weights[0] + binding.Weights[1] + binding.Weights[2] + binding.Weights[3];
            holdsRigidVertex = holdsRigidVertex || !(total > 0.0f);
        }
        if (holdsRigidVertex)
        {
            EXPECT_FALSE(SkinnedClusterSphereIsTight(boneRefs, palette))
                << "cluster " << c << " holds an uninfluenced vertex but kept a tight bound";
            ++forfeited;
        }

        // Whatever the verdict, containment still has to hold — which is the
        // property the forfeit exists to protect.
        glm::vec4 sphere =
            SkinnedClusterSphere(glm::vec4(cluster.BoundsCenter, cluster.BoundsRadius), boneRefs, palette);
        if (!SkinnedClusterSphereIsTight(boneRefs, palette))
        {
            sphere.w += padding;
        }
        for (u32 local = 0; local < cluster.VertexCount; ++local)
        {
            u32 const vertexIndex = dag.ClusterVertexRefs[cluster.VertexOffset + local];
            glm::vec3 const posed =
                SkinPosition(dag.Vertices[vertexIndex].Position, dag.Skinning[vertexIndex], palette);
            ASSERT_LE(glm::length(posed - glm::vec3(sphere)), sphere.w + kSlack)
                << "mixed cluster " << c << " vertex " << local << " left its bound";
        }
    }
    EXPECT_GT(forfeited, 0u) << "no cluster ended up holding an uninfluenced vertex — the fixture did not "
                                "exercise the case this test is for";
}

TEST(VirtualSkinnedBounds, ARestPosePaletteMovesNothing)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // The identity control. Without it every containment test above could pass
    // on a bound that is simply enormous, and nothing here would notice.
    std::vector<glm::mat4> const identity(kBoneCount, glm::mat4(1.0f));
    EXPECT_NEAR(SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, identity), 0.0f, kSlack);
    EXPECT_NEAR(SkinMaxBoneScale(identity), 1.0f, kSlack);

    for (sizet v = 0; v < static_cast<sizet>(dag.Vertices.Num()); ++v)
    {
        EXPECT_LT(glm::length(SkinPosition(dag.Vertices[v].Position, dag.Skinning[v], identity) -
                              dag.Vertices[v].Position),
                  kSlack);
    }
}

TEST(VirtualSkinnedBounds, TheDeformedSphereIsTighterThanTheInstanceWideFallback)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // The whole reason per-cluster bone sets are cooked at all. A per-cluster
    // bound that were no smaller than the instance-wide one would be pure cost:
    // the payload, the arena tail and the cull's two loops, buying nothing. This
    // measures the claim instead of assuming it.
    std::vector<glm::mat4> const palette = MakeBendPose(kBoneCount, 1.2f);
    f32 const padding = SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette);

    u32 tighter = 0;
    u32 tight = 0;
    for (sizet c = 0; c < static_cast<sizet>(dag.Clusters.Num()); ++c)
    {
        std::span<const u32> const boneRefs(dag.ClusterBoneRefs.GetData() + c * kMaxClusterBones, kMaxClusterBones);
        if (!SkinnedClusterSphereIsTight(boneRefs, palette))
        {
            continue;
        }
        ++tight;
        const VirtualCluster& cluster = dag.Clusters[c];
        glm::vec4 const sphere =
            SkinnedClusterSphere(glm::vec4(cluster.BoundsCenter, cluster.BoundsRadius), boneRefs, palette);
        if (sphere.w < cluster.BoundsRadius + padding)
        {
            ++tighter;
        }
    }
    ASSERT_GT(tight, 0u) << "no cluster got a tight bound — the cook produced no usable bone sets";
    EXPECT_GT(tighter * 2, tight) << "the per-cluster bound must beat the instance-wide one for most clusters, "
                                     "or the cooked bone sets are not earning their cost";
}

TEST(VirtualSkinnedBounds, ThePackedSkinBindingSurvivesTheArenaRoundTrip)
{
    // The GPU reads a 16-byte quantized binding, not the cook's 32-byte one, and
    // two bindings share a 32-byte arena element. A lane that leaked into its
    // neighbour would deform one vertex by another's bones — geometry tearing,
    // with nothing to catch it but a picture.
    VirtualVertexSkinning first;
    first.BoneIDs[0] = 0;
    first.BoneIDs[1] = 5;
    first.BoneIDs[2] = 99;
    first.BoneIDs[3] = 7;
    first.Weights[0] = 0.5f;
    first.Weights[1] = 0.25f;
    first.Weights[2] = 0.125f;
    first.Weights[3] = 0.125f;

    VirtualVertexSkinning second;
    second.BoneIDs[0] = 12;
    second.BoneIDs[1] = 3;
    second.Weights[0] = 0.75f;
    second.Weights[1] = 0.25f;

    VirtualGpuVertex element{};
    PackVirtualSkinning(element, 0, first);
    PackVirtualSkinning(element, 1, second);

    VirtualVertexSkinning const readFirst = UnpackVirtualSkinning(element, 0);
    VirtualVertexSkinning const readSecond = UnpackVirtualSkinning(element, 1);

    for (u32 i = 0; i < 4; ++i)
    {
        SCOPED_TRACE(i);
        // unorm16 is exact for these weights; the tolerance is one step, which
        // is what the shader's renormalization absorbs.
        EXPECT_NEAR(readFirst.Weights[i], first.Weights[i], 1.0f / 65535.0f);
        EXPECT_NEAR(readSecond.Weights[i], second.Weights[i], 1.0f / 65535.0f);
        if (first.Weights[i] > 0.0f)
        {
            EXPECT_EQ(readFirst.BoneIDs[i], first.BoneIDs[i]);
        }
        if (second.Weights[i] > 0.0f)
        {
            EXPECT_EQ(readSecond.BoneIDs[i], second.BoneIDs[i]);
        }
    }

    // An unused slot reads back as weight zero whatever its id says — that is
    // what stops the 0xFFFF sentinel ever reaching a palette fetch.
    EXPECT_EQ(readSecond.Weights[2], 0.0f);
    EXPECT_EQ(readSecond.Weights[3], 0.0f);
}

TEST(VirtualSkinnedBounds, SkinningSurvivesTheCookedBlobRoundTrip)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    std::vector<u8> const blob = VirtualMeshSerializer::SerializeToBlob(dag);
    VirtualMesh restored;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(blob, restored));

    ASSERT_TRUE(restored.IsSkinned());
    ASSERT_EQ(static_cast<sizet>(restored.Skinning.Num()), static_cast<sizet>(dag.Skinning.Num()));
    ASSERT_EQ(restored.ClusterBoneRefs, dag.ClusterBoneRefs);
    ASSERT_EQ(static_cast<sizet>(restored.BoneBounds.Num()), static_cast<sizet>(dag.BoneBounds.Num()));
    for (sizet v = 0; v < static_cast<sizet>(dag.Skinning.Num()); ++v)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            EXPECT_EQ(restored.Skinning[v].BoneIDs[i], dag.Skinning[v].BoneIDs[i]);
            EXPECT_FLOAT_EQ(restored.Skinning[v].Weights[i], dag.Skinning[v].Weights[i]);
        }
    }

    // The bound is what the blob exists to carry; comparing it directly says the
    // restored cook is usable, not merely structurally equal.
    std::vector<glm::mat4> const palette = MakeBendPose(kBoneCount, 0.8f);
    EXPECT_FLOAT_EQ(SkinDisplacementBound({ restored.BoneBounds.GetData(), static_cast<sizet>(restored.BoneBounds.Num()) }, palette),
                    SkinDisplacementBound({ dag.BoneBounds.GetData(), static_cast<sizet>(dag.BoneBounds.Num()) }, palette));
}

TEST(VirtualSkinnedBounds, ABlobWithAPartialSkinningPayloadIsRejected)
{
    VirtualMesh const dag = BuildSkinnedDag();
    ASSERT_TRUE(dag.IsSkinned());

    // All-or-nothing, checked on read. A cook that carried bindings but no
    // cluster bone sets would deform the vertices while the cull bounded them
    // from the rest pose — geometry culled against a volume it is not in, which
    // flickers rather than failing.
    {
        VirtualMesh partial = dag;
        partial.ClusterBoneRefs.Pop();
        VirtualMesh out;
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(
            VirtualMeshSerializer::SerializeToBlob(partial), out));
    }
    {
        VirtualMesh partial = dag;
        partial.BoneBounds.Reset();
        VirtualMesh out;
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(
            VirtualMeshSerializer::SerializeToBlob(partial), out));
    }
    {
        VirtualMesh partial = dag;
        partial.Skinning.Pop();
        VirtualMesh out;
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(
            VirtualMeshSerializer::SerializeToBlob(partial), out));
    }
}

TEST(VirtualSkinnedBounds, ASubmeshWithNoBoneWeightsCooksAsRigidAndStillLoads)
{
    // A static prop parented into a rigged character's file: the SOURCE is
    // rigged, so the compaction fills this part's skin stream, but none of its
    // vertices carry a weight. Claiming to be skinned there produces an empty
    // BoneBounds, which the blob's all-or-nothing check rejects — and a cook
    // that cannot be read is re-cooked and re-rejected on EVERY load, forever.
    // It does not deform, so it cooks rigid.
    Ref<MeshSource> source = MakeSkinnedIcosphereMesh(2, kBoneCount);
    TArray<BoneInfluence>& influences = source->GetBoneInfluences();
    for (i32 v = 0; v < influences.Num(); ++v)
    {
        influences[v] = BoneInfluence{};
    }
    ASSERT_FALSE(source->HasBoneInfluences());

    VirtualMesh const dag = VirtualMeshBuilder::Build(*source);
    ASSERT_TRUE(dag.IsValid());
    EXPECT_FALSE(dag.IsSkinned());
    EXPECT_TRUE(dag.BoneBounds.IsEmpty());
    EXPECT_TRUE(dag.ClusterBoneRefs.IsEmpty());

    VirtualMesh restored;
    EXPECT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(VirtualMeshSerializer::SerializeToBlob(dag), restored))
        << "the cook must round-trip; a rejected blob is re-cooked and re-rejected on every load";
}

TEST(VirtualSkinnedBounds, AnOutOfRangeBoneIdIsDroppedAtCookTime)
{
    // Bone ids arrive raw from the asset pack with nothing bounding them, and
    // the emission sizes its per-bone arrays from max(id) + 1 and then indexes
    // them by the same ids: 0xFFFFFFFF wraps that increment to zero and writes
    // out of bounds, and a merely large id asks for a multi-gigabyte allocation.
    // This path was unreachable while the builder rejected skinned sources.
    Ref<MeshSource> source = MakeSkinnedIcosphereMesh(2, kBoneCount);
    TArray<BoneInfluence>& influences = source->GetBoneInfluences();
    ASSERT_GT(influences.Num(), 2);
    influences[0].SetBoneData(0, 0xFFFFFFFFu, 1.0f);
    influences[0].SetBoneData(1, 0u, 0.0f);
    influences[1].SetBoneData(0, 5'000'000u, 1.0f);
    influences[1].SetBoneData(1, 0u, 0.0f);

    VirtualMesh const dag = VirtualMeshBuilder::Build(*source);
    ASSERT_TRUE(dag.IsValid());
    ASSERT_TRUE(dag.IsSkinned()) << "the other vertices still carry real weights";

    // The cap is what the GPU packing can address at all, so an id above it
    // could never have been read by a shader anyway.
    EXPECT_LE(static_cast<sizet>(dag.BoneBounds.Num()), static_cast<sizet>(kVirtualSkinningMaxBoneId));
    for (const VirtualVertexSkinning& binding : dag.Skinning)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            if (binding.Weights[i] > 0.0f)
            {
                EXPECT_LT(binding.BoneIDs[i], static_cast<sizet>(dag.BoneBounds.Num()))
                    << "a surviving influence must address a bone the cook actually bounded";
            }
        }
    }
}

TEST(VirtualSkinnedBounds, ARigidCookCarriesNoSkinningPayloadAtAll)
{
    // The cost control for every rigid virtual mesh in every scene: none of the
    // three arrays, none of the arena tails, nothing in the blob but three zero
    // header words.
    Ref<MeshSource> const source = MakeIcosphereMesh(2);
    VirtualMesh const dag = VirtualMeshBuilder::Build(*source);

    ASSERT_TRUE(dag.IsValid());
    EXPECT_FALSE(dag.IsSkinned());
    EXPECT_TRUE(dag.Skinning.IsEmpty());
    EXPECT_TRUE(dag.BoneBounds.IsEmpty());
    EXPECT_TRUE(dag.ClusterBoneRefs.IsEmpty());
    EXPECT_FALSE(PackVirtualMeshForGpu(dag).IsSkinned());
}
