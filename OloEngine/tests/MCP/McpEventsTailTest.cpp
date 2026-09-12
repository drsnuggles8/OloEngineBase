#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the unified diagnostics event ring buffer behind olo_events_tail
// (issue #306). The buffer is a header-only engine facility with no editor /
// GPU / scene dependency, so it is exercised here directly — push, wrap/overflow,
// the sinceId incremental-poll cursor, the category filter, the newest-N cap, and the
// bulk-suppression scope. The live MCP tool (which only serializes these records) is
// verified separately over the attach loop.
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using OloEngine::DiagnosticEvent;
    using OloEngine::DiagnosticEventCategory;
    using OloEngine::DiagnosticEventQuery;
    using OloEngine::DiagnosticEventQueryResult;
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

TEST_F(DiagnosticsEventLogTest, QueryWithCursorReportsBufferCursorNotFilteredMax)
{
    Log().Record(DiagnosticEventCategory::Play, "1");
    Log().Record(DiagnosticEventCategory::EntitySpawn, "2"); // filtered out below
    Log().Record(DiagnosticEventCategory::Stop, "3");        // newest match
    Log().Record(DiagnosticEventCategory::EntitySpawn, "4"); // filtered out, but newest in buffer

    DiagnosticEventQuery query;
    query.Categories = { DiagnosticEventCategory::Play, DiagnosticEventCategory::Stop };
    const DiagnosticEventQueryResult result = Log().QueryWithCursor(query);

    // Events match Query() exactly (the spawns are excluded)...
    ASSERT_EQ(2u, result.Events.size());
    EXPECT_EQ("1", result.Events.front().Message);
    EXPECT_EQ("3", result.Events.back().Message);

    // ...but the cursor is the newest id in the WHOLE buffer (the filtered-out id 4),
    // not the newest returned event (id 3). Otherwise a follow-up sinceId poll would
    // re-deliver id 4 the client deliberately filtered away.
    EXPECT_EQ(4u, result.LastId);
    EXPECT_EQ(Log().LastId(), result.LastId);
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
        DiagnosticEventCategory::SceneSave,
        DiagnosticEventCategory::SceneDirty,
        DiagnosticEventCategory::AssetImport,
        DiagnosticEventCategory::CompileFinished,
        DiagnosticEventCategory::CommandCompleted,
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

    // AllCategoryTokens is what the tool schemas and error messages are built
    // from: it must list exactly these, in enum order.
    const auto tokens = DiagnosticEvent::AllCategoryTokens();
    ASSERT_EQ(tokens.size(), std::size(all));
    for (std::size_t i = 0; i < tokens.size(); ++i)
        EXPECT_EQ(tokens[i], DiagnosticEvent::CategoryToString(all[i]));
}

// ---- #1131: the structured payload and its closed key set ------------------

TEST_F(DiagnosticsEventLogTest, DataBuilderEmitsCompactJsonWithEscapedStrings)
{
    OloEngine::DiagnosticEventData data;
    EXPECT_TRUE(data.Empty());
    EXPECT_EQ(data.Build(), "");
    data.Set("scene", "Say \"hi\"\n").Set("changed", true).Set("handle", u64{ 42 }).Set("seconds", 1.5);
    EXPECT_FALSE(data.Empty());
    EXPECT_EQ(data.Build(), R"({"scene":"Say \"hi\"\n","changed":true,"handle":42,"seconds":1.5})");
    // A repeated key replaces the value in place rather than duplicating it.
    data.Set("changed", false);
    EXPECT_EQ(data.Build(), R"({"scene":"Say \"hi\"\n","changed":false,"handle":42,"seconds":1.5})");
}

TEST_F(DiagnosticsEventLogTest, DataBuilderTruncatesLongValuesVisibly)
{
    const std::string document(OloEngine::DiagnosticEventData::kMaxDataValueChars * 3, 'x');
    OloEngine::DiagnosticEventData data;
    data.Set("scene", document);
    const std::string built = data.Build();
    EXPECT_LT(built.size(), document.size());
    EXPECT_NE(built.find("..."), std::string::npos) << "truncation must be visible in the record";
}

TEST_F(DiagnosticsEventLogTest, RecordStoresDataForAnAllowedKeySet)
{
    OloEngine::DiagnosticEventData data;
    data.Set("scene", "Level1").Set("dirty", true);
    const u64 id = Log().Record(DiagnosticEventCategory::SceneDirty, "dirty", 0, "Level1", data);
    ASSERT_EQ(1u, id);
    const auto events = All();
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(events[0].Data, R"({"scene":"Level1","dirty":true})");
}

TEST_F(DiagnosticsEventLogTest, LegacyCategoriesCarryNoData)
{
    Log().Record(DiagnosticEventCategory::Play, "Entered Play mode", 0, "Scene");
    const auto events = All();
    ASSERT_EQ(1u, events.size());
    EXPECT_TRUE(events[0].Data.empty());
    EXPECT_TRUE(OloEngine::DiagnosticEventDataKeys(DiagnosticEventCategory::Play).empty());
}

TEST_F(DiagnosticsEventLogTest, EveryCategoryDeclaresExactlyItsClosedKeySet)
{
    // The payload rule, pinned as the exact table: a key added to a category
    // (or one removed) fails here, in a diff a reviewer sees, rather than
    // widening what a subscriber can be shown. The seven older categories carry
    // no payload at all.
    using Keys = std::vector<std::string_view>;
    const std::vector<std::pair<DiagnosticEventCategory, Keys>> expected = {
        { DiagnosticEventCategory::SceneLoad, {} },
        { DiagnosticEventCategory::Play, {} },
        { DiagnosticEventCategory::Stop, {} },
        { DiagnosticEventCategory::EntitySpawn, {} },
        { DiagnosticEventCategory::EntityDestroy, {} },
        { DiagnosticEventCategory::AssetReload, {} },
        { DiagnosticEventCategory::ScriptError, {} },
        { DiagnosticEventCategory::SceneSave, { "scene", "path", "changed" } },
        { DiagnosticEventCategory::SceneDirty, { "scene", "dirty" } },
        { DiagnosticEventCategory::AssetImport, { "asset", "type", "handle", "source" } },
        { DiagnosticEventCategory::CompileFinished, { "kind", "target", "ok", "errors", "warnings", "seconds" } },
        { DiagnosticEventCategory::CommandCompleted, { "command", "ok", "durationMs", "projectWrite", "toolset" } },
    };
    ASSERT_EQ(expected.size(), OloEngine::kDiagnosticEventCategoryCount) << "add the new category's key set here";
    for (const auto& [category, keys] : expected)
    {
        const auto declared = OloEngine::DiagnosticEventDataKeys(category);
        EXPECT_EQ(Keys(declared.begin(), declared.end()), keys) << DiagnosticEvent::CategoryToString(category);
    }
}

TEST_F(DiagnosticsEventLogTest, SuppressCategoryScopeMutesOneCategoryOnThisThreadOnly)
{
    {
        const DiagnosticsEventLog::SuppressCategoryScope quiet(DiagnosticEventCategory::CommandCompleted);
        EXPECT_EQ(0u, Log().Record(DiagnosticEventCategory::CommandCompleted, "muted"));
        EXPECT_EQ(1u, Log().Record(DiagnosticEventCategory::Play, "other categories still record"));
        // Another thread is unaffected: the scope is thread-local by design.
        u64 fromOtherThread = 0;
        std::thread other([&fromOtherThread]
                          { fromOtherThread = DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::CommandCompleted, "elsewhere"); });
        other.join();
        EXPECT_EQ(2u, fromOtherThread);
    }
    EXPECT_EQ(3u, Log().Record(DiagnosticEventCategory::CommandCompleted, "scope ended"));
}

TEST_F(DiagnosticsEventLogTest, DataBuilderTruncatesOnAUtf8Boundary)
{
    // 90 three-byte characters = 270 bytes; the cut at 256 lands mid-character
    // and must back off so the value stays valid UTF-8.
    std::string text;
    for (int i = 0; i < 90; ++i)
        text += "\xE3\x81\x82"; // U+3042
    OloEngine::DiagnosticEventData data;
    data.Set("scene", text);
    const std::string built = data.Build();
    const std::size_t cut = built.find("...");
    ASSERT_NE(cut, std::string::npos);
    // Every character before the marker is whole: the byte count is a multiple of 3.
    const std::size_t valueStart = built.find(':') + 2; // past `{"scene":"`
    EXPECT_EQ((cut - valueStart) % 3, 0u);
}

// ---- #1131: the reported gap ------------------------------------------------

TEST_F(DiagnosticsEventLogTest, CursorInsideTheWindowReportsNoGap)
{
    for (int i = 0; i < 5; ++i)
        Log().Record(DiagnosticEventCategory::Play, "p");
    DiagnosticEventQuery query;
    query.SinceId = 2;
    query.MaxCount = 0;
    const DiagnosticEventQueryResult result = Log().QueryWithCursor(query);
    EXPECT_EQ(0u, result.Dropped);
    EXPECT_EQ(3u, result.Events.size());
    // A cursor at the head, and a cursor with no lower bound, report no gap either.
    query.SinceId = 5;
    EXPECT_EQ(0u, Log().QueryWithCursor(query).Dropped);
    query.SinceId = 0;
    EXPECT_EQ(0u, Log().QueryWithCursor(query).Dropped);
}

TEST_F(DiagnosticsEventLogTest, CursorBehindTheWindowReportsHowManyWereEvicted)
{
    const std::size_t capacity = DiagnosticsEventLog::kCapacity;
    for (std::size_t i = 0; i < capacity + 10; ++i)
        Log().Record(DiagnosticEventCategory::EntitySpawn, "e");
    // Ids 1..10 have been evicted; the oldest retained is 11.
    DiagnosticEventQuery query;
    query.SinceId = 3;
    query.MaxCount = 0;
    const DiagnosticEventQueryResult result = Log().QueryWithCursor(query);
    EXPECT_EQ(7u, result.Dropped) << "ids 4..10 were above the cursor and are gone";
    ASSERT_FALSE(result.Events.empty());
    EXPECT_EQ(11u, result.Events.front().Id);
    EXPECT_EQ(capacity + 10, result.LastId);
    // The gap is counted before the category filter: a filter that matches
    // nothing still reports the loss, because the lost records' categories are
    // unknown.
    query.Categories = { DiagnosticEventCategory::Play };
    const DiagnosticEventQueryResult filtered = Log().QueryWithCursor(query);
    EXPECT_TRUE(filtered.Events.empty());
    EXPECT_EQ(7u, filtered.Dropped);
}

// ---- #1131: the long-poll wait ---------------------------------------------

TEST_F(DiagnosticsEventLogTest, WaitReturnsImmediatelyWhenAMatchAlreadyExists)
{
    Log().Record(DiagnosticEventCategory::Play, "p");
    DiagnosticEventQuery query;
    query.SinceId = 0;
    const auto started = std::chrono::steady_clock::now();
    const DiagnosticEventQueryResult result =
        Log().WaitWithCursor(query, std::chrono::seconds(5), []
                             { return false; });
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    ASSERT_EQ(1u, result.Events.size());
    EXPECT_EQ(1u, result.LastId);
}

TEST_F(DiagnosticsEventLogTest, WaitTimesOutWithTheCursorWhenNothingMatches)
{
    Log().Record(DiagnosticEventCategory::Play, "p");
    DiagnosticEventQuery query;
    query.SinceId = 1;
    const DiagnosticEventQueryResult result =
        Log().WaitWithCursor(query, std::chrono::milliseconds(50), []
                             { return false; });
    EXPECT_TRUE(result.Events.empty());
    EXPECT_EQ(1u, result.LastId) << "a timeout still hands back the cursor to resume from";
}

TEST_F(DiagnosticsEventLogTest, WaitWakesWhenAMatchingEventIsRecorded)
{
    DiagnosticEventQuery query;
    query.SinceId = 0;
    query.Categories = { DiagnosticEventCategory::Stop };
    std::thread recorder(
        []
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "ignored by the filter");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Stop, "the one");
        });
    const DiagnosticEventQueryResult result =
        Log().WaitWithCursor(query, std::chrono::seconds(10), []
                             { return false; });
    recorder.join();
    ASSERT_EQ(1u, result.Events.size());
    EXPECT_EQ(DiagnosticEventCategory::Stop, result.Events[0].Category);
    EXPECT_EQ(2u, result.LastId);
}

TEST_F(DiagnosticsEventLogTest, WaitStopsEarlyWhenCancelled)
{
    std::atomic<bool> cancelled{ false };
    DiagnosticEventQuery query;
    query.SinceId = 0;
    std::thread canceller(
        [&cancelled]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cancelled.store(true);
        });
    const auto started = std::chrono::steady_clock::now();
    const DiagnosticEventQueryResult result = Log().WaitWithCursor(
        query, std::chrono::seconds(30), [&cancelled]
        { return cancelled.load(); });
    canceller.join();
    EXPECT_TRUE(result.Events.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5))
        << "cancellation is polled every 250 ms, not at the deadline";
}

TEST(DiagnosticsEventCategory, FromStringRejectsUnknownToken)
{
    DiagnosticEventCategory parsed{};
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("not_a_category", parsed));
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("", parsed));
    EXPECT_FALSE(DiagnosticEvent::CategoryFromString("SceneLoad", parsed)); // exact token only
}
