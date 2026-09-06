// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Task/ParallelFor.h"

#include <atomic>

// The calling thread is a worker too, and it takes ONE MORE context slot than
// the launched tasks do.
//
// ParallelForInternal decrements NumWorkers before sizing the task array, so
// the tasks take context indices 0..NumWorkers-1 and the master executor is
// constructed with NumWorkers as its own index. The clamp to Contexts.Num()
// is what keeps that last index inside a caller-supplied array. Nothing
// currently states that relationship in a test, so a future edit to either the
// clamp or the decrement would silently hand a body a context that was never
// constructed — which reads as garbage rather than as an out-of-range error.
//
// #1013 added the first caller with a FIXED-size context array
// (MAX_RENDER_WORKERS slots for mesh submission), where the clamp is load-
// bearing rather than incidental.
namespace
{
    using namespace OloEngine; // NOLINT(google-build-using-namespace)

    struct MarkedContext
    {
        // A value no default-constructed or uninitialised slot would carry.
        u32 Magic = 0xC0FFEEu;
        i32 Index = -1;
        std::atomic<i32> Calls{ 0 };
    };

    TEST(ParallelForExistingContexts, NeverIndexesPastTheCallerSuppliedContexts)
    {
        // Deliberately fewer contexts than this machine has hardware threads,
        // which is the shape that made the clamp land on Contexts.Num().
        constexpr i32 kContexts = 4;
        constexpr i32 kWork = 4096;

        std::vector<MarkedContext> contexts(static_cast<sizet>(kContexts));
        for (i32 index = 0; index < kContexts; ++index)
            contexts[static_cast<sizet>(index)].Index = index;

        std::atomic<i32> bodyCalls{ 0 };
        std::atomic<bool> sawForeignContext{ false };
        ParallelForWithExistingTaskContext(
            "ParallelForExistingContextsTest",
            TArrayView<MarkedContext>(contexts.data(), kContexts),
            kWork,
            /*MinBatchSize=*/1,
            [&](MarkedContext& context, i32)
            {
                // A context outside the array reads as anything at all; the
                // magic and the self-consistent index are what a real slot has.
                if (context.Magic != 0xC0FFEEu || context.Index < 0 || context.Index >= kContexts)
                    sawForeignContext.store(true, std::memory_order_relaxed);
                else
                    context.Calls.fetch_add(1, std::memory_order_relaxed);
                bodyCalls.fetch_add(1, std::memory_order_relaxed);
            });

        EXPECT_FALSE(sawForeignContext.load()) << "ParallelFor handed the body a context outside the supplied array — "
                                                  "the calling thread's slot was not reserved";
        EXPECT_EQ(bodyCalls.load(), kWork);

        i32 accounted = 0;
        for (const auto& context : contexts)
            accounted += context.Calls.load();
        EXPECT_EQ(accounted, kWork) << "every iteration must be accounted for by a real context slot";
    }

    // The degenerate case the clamp has to survive: one context means no worker
    // task at all, and the master runs the whole range on slot 0.
    TEST(ParallelForExistingContexts, SingleContextRunsEverythingOnThatContext)
    {
        std::vector<MarkedContext> contexts(1);
        contexts[0].Index = 0;

        constexpr i32 kWork = 256;
        ParallelForWithExistingTaskContext(
            "ParallelForSingleContextTest", TArrayView<MarkedContext>(contexts.data(), 1), kWork,
            /*MinBatchSize=*/1, [](MarkedContext& context, i32)
            { context.Calls.fetch_add(1, std::memory_order_relaxed); });

        EXPECT_EQ(contexts[0].Calls.load(), kWork);
        EXPECT_EQ(contexts[0].Magic, 0xC0FFEEu);
    }
} // namespace
