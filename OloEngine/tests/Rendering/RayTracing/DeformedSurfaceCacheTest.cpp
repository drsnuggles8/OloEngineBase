// OLO_TEST_LAYER: plumbing
//
// #1229 — the deformed-vertex producer's allocation policy.
//
// WHAT THIS FILE COVERS, and it is deliberately only the half a machine with no
// ray-tracing device can honestly cover: the sizing and growth arithmetic, and
// the disabled-cache contract that keeps OpenGL untouched.
//
// SUBSTITUTION, named rather than left implicit (substituted-seams-compound.md):
// everything that needs a real buffer is absent here. Nothing in this file can
// see a deformed stream whose contents are wrong, a device address that moved
// without the record noticing, a dispatch recorded outside a frame, or a
// compute write that reached an acceleration structure build before it landed.
// The first is pinned by the live editor verification in the PR, the last by
// the barrier in VulkanRayTracingBackend, and neither is reachable from a CI
// runner — which is every runner this project has
// (radv-lacks-descriptor-heap, and the ray-query gate is satisfied on no
// runner at all).
//
// What IS worth pinning here is the arithmetic, because two of its three
// failure modes are silent: a capacity that rounds DOWN allocates a buffer the
// compute shader writes past, and a growth rule that reallocates too eagerly
// turns every frame of a growing surface into a full BLAS rebuild rather than a
// refit. Neither raises anything at runtime.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/RayTracing/DeformedSurfaceCache.h"
// The cache's header only forward-declares MeshSource; constructing the empty
// Ref<MeshSource> the Acquire calls below pass needs the complete type here.
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <limits>
#include <span>
#include <unordered_set>

namespace OloEngine::Tests
{
    namespace RT = OloEngine::RayTracing;

    TEST(DeformedSurfaceSizing, AStreamIsExactlyTheEngineVertexLayout)
    {
        // The whole design rests on this: the deformed stream is byte-for-byte
        // an OloEngine::Vertex array, which is why the ray-tracing hit shaders
        // read it at their existing stride and needed no change. A stride that
        // drifted from sizeof(Vertex) would make every hit interpolate the
        // wrong triangle — plausibly, not obviously.
        EXPECT_EQ(RT::DeformedSurfaceCache::StreamBytes(1), sizeof(Vertex));
        EXPECT_EQ(RT::DeformedSurfaceCache::StreamBytes(1000), 1000u * sizeof(Vertex));
        EXPECT_EQ(RT::DeformedSurfaceCache::StreamBytes(0), 0u);
    }

    TEST(DeformedSurfaceSizing, StreamBytesDoesNotOverflowAtARealisticMaximum)
    {
        // u64 arithmetic on a u32 count. Computed in 32 bits this wraps, and
        // the wrap allocates a small buffer for a large mesh — an out-of-bounds
        // compute write with no diagnostic anywhere.
        constexpr u32 kHugeVertexCount = 200'000'000u;
        EXPECT_EQ(RT::DeformedSurfaceCache::StreamBytes(kHugeVertexCount),
                  static_cast<u64>(kHugeVertexCount) * sizeof(Vertex));
        EXPECT_GT(RT::DeformedSurfaceCache::StreamBytes(kHugeVertexCount),
                  static_cast<u64>(std::numeric_limits<u32>::max()));
    }

    TEST(DeformedSurfaceSizing, CapacityNeverRoundsDown)
    {
        // The direction that matters. An oversized capacity wastes memory; an
        // undersized one is a buffer the shader writes past, and the compute
        // shader has no bound of its own to catch it — its only bound is the
        // vertex count the CPU supplies.
        for (const u32 count : { 1u, 2u, 255u, 256u, 257u, 511u, 512u, 4095u, 100'000u })
        {
            EXPECT_GE(RT::DeformedSurfaceCache::CapacityFor(count), count) << "count " << count;
        }
        EXPECT_EQ(RT::DeformedSurfaceCache::CapacityFor(0), 0u);
    }

    TEST(DeformedSurfaceSizing, CapacityRoundsUpSoAGrowingSurfaceDoesNotRebuildEveryFrame)
    {
        // Growth granularity is not a micro-optimisation here. A reallocation
        // moves the device address, which the geometry fingerprint sees, which
        // forces a full BLAS REBUILD rather than a refit — so rounding is what
        // stops a surface that gains a vertex at a time from paying a rebuild
        // per frame.
        EXPECT_EQ(RT::DeformedSurfaceCache::CapacityFor(1), RT::DeformedSurfaceCache::CapacityFor(256));
        EXPECT_EQ(RT::DeformedSurfaceCache::CapacityFor(257), RT::DeformedSurfaceCache::CapacityFor(512));
        EXPECT_NE(RT::DeformedSurfaceCache::CapacityFor(256), RT::DeformedSurfaceCache::CapacityFor(257));
    }

    TEST(DeformedSurfaceSizing, CapacitySaturatesRatherThanWrappingAtTheTop)
    {
        // Rounding up near u32 max wraps, and a wrapped capacity is SMALLER
        // than the request — the one arithmetic bug in this file that produces
        // an undersized buffer rather than a large one. Not a real mesh, but
        // "not a real input" is how this class of bug ships.
        constexpr u32 kMax = std::numeric_limits<u32>::max();
        EXPECT_GE(RT::DeformedSurfaceCache::CapacityFor(kMax), kMax);
        EXPECT_GE(RT::DeformedSurfaceCache::CapacityFor(kMax - 1u), kMax - 1u);
    }

    TEST(DeformedSurfaceSizing, AnExistingBufferServesOnlyWhatItCanHold)
    {
        EXPECT_TRUE(RT::DeformedSurfaceCache::CapacityServes(256, 256));
        EXPECT_TRUE(RT::DeformedSurfaceCache::CapacityServes(512, 300))
            << "a surface that SHRANK must keep its buffer: reallocating buys nothing and costs a rebuild";
        EXPECT_FALSE(RT::DeformedSurfaceCache::CapacityServes(256, 257));
        EXPECT_FALSE(RT::DeformedSurfaceCache::CapacityServes(0, 1));
        EXPECT_FALSE(RT::DeformedSurfaceCache::CapacityServes(256, 0))
            << "a zero-vertex surface is not served by anything — it has no stream to point at";
    }

    TEST(DeformedSurfaceIdentity, TheKeyDistinguishesAnEntityFromItsBuffer)
    {
        // The hash folds two u64 lanes. A shift-and-xor would collide every
        // (entity, buffer) pair with its transpose, which in a scene where
        // entity ids and buffer handles are both small dense integers is not a
        // theoretical collision — and a collision here hands one character
        // another character's deformed vertices.
        const RT::DeformedSurfaceKeyHash hash;
        EXPECT_NE(hash(RT::DeformedSurfaceKey{ .EntityId = 3, .RestVertexBuffer = 7 }),
                  hash(RT::DeformedSurfaceKey{ .EntityId = 7, .RestVertexBuffer = 3 }));

        std::unordered_set<sizet> seen;
        for (u64 entity = 1; entity <= 32; ++entity)
        {
            for (u64 buffer = 1; buffer <= 32; ++buffer)
            {
                seen.insert(hash(RT::DeformedSurfaceKey{ .EntityId = entity, .RestVertexBuffer = buffer }));
            }
        }
        // 1024 distinct keys. A few collisions are a property of hashing; a
        // flood of them is the transpose bug above.
        EXPECT_GT(seen.size(), 1000u) << "the key hash collides far more than chance would explain";
    }

    TEST(DeformedSurfaceIdentity, TwoCharactersSharingAMeshAreDifferentSurfaces)
    {
        // The reason the key carries the entity at all. Two entities of one
        // skinned asset share the REST buffer and hold different poses; a key
        // on the buffer alone would give them one deformed stream and therefore
        // one pose.
        EXPECT_NE((RT::DeformedSurfaceKey{ .EntityId = 1, .RestVertexBuffer = 99 }),
                  (RT::DeformedSurfaceKey{ .EntityId = 2, .RestVertexBuffer = 99 }));
        // ...and the reason it carries the buffer: an LOD switch hands the
        // entity a different mesh, which is a different surface with a
        // different vertex count.
        EXPECT_NE((RT::DeformedSurfaceKey{ .EntityId = 1, .RestVertexBuffer = 99 }),
                  (RT::DeformedSurfaceKey{ .EntityId = 1, .RestVertexBuffer = 100 }));
    }

    TEST(DeformedSurfacePose, TheSameBonesHashTheSameAndDifferentBonesDoNot)
    {
        // The skip decision rests entirely on this. A hash that ignored a
        // moving bone would freeze a character's ray-traced silhouette at
        // whichever pose it was first dispatched at — the original #1229 defect
        // wearing a different hat, and just as quiet.
        const glm::mat4 rest[3] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        glm::mat4 moved[3] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };

        EXPECT_EQ(RT::DeformedSurfaceCache::HashPalette(rest), RT::DeformedSurfaceCache::HashPalette(rest));

        // One bone, one component, a displacement far below anything a float
        // epsilon would forgive.
        moved[1][3][1] = 1e-4f;
        EXPECT_NE(RT::DeformedSurfaceCache::HashPalette(rest), RT::DeformedSurfaceCache::HashPalette(moved));
    }

    TEST(DeformedSurfacePose, APaletteThatSHRANKDoesNotHashLikeItsOwnPrefix)
    {
        // A bone-count change is a different skeleton, and the palette's first
        // N matrices can easily be unchanged across it. Without the length fold
        // the two hash identically and the surface is never re-deformed for its
        // new rig.
        const glm::mat4 bones[3] = { glm::mat4(1.0f), glm::mat4(2.0f), glm::mat4(3.0f) };
        EXPECT_NE(RT::DeformedSurfaceCache::HashPalette(std::span{ bones, 3 }),
                  RT::DeformedSurfaceCache::HashPalette(std::span{ bones, 2 }));
    }

    TEST(DeformedSurfacePose, AnEmptyPaletteIsStable)
    {
        EXPECT_EQ(RT::DeformedSurfaceCache::HashPalette({}), RT::DeformedSurfaceCache::HashPalette({}));
    }

    TEST(DeformedSurfaceCacheContract, ADisabledCacheOffersNothingAndCountsNothing)
    {
        // What keeps OpenGL byte-identical to its pre-#1229 self. The cache is
        // armed from the ray-tracing capability, so on every backend without
        // one Acquire refuses — and, critically, refuses WITHOUT counting.
        //
        // The distinction is the whole value of the `refused` counter: it is
        // meant to read non-zero exactly when a character that should be
        // traceable is not. A disabled cache that counted would make every
        // OpenGL frame report thousands of refusals and the number would mean
        // nothing on either backend.
        RT::DeformedSurfaceCache cache;
        ASSERT_FALSE(cache.IsEnabled());

        cache.BeginFrame();
        const RT::DeformedSurfaceBinding binding =
            cache.Acquire(RT::DeformedSurfaceKey{ .EntityId = 1, .RestVertexBuffer = 2 }, /*isAnimated=*/true, {}, {});
        cache.EndFrame();

        EXPECT_FALSE(binding.IsValid());
        EXPECT_EQ(cache.GetStats().SurfacesRequested, 0u);
        EXPECT_EQ(cache.GetStats().Refused, 0u);
        EXPECT_EQ(cache.GetStats().ResidentSurfaces, 0u);
        EXPECT_FALSE(cache.HasWork());
        EXPECT_EQ(cache.Dispatch(), 0u);
    }

    TEST(DeformedSurfaceCacheContract, ARigidSurfaceIsNotAnAnimatedOneThatFailed)
    {
        // Acquire is called for EVERY staged submesh, not only animated ones,
        // so "this is not an animated surface" has to be distinguishable from
        // "this animated surface could not be produced". Without the explicit
        // isAnimated input, every wall in the level would land in the refused
        // count and the one number that means something would be unreadable.
        //
        // The cache is disabled here, so this pins the ordering of the two
        // early-outs rather than the production path: neither a rigid surface
        // nor a disabled cache may touch the counters.
        RT::DeformedSurfaceCache cache;
        cache.BeginFrame();
        static_cast<void>(cache.Acquire(RT::DeformedSurfaceKey{ .EntityId = 1, .RestVertexBuffer = 2 },
                                        /*isAnimated=*/false, {}, {}));
        cache.EndFrame();

        EXPECT_EQ(cache.GetStats().SurfacesRequested, 0u);
        EXPECT_EQ(cache.GetStats().Refused, 0u);
    }
} // namespace OloEngine::Tests
