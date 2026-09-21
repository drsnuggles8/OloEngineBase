// OLO_TEST_LAYER: unit
//
// =============================================================================
// GroomBodyColliderTest.cpp — the fitted body proxy. Issue #1250.
//
// The proxy is DERIVED from the body rather than authored (GroomBodyCollider.h
// says why), so the thing that can go wrong is not "somebody placed a capsule
// badly" — it is that the fit silently produces a shape with no relation to the
// limb it stands for. A capsule around the wrong axis, or one inflated by a
// single stray vertex, gives a coat that is pushed off the body by a smoothly
// varying amount, which reads as a binding error rather than a collision one.
//
// So every case here asserts a GEOMETRIC fact about a synthetic body whose
// answer is known in advance, not a property of a shape.
// =============================================================================

#include "OloEngine/Groom/GroomBodyCollider.h"

#include <gtest/gtest.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace OloEngine;

namespace
{
    /// A synthetic skinned body: vertices, indices and a four-influence stream
    /// in the exact 32-byte layout GroomSkinningView expects.
    struct FakeBody
    {
        std::vector<glm::vec3> Positions;
        std::vector<u32> Indices;
        /// { u32 ids[4]; f32 weights[4]; } per vertex, 32 bytes.
        std::vector<u32> Influences;
        std::vector<glm::mat4> Palette;

        void AddVertex(const glm::vec3& position, u32 bone)
        {
            Positions.push_back(position);
            Influences.insert(Influences.end(), { bone, 0u, 0u, 0u });
            const f32 one = 1.0f;
            Influences.insert(Influences.end(), { std::bit_cast<u32>(one), 0u, 0u, 0u });
        }

        /// A capsule-ish cloud of `rings` x `perRing` vertices around the
        /// segment from `a` to `b`, at `radius`, all owned by `bone`.
        void AddLimb(u32 bone, const glm::vec3& a, const glm::vec3& b, f32 radius, u32 rings = 12u,
                     u32 perRing = 12u)
        {
            const glm::vec3 axis = glm::normalize(b - a);
            // Any vector not parallel to the axis gives a usable basis; the
            // choice does not matter because the fit finds the axis itself.
            const glm::vec3 seed = std::abs(axis.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 u = glm::normalize(glm::cross(axis, seed));
            const glm::vec3 v = glm::cross(axis, u);
            for (u32 r = 0; r < rings; ++r)
            {
                const f32 t = static_cast<f32>(r) / static_cast<f32>(rings - 1u);
                const glm::vec3 centre = a + (b - a) * t;
                for (u32 s = 0; s < perRing; ++s)
                {
                    const f32 angle = 6.2831853f * static_cast<f32>(s) / static_cast<f32>(perRing);
                    AddVertex(centre + (u * std::cos(angle) + v * std::sin(angle)) * radius, bone);
                }
            }
        }

        /// One degenerate triangle per three vertices, purely so the surface
        /// view is "usable". The fit never reads the index buffer; it is here
        /// because GroomSurfaceView refuses a surface with no triangles, and
        /// that refusal is worth keeping honest.
        void FinishIndices()
        {
            Indices.clear();
            for (u32 i = 0; i + 2u < static_cast<u32>(Positions.size()); i += 3u)
            {
                Indices.insert(Indices.end(), { i, i + 1u, i + 2u });
            }
        }

        [[nodiscard]] GroomSurfaceView Surface() const
        {
            GroomSurfaceView view;
            view.PositionData = reinterpret_cast<const std::byte*>(Positions.data());
            view.PositionStride = sizeof(glm::vec3);
            view.VertexCount = static_cast<u32>(Positions.size());
            view.Indices = Indices.data();
            view.IndexCount = static_cast<u32>(Indices.size());
            return view;
        }

        [[nodiscard]] GroomSkinningView Skinning() const
        {
            GroomSkinningView view;
            const auto* base = reinterpret_cast<const std::byte*>(Influences.data());
            view.BoneIds = reinterpret_cast<const u32*>(base);
            view.Weights = reinterpret_cast<const f32*>(base + 16);
            view.Stride = 32u;
            view.VertexCount = static_cast<u32>(Positions.size());
            view.Palette = Palette;
            return view;
        }
    };
} // namespace

// The core geometric claim: the fitted capsule follows the LIMB'S axis and sits
// at the limb's radius. A fit that found the wrong eigenvector puts a capsule
// across the leg instead of along it, which is a shape that contains almost
// none of the body it stands for.
TEST(GroomBodyCollider, FitsTheLimbAxisAndRadius)
{
    FakeBody body;
    const glm::vec3 a{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 b{ 0.0f, 1.0f, 0.0f };
    body.AddLimb(0u, a, b, 0.2f);
    body.FinishIndices();
    body.Palette.assign(1, glm::mat4(1.0f));

    TArray<GroomColliderBinding> bindings;
    const GroomColliderBuildStats stats =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, bindings);

    ASSERT_EQ(stats.CollidersBuilt, 1u);
    ASSERT_EQ(bindings.Num(), 1u);
    const GroomColliderBinding& capsule = bindings.First();
    EXPECT_EQ(capsule.BoneIndex, 0u);

    // Along the limb, both ends within a ring of the true extremes. The
    // percentile cut is 2%/98% over 12 evenly spaced rings, so one ring is the
    // resolution of the answer.
    const glm::vec3 axis = glm::normalize(capsule.PointB - capsule.PointA);
    EXPECT_NEAR(std::abs(glm::dot(axis, glm::vec3(0.0f, 1.0f, 0.0f))), 1.0f, 1.0e-3f)
        << "the capsule must lie along the limb, not across it";
    const f32 length = glm::length(capsule.PointB - capsule.PointA);
    EXPECT_NEAR(length, 1.0f, 0.2f);

    // Radius at the limb's radius. The vertices are exactly on the surface, so
    // every percentile of the perpendicular distance is the same number.
    EXPECT_NEAR(capsule.Radius, 0.2f, 1.0e-3f);
}

// One capsule per bone, and each of them around its own limb. A fit that
// bucketed by anything other than the dominant influence produces one capsule
// spanning both limbs, which swallows the gap between them.
TEST(GroomBodyCollider, FitsOneCapsulePerBone)
{
    FakeBody body;
    body.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f);
    body.AddLimb(1u, { 5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 2.0f }, 0.35f);
    body.FinishIndices();
    body.Palette.assign(2, glm::mat4(1.0f));

    TArray<GroomColliderBinding> bindings;
    const GroomColliderBuildStats stats =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, bindings);
    ASSERT_EQ(stats.CollidersBuilt, 2u);
    ASSERT_EQ(bindings.Num(), 2u);
    // Sorted by bone, so this is the contract and not an accident of iteration.
    EXPECT_EQ(bindings[0].BoneIndex, 0u);
    EXPECT_EQ(bindings[1].BoneIndex, 1u);
    EXPECT_NEAR(bindings[0].Radius, 0.2f, 1.0e-3f);
    EXPECT_NEAR(bindings[1].Radius, 0.35f, 1.0e-3f);
    // The second capsule is around ITS limb, which is five units away.
    EXPECT_GT(bindings[1].PointA.x, 4.0f);
}

// PERCENTILES, not extrema. One stray vertex far out along the axis must not
// stretch the capsule down the leg — a proxy that contains every last vertex of
// a hand is a sphere around the whole hand, and it pushes the coat off the arm.
TEST(GroomBodyCollider, AStrayVertexDoesNotStretchTheCapsule)
{
    FakeBody clean;
    clean.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f);
    clean.FinishIndices();
    clean.Palette.assign(1, glm::mat4(1.0f));

    FakeBody strayed = clean;
    strayed.AddVertex(glm::vec3(0.0f, 20.0f, 0.0f), 0u); // one finger tip, far away
    strayed.FinishIndices();

    TArray<GroomColliderBinding> cleanFit;
    TArray<GroomColliderBinding> strayedFit;
    (void)BuildGroomBodyColliders(clean.Surface(), clean.Skinning(), GroomColliderBuildSettings{}, cleanFit);
    (void)BuildGroomBodyColliders(strayed.Surface(), strayed.Skinning(), GroomColliderBuildSettings{}, strayedFit);
    ASSERT_EQ(cleanFit.Num(), 1u);
    ASSERT_EQ(strayedFit.Num(), 1u);

    const f32 cleanLength = glm::length(cleanFit[0].PointB - cleanFit[0].PointA);
    const f32 strayedLength = glm::length(strayedFit[0].PointB - strayedFit[0].PointA);
    // Extrema would have made this 20x. The percentile cut keeps it within a
    // few ring spacings, because the stray vertex is one sample of 145.
    EXPECT_LT(strayedLength, cleanLength * 2.0f)
        << "clean " << cleanLength << " strayed " << strayedLength;
}

// A twist helper, an attachment point, an IK target: bones with a handful of
// vertices are not limbs, and fitting a capsule to three of them produces a
// shape with no relation to the body. Skipped, and COUNTED, because "this coat
// has no proxy" needs an answer in the inspector.
TEST(GroomBodyCollider, BonesWithTooFewVerticesAreSkippedAndCounted)
{
    FakeBody body;
    body.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f);
    for (u32 i = 0; i < 3u; ++i)
    {
        body.AddVertex(glm::vec3(3.0f + static_cast<f32>(i) * 0.01f, 0.0f, 0.0f), 1u);
    }
    body.FinishIndices();
    body.Palette.assign(2, glm::mat4(1.0f));

    TArray<GroomColliderBinding> bindings;
    const GroomColliderBuildStats stats =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, bindings);
    EXPECT_EQ(stats.BonesConsidered, 2u);
    EXPECT_EQ(stats.CollidersBuilt, 1u);
    EXPECT_EQ(stats.BonesSkippedTooFewVertices, 1u);
}

// The cap keeps the LARGEST, because a coat penetrating a torso is visible from
// across the room and a coat penetrating a finger is not — and it says it
// truncated, so "my fingers poke through" is not a mystery.
TEST(GroomBodyCollider, TheCapKeepsTheLargestAndSaysSo)
{
    FakeBody body;
    body.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, 0.5f);  // a torso
    body.AddLimb(1u, { 4.0f, 0.0f, 0.0f }, { 4.0f, 1.0f, 0.0f }, 0.15f); // an arm
    body.AddLimb(2u, { 8.0f, 0.0f, 0.0f }, { 8.0f, 0.2f, 0.0f }, 0.02f); // a finger
    body.FinishIndices();
    body.Palette.assign(3, glm::mat4(1.0f));

    GroomColliderBuildSettings settings;
    settings.MaxColliders = 2;

    TArray<GroomColliderBinding> bindings;
    const GroomColliderBuildStats stats =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), settings, bindings);
    EXPECT_TRUE(stats.Truncated);
    ASSERT_EQ(bindings.Num(), 2u);
    EXPECT_EQ(bindings[0].BoneIndex, 0u);
    EXPECT_EQ(bindings[1].BoneIndex, 1u);
}

// An unskinned or morph-only body has nothing to carry a capsule. The honest
// answer is NO colliders, not a proxy pinned to bone zero — which would be a
// capsule in the wrong place that looks like the feature working.
TEST(GroomBodyCollider, AnUnskinnedBodyProducesNoColliders)
{
    FakeBody body;
    body.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f);
    body.FinishIndices();
    body.Palette.clear(); // no palette: a static or morph-only target

    TArray<GroomColliderBinding> bindings;
    const GroomColliderBuildStats stats =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, bindings);
    EXPECT_EQ(stats.CollidersBuilt, 0u);
    EXPECT_TRUE(bindings.IsEmpty());
}

TEST(GroomBodyCollider, IsDeterministic)
{
    FakeBody body;
    body.AddLimb(0u, { 0.0f, 0.0f, 0.0f }, { 0.3f, 1.0f, -0.2f }, 0.2f);
    body.AddLimb(1u, { 2.0f, 0.5f, 1.0f }, { 2.4f, -0.5f, 1.3f }, 0.31f);
    body.FinishIndices();
    body.Palette.assign(2, glm::mat4(1.0f));

    TArray<GroomColliderBinding> a;
    TArray<GroomColliderBinding> b;
    const GroomColliderBuildStats statsA =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, a);
    const GroomColliderBuildStats statsB =
        BuildGroomBodyColliders(body.Surface(), body.Skinning(), GroomColliderBuildSettings{}, b);
    EXPECT_EQ(a, b);
    EXPECT_EQ(statsA, statsB);
}

// -----------------------------------------------------------------------------
// Resolving into a pose
// -----------------------------------------------------------------------------

TEST(GroomBodyCollider, ResolveCarriesTheCapsuleByItsBoneMatrix)
{
    GroomColliderBinding binding;
    binding.PointA = glm::vec3(0.0f, 0.0f, 0.0f);
    binding.PointB = glm::vec3(0.0f, 1.0f, 0.0f);
    binding.Radius = 0.25f;
    binding.BoneIndex = 1u;

    std::vector<glm::mat4> palette(2, glm::mat4(1.0f));
    palette[1] = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));

    TArray<GroomCollider> resolved;
    ResolveGroomBodyColliders({ &binding, 1 }, palette, glm::mat4(1.0f), 1.0f, resolved);
    ASSERT_EQ(resolved.Num(), 1u);
    EXPECT_EQ(resolved[0].PointA, glm::vec3(3.0f, 0.0f, 0.0f));
    EXPECT_EQ(resolved[0].PointB, glm::vec3(3.0f, 1.0f, 0.0f));
    EXPECT_EQ(resolved[0].Radius, 0.25f);
}

// A body scaled up at runtime must get a proxy that scales with it, rather than
// a coat that suddenly intersects.
TEST(GroomBodyCollider, ResolveScalesTheRadiusWithTheBone)
{
    GroomColliderBinding binding;
    binding.PointA = glm::vec3(0.0f);
    binding.PointB = glm::vec3(0.0f, 1.0f, 0.0f);
    binding.Radius = 0.25f;
    binding.BoneIndex = 0u;

    const std::vector<glm::mat4> palette{ glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)) };
    TArray<GroomCollider> resolved;
    ResolveGroomBodyColliders({ &binding, 1 }, palette, glm::mat4(1.0f), 1.0f, resolved);
    ASSERT_EQ(resolved.Num(), 1u);
    EXPECT_NEAR(resolved[0].Radius, 0.5f, 1.0e-5f);

    // And the authored scale multiplies on top, which is the lever that stands
    // in for a hand-placed proxy rig.
    ResolveGroomBodyColliders({ &binding, 1 }, palette, glm::mat4(1.0f), 3.0f, resolved);
    ASSERT_EQ(resolved.Num(), 1u);
    EXPECT_NEAR(resolved[0].Radius, 1.5f, 1.0e-5f);
}

// DROPPED, never clamped to a valid bone: a capsule at the wrong limb pushes the
// coat somewhere plausible and wrong.
TEST(GroomBodyCollider, ABindingNamingAMissingBoneIsDropped)
{
    GroomColliderBinding binding;
    binding.PointA = glm::vec3(0.0f);
    binding.PointB = glm::vec3(0.0f, 1.0f, 0.0f);
    binding.Radius = 0.25f;
    binding.BoneIndex = 9u;

    const std::vector<glm::mat4> palette(2, glm::mat4(1.0f));
    TArray<GroomCollider> resolved;
    ResolveGroomBodyColliders({ &binding, 1 }, palette, glm::mat4(1.0f), 1.0f, resolved);
    EXPECT_TRUE(resolved.IsEmpty());
}
