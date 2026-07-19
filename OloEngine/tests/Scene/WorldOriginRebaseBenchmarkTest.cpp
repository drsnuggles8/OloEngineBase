// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// WorldOriginRebaseBenchmarkTest
//
// Perf characterization for Scene::RebaseOrigin (issue #613). A floating-origin
// rebase shifts every ROOT TransformComponent then re-propagates world matrices,
// so its cost scales with entity count — the "cost of a rebase over N thousand
// entities" question in #429's / #613's acceptance bar. This measures and logs
// ms/call for a few N so that question has a concrete, machine-recorded answer
// rather than a guess.
//
// Follows WorldTransformPropagationBenchmarkTest: logs timings unconditionally
// (visible via ctest -V / direct exe run) and only *asserts* an upper bound when
// OLOENGINE_BENCH_ASSERT=1, since absolute timings vary by machine and a hard
// threshold would make CI flaky on shared runners.
//
// Physics / navigation are intentionally NOT enabled here — this isolates the
// pure ECS transform-shift + propagate cost that dominates for large scenes.
// The physics-side rebase cost (body SetPosition loop, constraint anchor shift,
// navmesh rebake) is exercised for correctness by the Functional rebase tests.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    bool BenchAssertEnabled()
    {
        const char* env = std::getenv("OLOENGINE_BENCH_ASSERT"); // NOLINT(concurrency-mt-unsafe)
        return env && std::string(env) == "1";
    }

    using Clock = std::chrono::high_resolution_clock;

    // N root entities spread along a diagonal — the realistic "many entities in
    // a large world" case a rebase has to shift.
    Ref<Scene> BuildFlatScene(u32 count)
    {
        Ref<Scene> scene = Scene::Create();
        for (u32 i = 0; i < count; ++i)
        {
            Entity e = scene->CreateEntity("E" + std::to_string(i));
            e.GetComponent<TransformComponent>().Translation = {
                static_cast<f32>(i), 0.0f, static_cast<f32>(i)
            };
        }
        return scene;
    }

    // Time a single RebaseOrigin averaged over `iterations`. The shift sign
    // alternates so repeated rebases don't march coordinates off toward infinity
    // (net zero every even count).
    f64 TimeRebaseMs(Ref<Scene> scene, u32 iterations)
    {
        scene->PropagateWorldTransforms(); // warm up allocations
        auto const start = Clock::now();
        for (u32 i = 0; i < iterations; ++i)
        {
            const f32 s = (i & 1u) ? -1024.0f : 1024.0f;
            scene->RebaseOrigin(glm::vec3(s, 0.0f, s));
        }
        auto const end = Clock::now();
        return std::chrono::duration<f64, std::milli>(end - start).count() / static_cast<f64>(iterations);
    }
} // namespace

TEST(WorldOriginRebaseBenchmark, Flat_10000Entities)
{
    constexpr u32 kCount = 10'000;
    Ref<Scene> scene = BuildFlatScene(kCount);

    const f64 avgMs = TimeRebaseMs(scene, 20);
    OLO_CORE_INFO("WorldOriginRebaseBenchmark: rebase over {0} entities -> {1:.3f} ms/call", kCount, avgMs);

    if (BenchAssertEnabled())
    {
        // Generous tripwire for an accidental super-linear regression (shift is
        // an O(N) view sweep + an O(N) propagate); not a tight gate.
        EXPECT_LT(avgMs, 250.0) << "RebaseOrigin regressed well beyond linear cost for 10k entities";
    }
}

TEST(WorldOriginRebaseBenchmark, Flat_50000Entities)
{
    constexpr u32 kCount = 50'000;
    Ref<Scene> scene = BuildFlatScene(kCount);

    const f64 avgMs = TimeRebaseMs(scene, 10);
    OLO_CORE_INFO("WorldOriginRebaseBenchmark: rebase over {0} entities -> {1:.3f} ms/call", kCount, avgMs);

    if (BenchAssertEnabled())
    {
        EXPECT_LT(avgMs, 1250.0) << "RebaseOrigin regressed well beyond linear cost for 50k entities";
    }
}
