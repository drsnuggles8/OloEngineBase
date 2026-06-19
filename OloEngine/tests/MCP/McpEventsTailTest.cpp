#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the unified diagnostics event ring buffer behind olo_events_tail
// (issue #306 item B). The buffer is a header-only engine facility with no editor /
// GPU / scene dependency, so it is exercised here directly — push, wrap/overflow,
// the sinceId incremental-poll cursor, the category filter, the newest-N cap, and the
// bulk-suppression scope. The live MCP tool (which only serializes these records) is
// verified separately over the attach loop.
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <string>
#include <vector>

namespace
{
    using OloEngine::DiagnosticEvent;
    using OloEngine::DiagnosticEventCategory;
    using OloEngine::DiagnosticEventQuery;
    using OloEngine::DiagnosticsEventLog;

    // The event log is a process-wide singleton, and other tests in this binary
    // (Functional world-tick tests) drive the very seams that record into it. Clearing
    // before and after each test isolates the assertions from that shared state.
    class DiagnosticsEventLogTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            DiagnosticsEventLog::Get().Clear();
        }
        void TearDown() override
        {
            DiagnosticsEventLog::Get().Clear();
        }

        static DiagnosticsEventLog& Log()
        {
            return DiagnosticsEventLog::Get();
        }

        // Convenience: query with no filter and an effectively unbounded cap.
        static std::vector<DiagnosticEvent> All()
        {
            DiagnosticEventQuery query;
            query.MaxCount = 0; // 0 = no cap
            return Log().Query(query);
        }
    };
} // namespace

TEST_F(DiagnosticsEventLogTest, RecordAssignsMonotonicOneBasedIds)
{
    EXPECT_EQ(0u, Log().LastId());
    EXPECT_EQ(1u, Log().Record(DiagnosticEventCategory::Play, "a"));
    EXPECT_EQ(2u, Log().Record(DiagnosticEventCategory::Stop, "b"));
    EXPECT_EQ(3u, Log().Record(DiagnosticEventCategory::SceneLoad, "c"));
    EXPECT_EQ(3u, Log().LastId());
}

TEST_F(DiagnosticsEventLogTest, QueryReturnsOldestFirstNewestLast)
{
    Log().Record(DiagnosticEventCategory::Play, "first");
    Log().Record(DiagnosticEventCategory::Stop, "second");

    const std::vector<DiagnosticEvent> events = All();
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("first", events.front().Message);
    EXPECT_EQ("second", events.back().Message);
    EXPECT_LT(events.front().Id, events.back().Id);
}

TEST_F(DiagnosticsEventLogTest, RecordPreservesEntityAndContext)
{
    Log().Record(DiagnosticEventCategory::EntitySpawn, "Spawned entity 'Hero'", 4242u, "MyScene");

    const std::vector<DiagnosticEvent> events = All();
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(DiagnosticEventCategory::EntitySpawn, events[0].Category);
    EXPECT_EQ(4242u, events[0].Entity);
    EXPECT_EQ("MyScene", events[0].Context);
    EXPECT_GT(events[0].Timestamp, 0.0);
}

TEST_F(DiagnosticsEventLogTest, SinceIdReturnsOnlyStrictlyNewer)
{
    const u64 id1 = Log().Record(DiagnosticEventCategory::Play, "1");
    Log().Record(DiagnosticEventCategory::Stop, "2");
    Log().Record(DiagnosticEventCategory::SceneLoad, "3");

    DiagnosticEventQuery query;
    query.SinceId = id1; // strictly greater than id1
    const std::vector<DiagnosticEvent> events = Log().Query(query);
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("2", events.front().Message);
    EXPECT_EQ("3", events.back().Message);
}

TEST_F(DiagnosticsEventLogTest, SinceIdAtLatestReturnsEmptyAndCursorIsStable)
{
    Log().Record(DiagnosticEventCategory::Play, "1");
    Log().Record(DiagnosticEventCategory::Stop, "2");
    const u64 cursor = Log().LastId();

    DiagnosticEventQuery query;
    query.SinceId = cursor;
    EXPECT_TRUE(Log().Query(query).empty());
    // Polling with the same cursor must not move it.
    EXPECT_EQ(cursor, Log().LastId());
}

TEST_F(DiagnosticsEventLogTest, CategoryFilterKeepsOnlyRequested)
{
    Log().Record(DiagnosticEventCategory::EntitySpawn, "spawn");
    Log().Record(DiagnosticEventCategory::ScriptError, "boom");
    Log().Record(DiagnosticEventCategory::EntityDestroy, "destroy");

    DiagnosticEventQuery query;
    query.Categories = { DiagnosticEventCategory::ScriptError };
    const std::vector<DiagnosticEvent> events = Log().Query(query);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("boom", events[0].Message);
}

TEST_F(DiagnosticsEventLogTest, CategoryFilterAcceptsMultiple)
{
    Log().Record(DiagnosticEventCategory::EntitySpawn, "spawn");
    Log().Record(DiagnosticEventCategory::ScriptError, "boom");
    Log().Record(DiagnosticEventCategory::EntityDestroy, "destroy");
    Log().Record(DiagnosticEventCategory::Play, "play");

    DiagnosticEventQuery query;
    query.Categories = { DiagnosticEventCategory::EntitySpawn, DiagnosticEventCategory::EntityDestroy };
    const std::vector<DiagnosticEvent> events = Log().Query(query);
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("spawn", events.front().Message);
    EXPECT_EQ("destroy", events.back().Message);
}

TEST_F(DiagnosticsEventLogTest, MaxCountKeepsNewest)
{
    for (int i = 1; i <= 5; ++i)
        Log().Record(DiagnosticEventCategory::Play, std::to_string(i));

    DiagnosticEventQuery query;
    query.MaxCount = 2;
    const std::vector<DiagnosticEvent> events = Log().Query(query);
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("4", events.front().Message);
    EXPECT_EQ("5", events.back().Message);
}

TEST_F(DiagnosticsEventLogTest, MaxCountAndSinceIdCompose)
{
    for (int i = 1; i <= 6; ++i)
        Log().Record(DiagnosticEventCategory::Play, std::to_string(i));

    DiagnosticEventQuery query;
    query.SinceId = 2;  // ids 3,4,5,6 eligible
    query.MaxCount = 2; // keep newest two of those
    const std::vector<DiagnosticEvent> events = Log().Query(query);
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("5", events.front().Message);
    EXPECT_EQ("6", events.back().Message);
}

TEST_F(DiagnosticsEventLogTest, RingBufferWrapsAndDropsOldestKeepingNewest)
{
    // Record far more than any sane capacity; the oldest must be evicted while ids keep
    // climbing. Asserted capacity-agnostically: whatever the retained window size S is,
    // it must be smaller than what we recorded (proving eviction), end at the newest id,
    // and be a contiguous run back from there.
    constexpr u64 recorded = 5000;
    for (u64 i = 0; i < recorded; ++i)
        Log().Record(DiagnosticEventCategory::EntitySpawn, "x");

    EXPECT_EQ(recorded, Log().LastId());

    const std::vector<DiagnosticEvent> events = All();
    ASSERT_FALSE(events.empty());
    EXPECT_LT(events.size(), recorded) << "ring buffer did not evict the oldest events";
    EXPECT_EQ(recorded, events.back().Id);
    EXPECT_EQ(recorded - events.size() + 1, events.front().Id);
    // Ids are contiguous and strictly increasing across the retained window.
    for (std::size_t i = 1; i < events.size(); ++i)
        EXPECT_EQ(events[i - 1].Id + 1, events[i].Id);
}

TEST_F(DiagnosticsEventLogTest, SuppressScopeMutesRecordingThenRestores)
{
    Log().Record(DiagnosticEventCategory::Play, "before");
    {
        DiagnosticsEventLog::SuppressScope suppress;
        EXPECT_EQ(0u, Log().Record(DiagnosticEventCategory::EntitySpawn, "muted"));
        EXPECT_EQ(0u, Log().Record(DiagnosticEventCategory::EntitySpawn, "also muted"));
    }
    Log().Record(DiagnosticEventCategory::Stop, "after");

    const std::vector<DiagnosticEvent> events = All();
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ("before", events.front().Message);
    EXPECT_EQ("after", events.back().Message);
    // The muted records consumed no ids — the surviving pair is 1 and 2.
    EXPECT_EQ(1u, events.front().Id);
    EXPECT_EQ(2u, events.back().Id);
}

TEST_F(DiagnosticsEventLogTest, SuppressScopeNestsByDepth)
{
    {
        DiagnosticsEventLog::SuppressScope outer;
        {
            DiagnosticsEventLog::SuppressScope inner;
            EXPECT_EQ(0u, Log().Record(DiagnosticEventCategory::EntitySpawn, "x"));
        }
        // Still suppressed: the outer scope is alive.
        EXPECT_EQ(0u, Log().Record(DiagnosticEventCategory::EntitySpawn, "y"));
    }
    // Both scopes gone — recording resumes.
    EXPECT_NE(0u, Log().Record(DiagnosticEventCategory::Play, "z"));
    EXPECT_EQ(1u, All().size());
}

TEST_F(DiagnosticsEventLogTest, ClearResetsBufferAndIdCounter)
{
    Log().Record(DiagnosticEventCategory::Play, "a");
    Log().Record(DiagnosticEventCategory::Stop, "b");
    Log().Clear();

    EXPECT_EQ(0u, Log().LastId());
    EXPECT_TRUE(All().empty());
    EXPECT_EQ(1u, Log().Record(DiagnosticEventCategory::SceneLoad, "fresh"));
}

TEST(DiagnosticsEventCategory, StringRoundTripsForEveryCategory)
{
    constexpr DiagnosticEventCategory all[] = {
        DiagnosticEventCategory::SceneLoad,
        DiagnosticEventCategory::Play,
        DiagnosticEventCategory::Stop,
        DiagnosticEventCategory::EntitySpawn,
        DiagnosticEventCategory::EntityDestroy,
        DiagnosticEventCategory::AssetReload,
        DiagnosticEventCategory::ScriptError,
    };
    static_assert(std::size(all) == OloEngine::kDiagnosticEventCategoryCount,
                  "update this list when DiagnosticEventCategory changes");

    for (const DiagnosticEventCategory category : all)
    {
        const char* token = DiagnosticEvent::CategoryToString(category);
        EXPECT_STRNE("unknown", token);
        DiagnosticEventCategory parsed{};
        ASSERT_TRUE(DiagnosticEvent::CategoryFromString(token, parsed)) << "token: " << token;
        EXPECT_EQ(category, parsed);
    }
}

TEST(DiagnosticsEventCategory, FromStringRejectsUnknownToken)
{
    DiagnosticEventCategory parsed{};
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("not_a_category", parsed));
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("", parsed));
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("SceneLoad", parsed)); // exact token only
}
