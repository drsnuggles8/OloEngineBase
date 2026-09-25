#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCacheBudgetWarningTest — issue #1431.
//
// The strand cache staying over its budget with every entry in use is a steady
// state, and it used to be logged every frame (about five lines a second on
// GroomAnimals.olo). GroomCacheBudgetWarningGate decides when the line is said:
// on entry into the state, again only when the overage grows materially, and
// once more after the cache has been back under the budget. The pass itself is
// driven through the same gate in GroomStrandVisualEvidenceTest.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Passes/GroomRenderPass.h"

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u64 kMiB = 1024ull * 1024ull;

        u32 CountLogged(GroomCacheBudgetWarningGate& gate, u64 overBytes, u32 frames)
        {
            u32 logged = 0;
            for (u32 i = 0; i < frames; ++i)
            {
                logged += gate.Observe(overBytes) ? 1u : 0u;
            }
            return logged;
        }
    } // namespace

    TEST(GroomCacheBudgetWarning, WithinBudgetNeverLogs)
    {
        GroomCacheBudgetWarningGate gate;
        EXPECT_EQ(CountLogged(gate, 0, 300), 0u);
    }

    TEST(GroomCacheBudgetWarning, ASteadyOverageLogsOnceNotEveryFrame)
    {
        // The issue's own numbers: 198.2 MiB over a 256 MiB budget, every frame.
        GroomCacheBudgetWarningGate gate;
        const u64 over = static_cast<u64>(198.2 * static_cast<f64>(kMiB));
        EXPECT_EQ(CountLogged(gate, over, 600), 1u);
        EXPECT_EQ(gate.LoggedOverBytes(), over);
    }

    TEST(GroomCacheBudgetWarning, MaterialGrowthLogsAgainAndSmallGrowthDoesNot)
    {
        GroomCacheBudgetWarningGate gate;
        const u64 over = 100 * kMiB;
        ASSERT_TRUE(gate.Observe(over));

        // A few percent more is the same state, still quiet.
        EXPECT_EQ(CountLogged(gate, over + 10 * kMiB, 60), 0u);
        // Wobbling down and back up to where it was is not growth either.
        EXPECT_EQ(CountLogged(gate, over / 2, 60), 0u);
        EXPECT_EQ(CountLogged(gate, over, 60), 0u);

        // A quarter more than the line last said is material: one line, then
        // quiet at the new level.
        EXPECT_EQ(CountLogged(gate, over * 5 / 4, 60), 1u);
        EXPECT_EQ(gate.LoggedOverBytes(), over * 5 / 4);
        // The next step is measured from THAT line, not from the first.
        EXPECT_EQ(CountLogged(gate, over * 5 / 4 + 10 * kMiB, 60), 0u);
        EXPECT_EQ(CountLogged(gate, over * 2, 60), 1u);
    }

    TEST(GroomCacheBudgetWarning, ATinyOverageCreepingUpNeedsAMebibyteToRelog)
    {
        // 25% of a few KiB is a few KiB; the absolute floor keeps a small
        // overage creeping up a page at a time from logging at every step.
        GroomCacheBudgetWarningGate gate;
        ASSERT_TRUE(gate.Observe(4096));
        u32 logged = 0;
        for (u64 over = 8192; over < kMiB; over += 4096)
        {
            logged += gate.Observe(over) ? 1u : 0u;
        }
        EXPECT_EQ(logged, 0u);
        EXPECT_TRUE(gate.Observe(4096 + kMiB));
    }

    TEST(GroomCacheBudgetWarning, ReturningUnderBudgetReArmsTheWarning)
    {
        GroomCacheBudgetWarningGate gate;
        EXPECT_EQ(CountLogged(gate, 50 * kMiB, 30), 1u);
        EXPECT_EQ(CountLogged(gate, 0, 30), 0u);
        EXPECT_EQ(gate.LoggedOverBytes(), 0u);
        // A fresh entry into the state is news again, even at a smaller overage.
        EXPECT_EQ(CountLogged(gate, 10 * kMiB, 30), 1u);
    }
} // namespace OloEngine::Tests
