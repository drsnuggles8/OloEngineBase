// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Task/Scheduler.h"

// Hardening for OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO: the value is read from the
// environment (untrusted config) and feeds the worker-thread budget math, where
// `ceil(workers * ratio)` is cast to i32. std::atof("inf") yields +inf, and the
// old `inf >= 1.0f` guard let it through — non-finite / out-of-range values must
// be rejected. ParseOversubscriptionRatio is the pure boundary that does that.

namespace
{
    using OloEngine::LowLevelTasks::kMaxOversubscriptionRatio;
    using OloEngine::LowLevelTasks::ParseOversubscriptionRatio;
} // namespace

TEST(SchedulerConfigTest, NullEnvValueRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio(nullptr).has_value());
}

TEST(SchedulerConfigTest, EmptyStringRejected)
{
    // std::atof("") == 0.0, which is below the minimum of 1.0.
    EXPECT_FALSE(ParseOversubscriptionRatio("").has_value());
}

TEST(SchedulerConfigTest, GarbageStringRejected)
{
    // Unparsable → atof returns 0.0 → below minimum.
    EXPECT_FALSE(ParseOversubscriptionRatio("not-a-number").has_value());
}

TEST(SchedulerConfigTest, PositiveInfinityRejected)
{
    // The crux: "inf" parses to +inf and must NOT pass the >= 1.0 guard.
    EXPECT_FALSE(ParseOversubscriptionRatio("inf").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("INF").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("infinity").has_value());
}

TEST(SchedulerConfigTest, NegativeInfinityRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio("-inf").has_value());
}

TEST(SchedulerConfigTest, NaNRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio("nan").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("NaN").has_value());
}

TEST(SchedulerConfigTest, BelowMinimumRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio("0.5").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("0").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("-2.0").has_value());
}

TEST(SchedulerConfigTest, AboveMaximumRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio("1000").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("1e9").has_value());
}

TEST(SchedulerConfigTest, ValidRatioAccepted)
{
    auto result = ParseOversubscriptionRatio("2.0");
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(*result, 2.0f);
}

TEST(SchedulerConfigTest, LowerBoundInclusive)
{
    auto result = ParseOversubscriptionRatio("1.0");
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(*result, 1.0f);
}

TEST(SchedulerConfigTest, UpperBoundInclusive)
{
    auto result = ParseOversubscriptionRatio("64");
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(*result, kMaxOversubscriptionRatio);
}
