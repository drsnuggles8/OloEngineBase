// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUParticleCompactionTest.cpp — issue #713 acceptance criterion #2: an
// existing compaction pass converted onto the prefix-sum primitive, with
// DETERMINISTIC OUTPUT ORDER verified.
//
// The pass is `GPUParticleSystem::Compact()`, which until #713 allocated every
// output slot with `atomicAdd` on one of two counters:
//
//     if (alive) aliveIndices[atomicAdd(counters.aliveCount, 1)] = idx;
//     else       freeList    [atomicAdd(counters.deadCount,  1)] = idx;
//
// It is now Particle_Compact.comp (flags) -> GPUPrefixSum (exclusive scan) ->
// Particle_CompactScatter.comp (prefixes to slots).
//
// WHY THIS TEST IS SHAPED AROUND *ORDER*, NOT MEMBERSHIP. The `atomicAdd`
// version already produced the correct SET of alive indices every frame — a
// test that sorted both sides and compared would have passed before the change
// and after it, and proved nothing about the thing #713 exists to fix. So every
// assertion here is about the sequence: the alive list must be ASCENDING in
// particle index, and repeated runs over an unchanged particle buffer must be
// element-for-element identical. The first assertion fails on the old shader by
// construction (its order is whatever the hardware scheduled), which is the
// evidence that the criterion is actually being tested.
//
// It matters visibly, not just in tests: `ParticleBatchRenderer` draws instance
// `i` as `aliveIndices[i]`, so this list IS the blend order of every transparent
// particle on screen.
//
// Classification: shaderpipe (real compute shaders on the GPU). SKIPs cleanly
// with no GL 4.6 context, so headless CI is a no-op.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#include <cstddef>
#include <random>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kMaxParticles = 5000; // 20 work groups: several block seams

        // A particle buffer whose only meaningful field is the alive flag
        // (`Misc.z`), which is all the compaction reads. The rest stays zeroed
        // so a failure message is about slots, not about float noise.
        std::vector<GPUParticle> MakeParticles(const std::vector<bool>& alive)
        {
            std::vector<GPUParticle> particles(alive.size());
            for (std::size_t i = 0; i < alive.size(); ++i)
                particles[i].Misc.z = alive[i] ? 1.0f : 0.0f;
            return particles;
        }

        struct CompactionReadback
        {
            GPUParticleCounters Counters{};
            std::vector<u32> AliveIndices;
            std::vector<u32> FreeList;
        };

        CompactionReadback RunCompaction(GPUParticleSystem& system, const std::vector<GPUParticle>& particles)
        {
            const auto byteCount = static_cast<u32>(particles.size() * sizeof(GPUParticle));

            // Copied out of the accessor rather than used inline: the getters
            // return `const Ref<StorageBuffer>&`, and `Ref<T>` propagates const
            // to the pointee (Core/Ref.h), so `SetData` — non-const — is not
            // reachable through the reference. The readbacks below call the
            // const `GetData` and need no such copy.
            Ref<StorageBuffer> particleBuffer = system.GetParticleSSBO();
            particleBuffer->SetData(particles.data(), byteCount, 0);

            system.Compact();

            CompactionReadback out;
            out.AliveIndices.resize(kMaxParticles);
            out.FreeList.resize(kMaxParticles);

            const u32 indexBytes = kMaxParticles * static_cast<u32>(sizeof(u32));
            system.GetAliveIndexSSBO()->GetData(out.AliveIndices.data(), indexBytes, 0);
            system.GetFreeListSSBO()->GetData(out.FreeList.data(), indexBytes, 0);
            system.GetCounterSSBO()->GetData(&out.Counters, static_cast<u32>(sizeof(GPUParticleCounters)), 0);
            return out;
        }
    } // namespace

    // =========================================================================
    // The compaction produces both lists in ascending particle index.
    //
    // The expected lists are built by a trivial CPU loop over the same alive
    // mask — there is exactly one order that satisfies "ascending", so this is
    // an equality check against a derivation, not against a recorded blob.
    // =========================================================================
    TEST(GPUParticleCompactionTest, AliveAndFreeListsAreInAscendingParticleIndex)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUParticleSystem system(kMaxParticles);
        ASSERT_TRUE(system.IsInitialized()) << "GPU particle system failed to initialise (shaders or prefix sum)";

        // A deliberately lumpy mask: runs of alive and dead particles that
        // straddle the 256-wide work-group boundary, so a block-local scan that
        // forgot to fold in its block offset lands in the wrong place rather
        // than coincidentally right.
        std::mt19937 rng(0x713u);
        std::bernoulli_distribution keep(0.4);
        std::vector<bool> alive(kMaxParticles);
        for (u32 i = 0; i < kMaxParticles; ++i)
            alive[i] = keep(rng);

        std::vector<u32> expectedAlive;
        std::vector<u32> expectedFree;
        for (u32 i = 0; i < kMaxParticles; ++i)
            (alive[i] ? expectedAlive : expectedFree).push_back(i);

        ASSERT_FALSE(expectedAlive.empty());
        ASSERT_FALSE(expectedFree.empty());

        const CompactionReadback result = RunCompaction(system, MakeParticles(alive));

        EXPECT_EQ(result.Counters.AliveCount, static_cast<u32>(expectedAlive.size()));
        EXPECT_EQ(result.Counters.DeadCount, static_cast<u32>(expectedFree.size()));
        EXPECT_EQ(result.Counters.AliveCount + result.Counters.DeadCount, kMaxParticles)
            << "every slot must be in exactly one of the two lists";

        // Only the populated prefix of each list is defined; the tail is
        // whatever the previous frame left.
        ASSERT_GE(result.AliveIndices.size(), expectedAlive.size());
        for (std::size_t i = 0; i < expectedAlive.size(); ++i)
        {
            ASSERT_EQ(result.AliveIndices[i], expectedAlive[i])
                << "alive slot " << i << " — the compacted list is not in ascending particle index";
        }
        for (std::size_t i = 0; i < expectedFree.size(); ++i)
        {
            ASSERT_EQ(result.FreeList[i], expectedFree[i])
                << "free slot " << i << " — the free list is not in ascending particle index";
        }
    }

    // =========================================================================
    // Repeated compaction of an unchanged particle buffer is bit-identical.
    //
    // This is the assertion the `atomicAdd` version could not pass. It is kept
    // separate from the ordering test above so a failure says which property
    // broke: a wrong-but-stable order fails only the first test, a
    // right-but-unstable one only this.
    // =========================================================================
    TEST(GPUParticleCompactionTest, RepeatedCompactionIsBitIdentical)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUParticleSystem system(kMaxParticles);
        ASSERT_TRUE(system.IsInitialized());

        std::mt19937 rng(0xC0FFEEu);
        std::bernoulli_distribution keep(0.63);
        std::vector<bool> alive(kMaxParticles);
        for (u32 i = 0; i < kMaxParticles; ++i)
            alive[i] = keep(rng);

        const std::vector<GPUParticle> particles = MakeParticles(alive);

        const CompactionReadback first = RunCompaction(system, particles);
        ASSERT_GT(first.Counters.AliveCount, 0u) << "the mask kept nothing — the comparisons below would be vacuous";

        const u32 aliveCount = first.Counters.AliveCount;
        const u32 deadCount = first.Counters.DeadCount;

        for (u32 run = 1; run < 6; ++run)
        {
            const CompactionReadback again = RunCompaction(system, particles);

            ASSERT_EQ(again.Counters.AliveCount, aliveCount) << "run " << run;
            ASSERT_EQ(again.Counters.DeadCount, deadCount) << "run " << run;

            // Compare only the defined prefixes — see the note above.
            for (u32 i = 0; i < aliveCount; ++i)
            {
                ASSERT_EQ(again.AliveIndices[i], first.AliveIndices[i])
                    << "run " << run << ", alive slot " << i << " — compaction is not deterministic";
            }
            for (u32 i = 0; i < deadCount; ++i)
            {
                ASSERT_EQ(again.FreeList[i], first.FreeList[i])
                    << "run " << run << ", free slot " << i << " — compaction is not deterministic";
            }
        }
    }

    // =========================================================================
    // The two degenerate masks. All-dead is the case where the scan's grand
    // total is 0, and all-alive is where the free list is empty and every
    // element of the alive list is its own index — both are where an off-by-one
    // in `idx - aliveBefore` shows up unambiguously.
    // =========================================================================
    TEST(GPUParticleCompactionTest, AllAliveAndAllDeadMasks)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUParticleSystem system(kMaxParticles);
        ASSERT_TRUE(system.IsInitialized());

        {
            const CompactionReadback allAlive =
                RunCompaction(system, MakeParticles(std::vector<bool>(kMaxParticles, true)));
            EXPECT_EQ(allAlive.Counters.AliveCount, kMaxParticles);
            EXPECT_EQ(allAlive.Counters.DeadCount, 0u);
            for (u32 i = 0; i < kMaxParticles; ++i)
                ASSERT_EQ(allAlive.AliveIndices[i], i) << "all-alive: slot " << i;
        }

        {
            const CompactionReadback allDead =
                RunCompaction(system, MakeParticles(std::vector<bool>(kMaxParticles, false)));
            EXPECT_EQ(allDead.Counters.AliveCount, 0u);
            EXPECT_EQ(allDead.Counters.DeadCount, kMaxParticles);
            for (u32 i = 0; i < kMaxParticles; ++i)
                ASSERT_EQ(allDead.FreeList[i], i) << "all-dead: free slot " << i;
        }
    }
} // namespace OloEngine::Tests
