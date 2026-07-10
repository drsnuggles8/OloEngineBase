// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Task/LocalQueue.h"

// Regression coverage for issue #611: TLocalQueueRegistry guarded two invariants only
// with OLO_CORE_ASSERT, which is a no-op in Dist builds:
//   - AddLocalQueue() overflow past MaxLocalQueues used to write out of bounds.
//   - StealItem() divided by NumQueues with no guard against NumQueues == 0.
// Both are now handled without relying on asserts, so these tests exercise the
// no-crash / no-UB behavior directly (not just under a debugger).

namespace
{
    using OloEngine::LowLevelTasks::Private::ELocalQueueType;
    using OloEngine::LowLevelTasks::Private::TLocalQueueRegistry;

    // Small MaxLocalQueues so the overflow path is reachable without creating
    // thousands of TLocalQueue instances.
    using FSmallRegistry = TLocalQueueRegistry<16, 2>;
} // namespace

TEST(LocalQueueTest, StealItemOnEmptyRegistryDoesNotCrash)
{
    FSmallRegistry Registry;

    // No TLocalQueue has ever registered with this registry: NumLocalQueues == 0.
    // Previously this computed CachedRandomIndex % 0 (undefined behavior / crash).
    EXPECT_EQ(Registry.DequeueSteal(/*GetBackGroundTasks*/ true), nullptr);
    EXPECT_EQ(Registry.DequeueSteal(/*GetBackGroundTasks*/ false), nullptr);
}

TEST(LocalQueueTest, AddLocalQueueBeyondCapacityDoesNotCorruptRegistry)
{
    FSmallRegistry Registry;

    // MaxLocalQueues is 2 for FSmallRegistry: register 2 successfully, then attempt a
    // 3rd, which must fail safely instead of writing past m_LocalQueues[MaxLocalQueues].
    FSmallRegistry::TLocalQueue QueueA;
    FSmallRegistry::TLocalQueue QueueB;
    FSmallRegistry::TLocalQueue QueueC;

    QueueA.Init(Registry, ELocalQueueType::EForeground);
    QueueB.Init(Registry, ELocalQueueType::EForeground);
    // This registration is expected to be rejected (registry at capacity); it must not
    // corrupt the registry's internal array or crash.
    QueueC.Init(Registry, ELocalQueueType::EForeground);

    // The registry must still be usable afterwards: stealing finds nothing (no tasks were
    // enqueued) but must not crash or read/write out of bounds.
    EXPECT_EQ(Registry.DequeueSteal(true), nullptr);
    EXPECT_EQ(Registry.DequeueSteal(false), nullptr);
}
