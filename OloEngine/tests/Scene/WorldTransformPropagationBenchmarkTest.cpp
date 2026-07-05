// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// WorldTransformPropagationBenchmarkTest
//
// Perf smoke test for Scene::PropagateWorldTransforms() (issue #499): a flat,
// depth-sorted BFS sweep that composes parent-chain world matrices for every
// TransformComponent once per tick. Two shapes matter for the DOD claim:
//   - Flat (all-root) hierarchies: the pass should be ~O(N) with a small
//     constant, dominated by the BFS seed + the matrix-multiply sweep.
//   - Deep chains: same O(N) total work (each entity visited once), but this
//     exercises the queue-based BFS descent instead of the root fast-path.
//
// Follows the CommandBucketBenchmarkTest pattern: logs timings unconditionally
// (visible via ctest -V / direct exe run) and only *asserts* an upper bound
// when OLOENGINE_BENCH_ASSERT=1, since absolute timings vary by machine and a
// hard-coded threshold would make CI flaky on shared/loaded runners.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

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

    // N entities, all roots (no parenting) — the common case for e.g. the
    // ecs_static_100000 perf-stress scene referenced in issue #499.
    Ref<Scene> BuildFlatScene(u32 count)
    {
        Ref<Scene> scene = Scene::Create();
        for (u32 i = 0; i < count; ++i)
        {
            Entity e = scene->CreateEntity("Flat" + std::to_string(i));
            e.GetComponent<TransformComponent>().Translation = { static_cast<f32>(i), 0.0f, 0.0f };
        }
        return scene;
    }

    // `chainCount` independent chains of `chainDepth` entities each, so the
    // BFS must descend via RelationshipComponent::m_Children rather than
    // taking the root fast-path for every entity.
    Ref<Scene> BuildDeepChainScene(u32 chainCount, u32 chainDepth)
    {
        Ref<Scene> scene = Scene::Create();
        for (u32 c = 0; c < chainCount; ++c)
        {
            Entity parent = scene->CreateEntity("Chain" + std::to_string(c) + "_0");
            parent.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
            for (u32 d = 1; d < chainDepth; ++d)
            {
                Entity child = scene->CreateEntity("Chain" + std::to_string(c) + "_" + std::to_string(d));
                child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
                child.SetParent(parent);
                parent = child;
            }
        }
        return scene;
    }

    // Takes Ref<Scene> by value (not const&): Ref<T>::operator-> is const-qualified
    // to return `const T*`, so a `const Ref<Scene>&` param can't call the
    // non-const Scene::PropagateWorldTransforms() below.
    f64 TimePropagationMs(Ref<Scene> scene, u32 iterations)
    {
        // Warm up once so the first-call allocation cost isn't attributed
        // to the timed loop (this benchmark cares about steady-state cost).
        scene->PropagateWorldTransforms();

        auto const start = Clock::now();
        for (u32 i = 0; i < iterations; ++i)
        {
            scene->PropagateWorldTransforms();
        }
        auto const end = Clock::now();
        return std::chrono::duration<f64, std::milli>(end - start).count() / static_cast<f64>(iterations);
    }
} // namespace

TEST(WorldTransformPropagationBenchmark, FlatHierarchy_100000Entities)
{
    constexpr u32 kCount = 100'000;
    Ref<Scene> scene = BuildFlatScene(kCount);

    const f64 avgMs = TimePropagationMs(scene, 10);
    OLO_CORE_INFO("WorldTransformPropagationBenchmark: flat {0} entities -> {1:.3f} ms/call", kCount, avgMs);

    // Sanity: every entity actually got a composed world transform.
    auto view = scene->GetAllEntitiesWith<WorldTransformComponent>();
    EXPECT_EQ(view.size(), kCount);

    if (BenchAssertEnabled())
    {
        // Generous upper bound (measured ~290 ms on an unoptimized Debug
        // build) — this is a regression tripwire for an accidental
        // O(N^2)/O(N log N) change, not a tight perf gate; see
        // CommandBucketBenchmarkTest for the same 100K-scale generosity.
        EXPECT_LT(avgMs, 750.0) << "PropagateWorldTransforms regressed well beyond linear-scan cost for a flat 100k-entity scene";
    }
}

TEST(WorldTransformPropagationBenchmark, DeepChains_1000ChainsOf100)
{
    constexpr u32 kChains = 1'000;
    constexpr u32 kDepth = 100;
    Ref<Scene> scene = BuildDeepChainScene(kChains, kDepth);

    const f64 avgMs = TimePropagationMs(scene, 10);
    OLO_CORE_INFO("WorldTransformPropagationBenchmark: {0} chains x depth {1} ({2} entities) -> {3:.3f} ms/call",
                  kChains, kDepth, kChains * kDepth, avgMs);

    auto view = scene->GetAllEntitiesWith<WorldTransformComponent>();
    EXPECT_EQ(view.size(), kChains * kDepth);

    if (BenchAssertEnabled())
    {
        // Deep chains pay extra BFS-descent bookkeeping per entity (measured
        // ~1.1 s on an unoptimized Debug build) — same tripwire intent as above.
        EXPECT_LT(avgMs, 2500.0) << "PropagateWorldTransforms regressed well beyond linear-scan cost for 100k entities in deep chains";
    }
}
