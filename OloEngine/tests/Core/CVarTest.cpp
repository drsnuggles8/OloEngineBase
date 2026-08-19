// =============================================================================
// CVarTest.cpp
//
// Guards Core/CVar.{h,cpp} — the console-variable layer over the debug-lever
// registry (issue #821).
//
// Four things are worth a test here, and only one of them is "does the getter
// return what the setter set":
//
//   1. **Parsing at the boundary.** Everything that reaches this registry
//      arrives as text from somewhere untrustworthy — a command line, a console
//      prompt, an MCP call. The interesting cases are the ones a lenient parser
//      gets WRONG: "tru" must not read as true, "8x" must not read as 8, "inf"
//      must not reach a consumer that multiplies by it, and "unset" must stay
//      distinguishable from 0.
//   2. **`--set` argument handling**, including the malformed shapes: no '=',
//      empty name, a trailing `--set` with nothing after it.
//   3. **The change notification firing exactly once per change** — the reason
//      the issue exists. A callback that fires twice makes a subsystem rebuild a
//      GPU resource twice; one that fires zero times is the bug this replaces.
//      Also the coalescing and no-op-write cases, which are the same contract
//      seen from the other side.
//   4. **The levers are all reachable by name**, with the same rendering
//      `Levers::Snapshot()` uses — because the console and `olo_debug_levers`
//      are read side by side and disagreeing would be worse than either alone.
//
// Every case restores what it changed: the levers are process-global and other
// suites read them.
// =============================================================================

// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"

#include "OloEngine/Core/CVar.h"
#include "OloEngine/Core/DebugLevers.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Puts a lever back however this test found it. The suite shares one
        // process with everything else that reads a lever.
        class ToggleGuard
        {
          public:
            explicit ToggleGuard(bool (*get)(), void (*set)(bool)) : m_Set(set), m_Saved(get()) {}
            ~ToggleGuard()
            {
                m_Set(m_Saved);
            }
            ToggleGuard(const ToggleGuard&) = delete;
            ToggleGuard& operator=(const ToggleGuard&) = delete;
            ToggleGuard(ToggleGuard&&) = delete;
            ToggleGuard& operator=(ToggleGuard&&) = delete;

          private:
            void (*m_Set)(bool);
            bool m_Saved;
        };

        // The registry does not fire callbacks at the write; it fires them when
        // the frame loop drains. A test has no frame loop, so it drains itself.
        void PumpFrame()
        {
            CVars::DispatchPendingChanges();
        }
    } // namespace

    // -------------------------------------------------------------------
    // 1. Parsing at the boundary
    // -------------------------------------------------------------------

    TEST(CVarParse, BooleanTextAcceptsTheObviousFormsAndRejectsEverythingElse)
    {
        for (const std::string_view on : { "1", "true", "TRUE", "True", "yes", "on", "  on  " })
        {
            const std::optional<bool> parsed = CVars::ParseBoolText(on);
            ASSERT_TRUE(parsed.has_value()) << "'" << on << "' should parse";
            EXPECT_TRUE(*parsed) << "'" << on << "'";
        }
        for (const std::string_view off : { "0", "false", "FALSE", "no", "off" })
        {
            const std::optional<bool> parsed = CVars::ParseBoolText(off);
            ASSERT_TRUE(parsed.has_value()) << "'" << off << "' should parse";
            EXPECT_FALSE(*parsed) << "'" << off << "'";
        }

        // The point of being stricter than Env::IsTruthy: at a prompt a typo
        // must come back as an error, not silently read as ON because it does
        // not start with '0' or 'f'.
        for (const std::string_view junk : { "tru", "onn", "2", "", "  ", "yes please" })
        {
            EXPECT_FALSE(CVars::ParseBoolText(junk).has_value()) << "'" << junk << "' must not parse as a boolean";
        }
    }

    TEST(CVarParse, IntegerTextRejectsTrailingGarbageRatherThanTruncating)
    {
        EXPECT_EQ(CVars::ParseIntText("8"), std::optional<i64>{ 8 });
        EXPECT_EQ(CVars::ParseIntText(" -3 "), std::optional<i64>{ -3 });
        EXPECT_EQ(CVars::ParseIntText("0"), std::optional<i64>{ 0 });

        // std::atoi would answer 8, 0, 0 here. That silent truncation is the
        // exact failure the lever registry removed from the engine.
        EXPECT_FALSE(CVars::ParseIntText("8x").has_value());
        EXPECT_FALSE(CVars::ParseIntText("x8").has_value());
        EXPECT_FALSE(CVars::ParseIntText("8 9").has_value());
        EXPECT_FALSE(CVars::ParseIntText("").has_value());
        EXPECT_FALSE(CVars::ParseIntText("1e3").has_value());
    }

    TEST(CVarParse, UnsetTextIsItsOwnValue)
    {
        EXPECT_TRUE(CVars::IsUnsetText("unset"));
        EXPECT_TRUE(CVars::IsUnsetText("UNSET"));
        EXPECT_TRUE(CVars::IsUnsetText("none"));
        EXPECT_TRUE(CVars::IsUnsetText(""));
        EXPECT_TRUE(CVars::IsUnsetText("   "));
        EXPECT_FALSE(CVars::IsUnsetText("0"));
        EXPECT_FALSE(CVars::IsUnsetText("off"));
    }

    // -------------------------------------------------------------------
    // 2. Name-based get/set
    // -------------------------------------------------------------------

    TEST(CVarRegistry, SetsAToggleByNameAndReportsTheMove)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);

        const CVars::SetResult result = CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "on");
        EXPECT_TRUE(result.Ok) << result.Error;
        EXPECT_TRUE(result.Changed);
        EXPECT_EQ(result.OldValue, "off");
        EXPECT_EQ(result.NewValue, "on");
        EXPECT_TRUE(Levers::BlackSquareHunt()) << "the name-based write must reach the typed accessor";

        // Setting it to what it already is is not an error, but it is not a
        // change either — that distinction is what stops a callback firing.
        const CVars::SetResult again = CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "true");
        EXPECT_TRUE(again.Ok);
        EXPECT_FALSE(again.Changed);
    }

    TEST(CVarRegistry, NameLookupIsCaseInsensitive)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);

        EXPECT_TRUE(CVars::SetFromString("olo_rg_blacksquare_hunt", "on").Ok);
        EXPECT_TRUE(Levers::BlackSquareHunt());
        ASSERT_TRUE(CVars::Find("Olo_Rg_BlackSquare_Hunt").has_value());
    }

    TEST(CVarRegistry, UnknownNameFailsAndSuggests)
    {
        const CVars::SetResult result = CVars::SetFromString("OLO_POISON", "on");
        EXPECT_FALSE(result.Ok);
        EXPECT_NE(result.Error.find("unknown console variable"), std::string::npos);
        // The suggestion is the difference between a dead end and a next step.
        EXPECT_NE(result.Error.find("OLO_RG_POISON_TRANSIENTS"), std::string::npos) << result.Error;
    }

    TEST(CVarRegistry, BadValuesAreRefusedWithAReasonAndLeaveTheValueAlone)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);

        const CVars::SetResult result = CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "tru");
        EXPECT_FALSE(result.Ok);
        EXPECT_FALSE(result.Error.empty());
        EXPECT_FALSE(Levers::BlackSquareHunt()) << "a rejected parse must not half-apply";
    }

    TEST(CVarRegistry, TristateDistinguishesUnsetFromOffThroughTheNameBasedPath)
    {
        const Levers::Tristate saved = Levers::TaskGraphDynamicPrioritization();

        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_DYNAMIC_PRIORITIZATION", "off").Ok);
        EXPECT_EQ(Levers::TaskGraphDynamicPrioritization(), Levers::Tristate::Off);

        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_DYNAMIC_PRIORITIZATION", "unset").Ok);
        EXPECT_EQ(Levers::TaskGraphDynamicPrioritization(), Levers::Tristate::Unset)
            << "'unset' means 'leave the hardware-derived default alone' and must not collapse to Off";

        Levers::SetTaskGraphDynamicPrioritization(saved);
    }

    TEST(CVarRegistry, NumericLeversKeepTheirBoundsAndTheirUnsetState)
    {
        const std::optional<f32> savedRatio = Levers::TaskGraphOversubscriptionRatio();
        const std::optional<i64> savedWorkers = Levers::TaskGraphNumWorkers();

        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", "2.5").Ok);
        ASSERT_TRUE(Levers::TaskGraphOversubscriptionRatio().has_value());
        EXPECT_FLOAT_EQ(*Levers::TaskGraphOversubscriptionRatio(), 2.5f);

        // Non-finite must die HERE. Downstream, inf passes a `>= 1.0f` guard and
        // the resulting ceil(workers * inf) cast to i32 is undefined behaviour —
        // the same reason the environment seed rejects it.
        for (const std::string_view bad : { "inf", "INF", "nan", "0.5", "1000", "2.0abc" })
        {
            const CVars::SetResult result = CVars::SetFromString("OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", bad);
            EXPECT_FALSE(result.Ok) << "'" << bad << "' was accepted";
        }
        EXPECT_FLOAT_EQ(*Levers::TaskGraphOversubscriptionRatio(), 2.5f) << "a refused write must not disturb the value";

        // Surrounding whitespace is accepted for EVERY kind. It reaches the
        // registry untrimmed from olo_cvar_set (a raw JSON string), and the
        // number path goes through ParseNumberLever, which deliberately rejects
        // trailing space for the ENVIRONMENT — so without an explicit trim here
        // "2.5 " came back as a range error that named a range it was inside.
        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", " 3.5 ").Ok);
        ASSERT_TRUE(Levers::TaskGraphOversubscriptionRatio().has_value());
        EXPECT_FLOAT_EQ(*Levers::TaskGraphOversubscriptionRatio(), 3.5f);

        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", "unset").Ok);
        EXPECT_FALSE(Levers::TaskGraphOversubscriptionRatio().has_value());

        // Below the row's declared minimum, which for this one is 1.
        EXPECT_FALSE(CVars::SetFromString("OLO_TASK_GRAPH_NUM_WORKERS", "0").Ok);
        EXPECT_TRUE(CVars::SetFromString("OLO_TASK_GRAPH_NUM_WORKERS", "2").Ok);
        ASSERT_TRUE(Levers::TaskGraphNumWorkers().has_value());
        EXPECT_EQ(*Levers::TaskGraphNumWorkers(), 2);

        Levers::SetTaskGraphOversubscriptionRatio(savedRatio);
        Levers::SetTaskGraphNumWorkers(savedWorkers);
    }

    TEST(CVarRegistry, TextLeversAreReadOnlyAndSayWhy)
    {
        const CVars::SetResult result = CVars::SetFromString("OLO_PHYSICS_CACHE_DIR", "C:/tmp");
        EXPECT_FALSE(result.Ok);
        // Accepting it and then ignoring it is strictly worse than refusing: the
        // caller would believe the write took.
        EXPECT_NE(result.Error.find("read-only"), std::string::npos) << result.Error;
    }

    TEST(CVarRegistry, CompletionExtendsAsFarAsItIsUnambiguous)
    {
        const std::vector<std::string_view> rg = CVars::Complete("OLO_RG_");
        ASSERT_GE(rg.size(), 3u) << "the render-graph levers should all match";
        for (const std::string_view name : rg)
        {
            EXPECT_TRUE(name.starts_with("OLO_RG_"));
        }

        // Case-insensitive, and the common prefix stops where the names diverge.
        EXPECT_FALSE(CVars::Complete("olo_rg_").empty());
        EXPECT_EQ(CVars::LongestCompletion("OLO_RG_POIS"), "OLO_RG_POISON_TRANSIENTS");
        EXPECT_TRUE(CVars::Complete("OLO_NOT_A_LEVER").empty());
        EXPECT_EQ(CVars::LongestCompletion("OLO_NOT_A_LEVER"), "OLO_NOT_A_LEVER");
    }

    // -------------------------------------------------------------------
    // 3. `--set`
    // -------------------------------------------------------------------

    TEST(CVarCommandLine, ParseAssignmentHandlesTheMalformedShapes)
    {
        const auto ok = CVars::ParseAssignment("NAME=value");
        ASSERT_TRUE(ok.has_value());
        EXPECT_EQ(ok->first, "NAME");
        EXPECT_EQ(ok->second, "value");

        // A path or any value containing '=' must survive: split on the FIRST
        // one only.
        const auto multi = CVars::ParseAssignment("NAME=a=b");
        ASSERT_TRUE(multi.has_value());
        EXPECT_EQ(multi->second, "a=b");

        // An empty value is legal — it is how you clear an optional lever.
        const auto empty = CVars::ParseAssignment("NAME=");
        ASSERT_TRUE(empty.has_value());
        EXPECT_TRUE(empty->second.empty());

        EXPECT_FALSE(CVars::ParseAssignment("NAME").has_value()) << "no '=' is not an assignment";
        EXPECT_FALSE(CVars::ParseAssignment("=value").has_value()) << "an empty name is not an assignment";
        EXPECT_FALSE(CVars::ParseAssignment("").has_value());
    }

    TEST(CVarCommandLine, AppliesBothSpellingsAndReportsWhatItRefused)
    {
        const ToggleGuard hunt(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        const ToggleGuard diag(&Levers::RenderGraphDiagnostics, &Levers::SetRenderGraphDiagnostics);
        Levers::SetBlackSquareHunt(false);
        Levers::SetRenderGraphDiagnostics(false);

        // argv is char*, and ApplyCommandLine takes it as the real main() does.
        std::array<std::string, 8> storage{ "OloEditor.exe",
                                            "--set",
                                            "OLO_RG_BLACKSQUARE_HUNT=on",
                                            "--set=OLO_RENDERGRAPH_DIAGNOSTICS=on",
                                            "--some-other-flag",
                                            "--set",
                                            "OLO_NOT_A_LEVER=on",
                                            "--set=malformed" };
        std::array<char*, 8> argv{};
        for (sizet i = 0; i < storage.size(); ++i)
        {
            argv[i] = storage[i].data();
        }

        const CVars::CommandLineResult result = CVars::ApplyCommandLine(static_cast<int>(argv.size()), argv.data());

        EXPECT_EQ(result.Applied, 2u);
        EXPECT_TRUE(Levers::BlackSquareHunt());
        EXPECT_TRUE(Levers::RenderGraphDiagnostics());

        // A typo must be reported, not swallowed — a silently ignored --set is
        // indistinguishable from a lever that does not work.
        ASSERT_EQ(result.Errors.size(), 2u);
        EXPECT_NE(result.Errors[0].find("OLO_NOT_A_LEVER"), std::string::npos);
        EXPECT_NE(result.Errors[1].find("NAME=VALUE"), std::string::npos);
    }

    TEST(CVarCommandLine, TrailingSetWithNoArgumentIsReportedNotRead)
    {
        std::array<std::string, 2> storage{ "OloServer.exe", "--set" };
        std::array<char*, 2> argv{};
        for (sizet i = 0; i < storage.size(); ++i)
        {
            argv[i] = storage[i].data();
        }

        const CVars::CommandLineResult result = CVars::ApplyCommandLine(static_cast<int>(argv.size()), argv.data());
        EXPECT_EQ(result.Applied, 0u);
        ASSERT_EQ(result.Errors.size(), 1u);
        EXPECT_NE(result.Errors[0].find("--set needs an argument"), std::string::npos);
    }

    // -------------------------------------------------------------------
    // 4. The change notification — the reason this issue exists
    // -------------------------------------------------------------------

    TEST(CVarChangeNotification, FiresExactlyOncePerChange)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);
        PumpFrame(); // absorb the guard's own write

        u32 calls = 0;
        std::string lastSeen;
        const CVars::CallbackHandle handle =
            CVars::AddChangeCallback("OLO_RG_BLACKSQUARE_HUNT", [&](const CVars::CVarInfo& info)
                                     {
                                         ++calls;
                                         lastSeen = info.Value; },
                                     /*invokeNow*/ false);
        ASSERT_TRUE(handle.IsValid());

        EXPECT_EQ(calls, 0u) << "a write is not a notification — the frame drain is";

        EXPECT_TRUE(CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "on").Ok);
        EXPECT_EQ(calls, 0u) << "still nothing until the frame drains";

        PumpFrame();
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(lastSeen, "on");

        // A second drain with nothing pending must not re-fire; a subsystem's
        // reaction is usually expensive.
        PumpFrame();
        EXPECT_EQ(calls, 1u);

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
    }

    TEST(CVarChangeNotification, ANoOpWriteAndACancellingPairNotifyNobody)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);
        PumpFrame();

        u32 calls = 0;
        const CVars::CallbackHandle handle = CVars::AddChangeCallback(
            "OLO_RG_BLACKSQUARE_HUNT", [&](const CVars::CVarInfo&)
            { ++calls; }, /*invokeNow*/ false);
        ASSERT_TRUE(handle.IsValid());

        // Set to the value it already had.
        EXPECT_TRUE(CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "off").Ok);
        PumpFrame();
        EXPECT_EQ(calls, 0u);

        // On then off within one frame: the callback's contract is "apply the
        // current value", and the current value never moved.
        Levers::SetBlackSquareHunt(true);
        Levers::SetBlackSquareHunt(false);
        PumpFrame();
        EXPECT_EQ(calls, 0u);

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
    }

    TEST(CVarChangeNotification, SeveralChangesInOneFrameCoalesceIntoOneCall)
    {
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);
        PumpFrame();

        u32 calls = 0;
        const CVars::CallbackHandle handle = CVars::AddChangeCallback(
            "OLO_RG_BLACKSQUARE_HUNT", [&](const CVars::CVarInfo&)
            { ++calls; }, /*invokeNow*/ false);
        ASSERT_TRUE(handle.IsValid());

        Levers::SetBlackSquareHunt(true);
        Levers::SetBlackSquareHunt(false);
        Levers::SetBlackSquareHunt(true);
        PumpFrame();
        EXPECT_EQ(calls, 1u) << "the observer applies the CURRENT value, so intermediate states are not events";

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
    }

    TEST(CVarChangeNotification, RegisteringAfterTheValueChangedStillSynchronises)
    {
        // The ordering question the issue calls out: a subsystem that comes up
        // AFTER a `--set` must not be left holding the old value. invokeNow is
        // what makes that a non-issue, and it is why callbacks are written as
        // "apply the current value" rather than "handle a delta".
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);
        PumpFrame();
        EXPECT_TRUE(CVars::SetFromString("OLO_RG_BLACKSQUARE_HUNT", "on").Ok);
        PumpFrame();

        std::string seen;
        u32 calls = 0;
        const CVars::CallbackHandle handle = CVars::AddChangeCallback("OLO_RG_BLACKSQUARE_HUNT",
                                                                      [&](const CVars::CVarInfo& info)
                                                                      {
                                                                          ++calls;
                                                                          seen = info.Value;
                                                                      });
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(calls, 1u) << "registration itself must deliver the current value";
        EXPECT_EQ(seen, "on");

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
    }

    TEST(CVarChangeNotification, ATypedSetterNotifiesJustLikeANameBasedOne)
    {
        // RenderGraph::SetTransientDebugFlags and every other caller writes
        // through the generated lever setter, not through SetFromString. If only
        // the name-based path notified, the aliasing pool eviction would still
        // be a special case — which is the bug this generalises away.
        const ToggleGuard guard(&Levers::DisableTransientAliasing, &Levers::SetDisableTransientAliasing);
        Levers::SetDisableTransientAliasing(false);
        PumpFrame();

        u32 calls = 0;
        const CVars::CallbackHandle handle = CVars::AddChangeCallback(
            "OLO_RG_DISABLE_ALIASING", [&](const CVars::CVarInfo&)
            { ++calls; }, /*invokeNow*/ false);
        ASSERT_TRUE(handle.IsValid());

        Levers::SetDisableTransientAliasing(true);
        PumpFrame();
        EXPECT_EQ(calls, 1u);

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
    }

    TEST(CVarChangeNotification, ASmallFloatChangeStillNotifies)
    {
        // The change test compares RENDERED values, so the rendering has to
        // round-trip. With std::to_string's fixed six decimals these two floats
        // render identically, the registry concludes "nothing moved", and an
        // observer keeps a stale value while the typed accessor already returns
        // the new one — a silent failure of the whole mechanism, in exactly the
        // case a human would never spot by eye.
        const std::optional<f32> saved = Levers::TaskGraphOversubscriptionRatio();

        Levers::SetTaskGraphOversubscriptionRatio(1.0f);
        PumpFrame();

        u32 calls = 0;
        const CVars::CallbackHandle handle = CVars::AddChangeCallback(
            "OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", [&](const CVars::CVarInfo&)
            { ++calls; }, /*invokeNow*/ false);
        ASSERT_TRUE(handle.IsValid());

        Levers::SetTaskGraphOversubscriptionRatio(1.0000004f);
        PumpFrame();
        EXPECT_EQ(calls, 1u) << "a real change below six decimal places must still notify";

        EXPECT_TRUE(CVars::RemoveChangeCallback(handle));
        Levers::SetTaskGraphOversubscriptionRatio(saved);
    }

    TEST(CVarChangeNotification, InvokeNowIsAnExtraCallOutsideTheOncePerChangeAccounting)
    {
        // Documented, not accidental: registering while a change is already
        // pending calls the NEW callback twice (immediately, then at dispatch).
        // Advancing the shared last-notified value at registration instead would
        // swallow the pending notification for every callback already on that
        // cvar — trading a harmless repeat for a lost update. This test exists
        // so the trade stays a decision rather than a surprise.
        const ToggleGuard guard(&Levers::BlackSquareHunt, &Levers::SetBlackSquareHunt);
        Levers::SetBlackSquareHunt(false);
        PumpFrame();

        u32 early = 0;
        const CVars::CallbackHandle first = CVars::AddChangeCallback(
            "OLO_RG_BLACKSQUARE_HUNT", [&](const CVars::CVarInfo&)
            { ++early; }, /*invokeNow*/ false);
        ASSERT_TRUE(first.IsValid());

        Levers::SetBlackSquareHunt(true); // marked, not yet dispatched

        u32 late = 0;
        const CVars::CallbackHandle second = CVars::AddChangeCallback(
            "OLO_RG_BLACKSQUARE_HUNT", [&](const CVars::CVarInfo&)
            { ++late; }, /*invokeNow*/ true);
        ASSERT_TRUE(second.IsValid());
        EXPECT_EQ(late, 1u) << "invokeNow delivers immediately";

        PumpFrame();
        EXPECT_EQ(early, 1u) << "the already-registered callback must NOT lose the pending change";
        EXPECT_EQ(late, 2u) << "the late registrant sees it twice — harmless, because callbacks are idempotent";

        EXPECT_TRUE(CVars::RemoveChangeCallback(first));
        EXPECT_TRUE(CVars::RemoveChangeCallback(second));
    }

    TEST(CVarChangeNotification, AnUnknownNameRegistersNothing)
    {
        const CVars::CallbackHandle handle =
            CVars::AddChangeCallback("OLO_NOT_A_LEVER", [](const CVars::CVarInfo&) {});
        EXPECT_FALSE(handle.IsValid());
        EXPECT_FALSE(CVars::RemoveChangeCallback(handle));
    }

    // -------------------------------------------------------------------
    // 5. Typed CVar<T>
    // -------------------------------------------------------------------

    // Namespace-scope, because that is the intended lifetime: the registry keeps
    // a pointer and nothing is ever unregistered.
    namespace
    {
        CVars::CVar<bool> s_TestBool{ "OLO_TEST_CVAR_BOOL", false, "A bool cvar that exists only for CVarTest." };
        CVars::CVar<i64> s_TestInt{ "OLO_TEST_CVAR_INT", 7, "An int cvar that exists only for CVarTest." };
        CVars::CVar<f32> s_TestFloat{ "OLO_TEST_CVAR_FLOAT", 1.5f, "A float cvar that exists only for CVarTest." };
        CVars::CVar<std::string> s_TestString{ "OLO_TEST_CVAR_STRING", "hello",
                                               "A string cvar that exists only for CVarTest." };
    } // namespace

    TEST(TypedCVar, RoundTripsThroughBothTheTypedAndTheNameBasedSide)
    {
        EXPECT_EQ(s_TestBool.Get(), false);
        EXPECT_EQ(s_TestInt.Get(), 7);
        EXPECT_FLOAT_EQ(s_TestFloat.Get(), 1.5f);
        EXPECT_EQ(s_TestString.Get(), "hello");

        EXPECT_TRUE(CVars::SetFromString("OLO_TEST_CVAR_BOOL", "on").Ok);
        EXPECT_TRUE(s_TestBool.Get());

        EXPECT_TRUE(CVars::SetFromString("OLO_TEST_CVAR_INT", "-12").Ok);
        EXPECT_EQ(s_TestInt.Get(), -12);

        EXPECT_TRUE(CVars::SetFromString("OLO_TEST_CVAR_FLOAT", "0.25").Ok);
        EXPECT_FLOAT_EQ(s_TestFloat.Get(), 0.25f);

        EXPECT_TRUE(CVars::SetFromString("OLO_TEST_CVAR_STRING", "world").Ok);
        EXPECT_EQ(s_TestString.Get(), "world");

        // A typed Set is visible to the registry, and IsDefault tracks the
        // constructor default rather than "has anyone written it".
        s_TestInt.Set(7);
        const std::optional<CVars::CVarInfo> info = CVars::Find("OLO_TEST_CVAR_INT");
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->Value, "7");
        EXPECT_TRUE(info->IsDefault);

        // Restore, so a later test reading Snapshot() sees a clean table.
        s_TestBool.Set(false);
        s_TestFloat.Set(1.5f);
        s_TestString.Set("hello");
    }

    TEST(TypedCVar, RejectsNonFiniteAndMalformedValues)
    {
        const f32 saved = s_TestFloat.Get();
        for (const std::string_view bad : { "inf", "-inf", "nan", "1.0abc", "" })
        {
            EXPECT_FALSE(CVars::SetFromString("OLO_TEST_CVAR_FLOAT", bad).Ok) << "'" << bad << "' was accepted";
        }
        EXPECT_FLOAT_EQ(s_TestFloat.Get(), saved);
        EXPECT_FALSE(CVars::SetFromString("OLO_TEST_CVAR_INT", "1.5").Ok);
    }

    // -------------------------------------------------------------------
    // 6. The levers and the cvar registry stay one list
    // -------------------------------------------------------------------

    TEST(CVarRegistry, EveryLeverIsReachableByNameAndRendersIdentically)
    {
        // Two enumerations of the same table are read side by side —
        // olo_debug_levers and the console's `list`. Disagreeing about a value
        // would be worse than either being absent, because both look right.
        const std::vector<CVars::CVarInfo> cvars = CVars::Snapshot();
        ASSERT_FALSE(cvars.empty());

        for (const Levers::LeverInfo& lever : Levers::Snapshot())
        {
            const std::optional<CVars::CVarInfo> found = CVars::Find(lever.Name);
            ASSERT_TRUE(found.has_value()) << "lever '" << lever.Name << "' is not reachable by name";
            EXPECT_EQ(found->Value, lever.Value) << "'" << lever.Name << "' renders differently in the two tables";
            EXPECT_EQ(found->IsDefault, lever.IsDefault) << "'" << lever.Name << "'";
            EXPECT_EQ(found->Help, lever.Help) << "'" << lever.Name << "'";
        }
    }

    TEST(CVarRegistry, NamesAreUniqueAndValuesAreNeverBlank)
    {
        std::set<std::string> names;
        for (const CVars::CVarInfo& info : CVars::Snapshot())
        {
            EXPECT_FALSE(info.Name.empty());
            // An empty rendered value would break the change comparison as well
            // as the display: the registry decides "did it move?" on this string.
            EXPECT_FALSE(info.Value.empty()) << "'" << info.Name << "' renders as an empty string";
            EXPECT_TRUE(names.insert(std::string(info.Name)).second) << "duplicate cvar '" << info.Name << "'";
        }
    }
} // namespace OloEngine::Tests
