// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpEditorLivenessTest — unit test (headless, no GL, no window, no live editor).
//
// Pins the liveness predicate behind the #607 "a minimized editor silently
// swallows injection and serves stale screenshots" gap.
//
// What made that bug expensive was not that it happened, but that NOTHING said
// so. While the window is iconified, Application::Run skips its whole
// `if (!m_Minimized)` block, so EditorLayer::OnUpdate never runs: no frame
// counter advance, no DrainMcpInputQueue, no redraw. MarshalRead keeps working
// (game-thread tasks are pumped BEFORE that guard), so every read tool answers
// with HTTP 200 and a perfectly plausible payload. Two screenshots either side of
// a 10 m walk came back byte-identical, which reads as "the camera didn't move" —
// the exact opposite of the truth.
//
// So the predicate itself is the safety-critical part, and the cases below are
// the ones a careless implementation gets wrong:
//
//   * an UNAVAILABLE signal must never be reported as a stall (a headless host
//     answers nothing, and refusing every call there would be a regression);
//   * a freshly launched editor (no frame stamped yet) must not read as stalled;
//   * "iconified" must dominate the timing test, because the loop can be parked
//     microseconds after its last frame;
//   * the stall REASON must be non-empty exactly when ticking is false, since
//     that string is the whole remediation an agent gets.
// =============================================================================

#include "MCP/McpEditorLiveness.h"

#include <limits>
#include <string>

namespace
{
    using OloEngine::MCP::McpEditorLiveness;
    namespace Liveness = OloEngine::MCP::EditorLiveness;

    // A healthy editor: available, running frames, window up.
    McpEditorLiveness Healthy()
    {
        McpEditorLiveness liveness;
        liveness.Available = true;
        liveness.FrameIndex = 4242;
        liveness.MsSinceLastFrame = 16.7; // ~60 fps
        liveness.Iconified = false;
        liveness.Focused = true;
        liveness.Visible = true;
        return liveness;
    }
} // namespace

TEST(McpEditorLiveness, HealthyEditorIsTickingAndNotStale)
{
    const McpEditorLiveness liveness = Healthy();
    EXPECT_TRUE(Liveness::IsTicking(liveness));
    EXPECT_FALSE(Liveness::IsStale(liveness));
    EXPECT_TRUE(Liveness::StallReason(liveness).empty());
}

TEST(McpEditorLiveness, UnfocusedEditorStillCounts)
{
    // Injection feeds the editor's OWN GLFW/ImGui stream, never the OS input
    // queue, so focus is irrelevant to whether a call can work — and treating an
    // unfocused editor as broken would refuse every legitimate background-driven
    // session, which is the normal way an agent drives it.
    McpEditorLiveness liveness = Healthy();
    liveness.Focused = false;
    EXPECT_TRUE(Liveness::IsTicking(liveness));
    EXPECT_FALSE(Liveness::IsStale(liveness));
}

TEST(McpEditorLiveness, IconifiedIsNotTickingEvenWithAFreshFrame)
{
    // The window can be minimized microseconds after completing a frame, so the
    // timing test alone would call this healthy for most of a second. The
    // iconified flag has to dominate.
    McpEditorLiveness liveness = Healthy();
    liveness.Iconified = true;
    liveness.MsSinceLastFrame = 0.5;

    EXPECT_FALSE(Liveness::IsTicking(liveness));
    EXPECT_TRUE(Liveness::IsStale(liveness));
    EXPECT_NE(Liveness::StallReason(liveness).find("MINIMIZED"), std::string::npos)
        << "the reason must name the minimized window — it is the entire remediation";
}

TEST(McpEditorLiveness, LongGapWithoutAFrameIsAStall)
{
    McpEditorLiveness liveness = Healthy();
    liveness.MsSinceLastFrame = Liveness::s_StallThresholdMs + 1.0;

    EXPECT_FALSE(Liveness::IsTicking(liveness));
    EXPECT_TRUE(Liveness::IsStale(liveness));
    EXPECT_FALSE(Liveness::StallReason(liveness).empty());
}

TEST(McpEditorLiveness, SlowButRunningEditorIsNotAStall)
{
    // A heavily loaded editor at ~2 fps is slow, not parked. The threshold must
    // sit above any plausible real frame time, or every capture on a busy scene
    // would carry a false STALE FRAME banner and train the reader to ignore it.
    McpEditorLiveness liveness = Healthy();
    liveness.MsSinceLastFrame = Liveness::s_StallThresholdMs - 1.0;

    EXPECT_TRUE(Liveness::IsTicking(liveness));
    EXPECT_FALSE(Liveness::IsStale(liveness));
}

TEST(McpEditorLiveness, UnavailableSignalIsUnknownNotStale)
{
    // A headless host wires no liveness hook. "We cannot tell" must not be
    // reported as "the editor is broken": IsStale gates the loud refusals, and a
    // false positive there would disable input injection in every such build.
    const McpEditorLiveness liveness; // Available == false
    EXPECT_FALSE(Liveness::IsTicking(liveness));
    EXPECT_FALSE(Liveness::IsStale(liveness));
    EXPECT_TRUE(Liveness::StallReason(liveness).empty());
    EXPECT_TRUE(Liveness::ToJson(liveness).is_null()) << "null distinguishes UNKNOWN from a reported stall";
}

TEST(McpEditorLiveness, FirstFrameHasNoTimestampAndIsNotAStall)
{
    // EditorLayer reports 0 ms until its first OnUpdate has stamped the clock. A
    // literal reading of "no frame yet" would refuse every call during startup.
    McpEditorLiveness liveness = Healthy();
    liveness.FrameIndex = 0;
    liveness.MsSinceLastFrame = 0.0;
    EXPECT_TRUE(Liveness::IsTicking(liveness));
}

TEST(McpEditorLiveness, NonFiniteGapDoesNotInventAStall)
{
    McpEditorLiveness liveness = Healthy();
    liveness.MsSinceLastFrame = std::numeric_limits<f64>::quiet_NaN();
    EXPECT_TRUE(Liveness::IsTicking(liveness))
        << "an unusable clock reading is not evidence of a stall (a NaN comparison is false either way, "
           "so this must be handled explicitly rather than falling out of the comparison)";
}

TEST(McpEditorLiveness, JsonCarriesEveryFieldAndTheReasonOnlyWhenStalled)
{
    const McpEditorLiveness healthy = Healthy();
    const auto healthyJson = Liveness::ToJson(healthy);
    ASSERT_TRUE(healthyJson.is_object());
    EXPECT_TRUE(healthyJson.at("ticking").get<bool>());
    EXPECT_EQ(healthyJson.at("frameIndex").get<u64>(), 4242u);
    EXPECT_TRUE(healthyJson.at("focused").get<bool>());
    EXPECT_FALSE(healthyJson.at("iconified").get<bool>());
    EXPECT_FALSE(healthyJson.contains("stallReason")) << "a healthy editor must not carry a reason";

    McpEditorLiveness stalled = healthy;
    stalled.Iconified = true;
    const auto stalledJson = Liveness::ToJson(stalled);
    EXPECT_FALSE(stalledJson.at("ticking").get<bool>());
    ASSERT_TRUE(stalledJson.contains("stallReason"));
    EXPECT_FALSE(stalledJson.at("stallReason").get<std::string>().empty());
}

TEST(McpEditorLiveness, SchemaDescribesTheReportedShape)
{
    // The outputSchema is what a schema-eager client validates against, so a field
    // present in ToJson but absent here is a protocol-level lie.
    const auto schema = Liveness::SchemaNode().ToJson();
    ASSERT_TRUE(schema.contains("properties"));
    const auto& properties = schema.at("properties");
    for (const char* field : { "ticking", "frameIndex", "msSinceLastFrame", "iconified", "focused", "visible",
                               "captureUnready", "stallReason" })
    {
        EXPECT_TRUE(properties.contains(field)) << "outputSchema is missing '" << field << "'";
    }
}
