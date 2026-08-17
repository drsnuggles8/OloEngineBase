// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Task/Scheduler.h"

// Hardening for OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO: the value is read from the
// environment (untrusted config) and feeds the worker-thread budget math, where
// `ceil(workers * ratio)` is cast to i32. std::strtof("inf") yields +inf, and the
// old `inf >= 1.0f` guard let it through — non-finite / out-of-range values must
// be rejected. Levers::ParseNumberLever is the pure boundary that does that; it is
// shared by every OLO_LEVER_NUMBER in the debug-lever registry, so these cases now
// pin the parse for all of them rather than for this one variable. The bound under
// test is still kMaxOversubscriptionRatio, which is what the lever table passes.

namespace
{
    using OloEngine::LowLevelTasks::kMaxOversubscriptionRatio;

    // The registry's generic parse, bound to this lever's accepted range.
    std::optional<f32> ParseOversubscriptionRatio(const char* envValue)
    {
        return OloEngine::Levers::ParseNumberLever(envValue, 1.0f, kMaxOversubscriptionRatio);
    }
} // namespace

TEST(SchedulerConfigTest, NullEnvValueRejected)
{
    EXPECT_FALSE(ParseOversubscriptionRatio(nullptr).has_value());
}

TEST(SchedulerConfigTest, EmptyStringRejected)
{
    // std::strtof("") consumes no characters → rejected as not-a-number.
    EXPECT_FALSE(ParseOversubscriptionRatio("").has_value());
}

TEST(SchedulerConfigTest, GarbageStringRejected)
{
    // Unparsable → strtof consumes no characters → rejected as not-a-number.
    EXPECT_FALSE(ParseOversubscriptionRatio("not-a-number").has_value());
}

TEST(SchedulerConfigTest, PartiallyNumericStringRejected)
{
    // strtof parses the "2.0" prefix and stops at "abc"; requiring the whole string be
    // consumed rejects it rather than silently accepting the leading number.
    EXPECT_FALSE(ParseOversubscriptionRatio("2.0abc").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("2.0 ").has_value());
    EXPECT_FALSE(ParseOversubscriptionRatio("2x").has_value());
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
