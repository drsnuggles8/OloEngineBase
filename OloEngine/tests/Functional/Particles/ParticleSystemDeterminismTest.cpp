#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: Functional
// =============================================================================
// ParticleSystemDeterminismTest — Functional Test.
//
// Seam under test:
//   ParticleSystem emission RNG × run determinism (issue #452 / #576).
//
// Each ParticleSystem now owns a per-system random stream (SeedRandom /
// DeriveSeed) instead of drawing from the thread_local
// RandomUtils::GetGlobalRandom(). This is the mechanism that closes the #452
// sub-emitter / particle-jitter determinism gap: given the same run seed and
// the same entity UUID, a system replays an identical particle stream —
// independent of (a) how many other consumers (loot rolls, sibling emitters)
// drew from the shared global stream this frame, and (b) which thread the
// update ran on (the global stream is only seeded on the game thread).
//
// These tests drive ParticleSystem directly (the unit that owns the stream)
// rather than through a full Scene: the property under test is the emission
// stream itself, and a direct drive is both the tightest and the most robust
// way to pin it. The Scene wiring that seeds each system from
// global-seed × UUID lives in Scene::OnRuntimeStart / OnSimulationStart.
// =============================================================================

#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Particle/ParticleSystem.h"

#include <bit>
#include <functional>
#include <glm/glm.hpp>
#include <vector>

using namespace OloEngine;

namespace
{
    // A configuration that exercises every RNG-driven code path in emission:
    // rate-based spawn count, speed / size / rotation variance, a lifetime
    // range, and a Sphere emission shape (rejection-sampled position +
    // direction). If the stream is deterministic here it is deterministic
    // everywhere the emitter draws randomness.
    ParticleSystem MakeConfiguredSystem()
    {
        ParticleSystem sys(512);
        sys.Playing = true;
        sys.Looping = true;
        sys.Duration = 1000.0f; // No mid-run loop reset within the test window.
        sys.Emitter.RateOverTime = 40.0f;
        sys.Emitter.InitialSpeed = 3.0f;
        sys.Emitter.SpeedVariance = 1.5f;
        sys.Emitter.InitialSize = 1.0f;
        sys.Emitter.SizeVariance = 0.5f;
        sys.Emitter.InitialRotation = 0.0f;
        sys.Emitter.RotationVariance = 3.0f;
        sys.Emitter.LifetimeMin = 0.8f;
        sys.Emitter.LifetimeMax = 1.6f;
        sys.Emitter.Shape = EmitSphere{ 2.0f };
        return sys;
    }

    // Bit-exact signature of every alive particle's spawned state. Encoded as
    // integers (via std::bit_cast) so the comparison is an exact identity
    // check without float == : determinism means bit-identical, not "close".
    std::vector<u32> Signature(const ParticleSystem& sys)
    {
        const u32 count = sys.GetAliveCount();
        const ParticlePool& pool = sys.GetPool();
        std::vector<u32> sig;
        sig.reserve(static_cast<sizet>(count) * 9 + 1);
        sig.push_back(count);
        const auto push = [&sig](f32 v)
        { sig.push_back(std::bit_cast<u32>(v)); };
        for (u32 i = 0; i < count; ++i)
        {
            push(pool.m_Positions[i].x);
            push(pool.m_Positions[i].y);
            push(pool.m_Positions[i].z);
            push(pool.m_Velocities[i].x);
            push(pool.m_Velocities[i].y);
            push(pool.m_Velocities[i].z);
            push(pool.m_Sizes[i]);
            push(pool.m_Rotations[i]);
            push(pool.m_Lifetimes[i]);
        }
        return sig;
    }

    // Advance a system `frames` fixed steps, optionally running `between`
    // before each Update (used to perturb the global RNG mid-run).
    std::vector<u32> RunAndSnapshot(ParticleSystem& sys, u32 frames, const std::function<void()>& between = {})
    {
        constexpr f32 dt = 1.0f / 60.0f;
        const glm::vec3 emitterPos{ 5.0f, 1.0f, -2.0f };
        for (u32 f = 0; f < frames; ++f)
        {
            if (between)
            {
                between();
            }
            sys.Update(dt, emitterPos);
        }
        return Signature(sys);
    }

    constexpr u64 kRunSeed = 0xC0FFEEULL;
    constexpr u64 kEntityUUID = 0x1234'5678'9ABC'DEF0ULL;
    constexpr u32 kFrames = 40;
} // namespace

// Same run seed + same UUID ⇒ byte-identical particle stream.
TEST(ParticleSystemDeterminism, SameSeedAndUUIDProducesIdenticalStream)
{
    const u64 seed = ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID);

    ParticleSystem a = MakeConfiguredSystem();
    a.SeedRandom(seed);
    const std::vector<u32> sigA = RunAndSnapshot(a, kFrames);

    ParticleSystem b = MakeConfiguredSystem();
    b.SeedRandom(seed);
    const std::vector<u32> sigB = RunAndSnapshot(b, kFrames);

    ASSERT_GT(sigA[0], 5u) << "sanity: the configuration should have emitted a non-trivial pool";
    EXPECT_EQ(sigA, sigB)
        << "two systems seeded identically diverged — per-system RNG is not deterministic";
}

// Same run seed but a different entity UUID ⇒ a different stream (each emitter
// in a scene has its own reproducible-but-distinct jitter).
TEST(ParticleSystemDeterminism, DifferentUUIDProducesDifferentStream)
{
    ParticleSystem a = MakeConfiguredSystem();
    a.SeedRandom(ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID));
    const std::vector<u32> sigA = RunAndSnapshot(a, kFrames);

    ParticleSystem b = MakeConfiguredSystem();
    b.SeedRandom(ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID ^ 0x1ULL));
    const std::vector<u32> sigB = RunAndSnapshot(b, kFrames);

    ASSERT_GT(sigA[0], 5u);
    EXPECT_NE(sigA, sigB)
        << "distinct UUIDs produced identical streams — the UUID is not entering the seed";
}

// The regression that #452 left open: particle emission must NOT depend on the
// shared thread_local global RNG. Draw heavily from the global stream between
// every Update; the per-system stream — and therefore the particle output —
// must be untouched. Before the per-system RNG this interleaving shifted the
// jitter and the signatures would diverge.
TEST(ParticleSystemDeterminism, StreamIsIndependentOfGlobalRandom)
{
    const u64 seed = ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID);

    ParticleSystem baseline = MakeConfiguredSystem();
    baseline.SeedRandom(seed);
    const std::vector<u32> sigBaseline = RunAndSnapshot(baseline, kFrames);

    // Perturb the global stream (as a sibling loot roll / another emitter would)
    // before each frame's Update.
    ParticleSystem perturbed = MakeConfiguredSystem();
    perturbed.SeedRandom(seed);
    const auto churnGlobal = []
    {
        auto& g = RandomUtils::GetGlobalRandom();
        for (int k = 0; k < 7; ++k)
        {
            (void)g.GetFloat32();
        }
    };
    const std::vector<u32> sigPerturbed = RunAndSnapshot(perturbed, kFrames, churnGlobal);

    ASSERT_GT(sigBaseline[0], 5u);
    EXPECT_EQ(sigBaseline, sigPerturbed)
        << "particle emission changed when the global RNG was perturbed — the per-system "
           "stream is not fully isolated from RandomUtils::GetGlobalRandom()";
}

// DeriveSeed contract: pure, stable, and distinct across UUID and sub-stream.
TEST(ParticleSystemDeterminism, DeriveSeedIsStableAndDistinct)
{
    // Stable: same inputs ⇒ same output.
    EXPECT_EQ(ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID),
              ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID));

    // The parent stream (subStream 0) differs from every child sub-stream, and
    // the child sub-streams differ from each other — no two systems on one
    // entity share a stream.
    const u64 parent = ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID, 0);
    const u64 child0 = ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID, 1);
    const u64 child1 = ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID, 2);
    EXPECT_NE(parent, child0);
    EXPECT_NE(parent, child1);
    EXPECT_NE(child0, child1);

    // Distinct entities draw distinct parent streams under the same run seed.
    EXPECT_NE(ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID),
              ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID + 1));

    // A different run seed reshuffles a given entity's stream (replays under a
    // new seed do not alias the old run).
    EXPECT_NE(ParticleSystem::DeriveSeed(kRunSeed, kEntityUUID),
              ParticleSystem::DeriveSeed(kRunSeed + 1, kEntityUUID));

    // A zero UUID must not collapse to a degenerate seed equal to the run seed.
    EXPECT_NE(ParticleSystem::DeriveSeed(kRunSeed, 0), kRunSeed);
}
