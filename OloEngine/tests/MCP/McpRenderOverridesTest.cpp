#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure name tables + result-shaping behind the ephemeral
// render-override tools olo_render_toggle_pass / olo_render_set_debug_view (issue
// #316 Part 4 — the rendering A/B harness). The token<->field mapping and the
// response JSON schema live in a header-only free-function module with no renderer
// / editor / GPU dependencies, precisely so they can be exercised here without a
// live editor or GL context — the same split McpShaderReload.h / McpRenderExplain.h
// use (the MCP test binary compiles the dispatch core but deliberately NOT
// McpTools.cpp, the editor-backed handler). The live flip is verified separately
// over the MCP attach loop; this pins the name resolution (canonical + aliases) and
// the response shapes the agent reads.
#include "MCP/McpRenderOverrides.h"

#include "OloEngine/Atmosphere/Ephemeris.h"

#include <cmath>
#include <limits>
#include <string>

namespace
{
    namespace RO = OloEngine::MCP::RenderOverrides;
    using Json = nlohmann::json;
} // namespace

// ---- Pass name resolution ------------------------------------------------

TEST(McpRenderOverrides, ParsesEveryCanonicalPassToken)
{
    for (const auto& info : RO::kPasses)
    {
        RO::Pass pass{};
        EXPECT_TRUE(RO::ParsePass(info.Token, pass)) << "canonical token: " << info.Token;
        EXPECT_EQ(info.Id, pass) << "canonical token: " << info.Token;
        // PassToken round-trips back to the canonical spelling.
        EXPECT_EQ(std::string(info.Token), std::string(RO::PassToken(info.Id)));
    }
}

TEST(McpRenderOverrides, ParsePassIsCaseAndSeparatorInsensitive)
{
    RO::Pass pass{};
    EXPECT_TRUE(RO::ParsePass("Bloom", pass));
    EXPECT_EQ(RO::Pass::Bloom, pass);
    EXPECT_TRUE(RO::ParsePass("SSAO", pass));
    EXPECT_EQ(RO::Pass::SSAO, pass);
    EXPECT_TRUE(RO::ParsePass("chromatic aberration", pass));
    EXPECT_EQ(RO::Pass::ChromaticAberration, pass);
    EXPECT_TRUE(RO::ParsePass("color_grading", pass));
    EXPECT_EQ(RO::Pass::ColorGrading, pass);
    EXPECT_TRUE(RO::ParsePass("depth-of-field", pass));
    EXPECT_EQ(RO::Pass::DepthOfField, pass);
}

TEST(McpRenderOverrides, ParsePassResolvesAliases)
{
    RO::Pass pass{};
    EXPECT_TRUE(RO::ParsePass("dof", pass));
    EXPECT_EQ(RO::Pass::DepthOfField, pass);
    EXPECT_TRUE(RO::ParsePass("ca", pass));
    EXPECT_EQ(RO::Pass::ChromaticAberration, pass);
    EXPECT_TRUE(RO::ParsePass("colourgrading", pass));
    EXPECT_EQ(RO::Pass::ColorGrading, pass);
    EXPECT_TRUE(RO::ParsePass("eyeadaptation", pass));
    EXPECT_EQ(RO::Pass::AutoExposure, pass);
    EXPECT_TRUE(RO::ParsePass("scattering", pass));
    EXPECT_EQ(RO::Pass::FogScattering, pass);
    EXPECT_TRUE(RO::ParsePass("volumetric", pass));
    EXPECT_EQ(RO::Pass::FogVolumetric, pass);
    EXPECT_TRUE(RO::ParsePass("lightshafts", pass));
    EXPECT_EQ(RO::Pass::GodRays, pass);
}

TEST(McpRenderOverrides, ParsePassRejectsUnknownAndEmpty)
{
    RO::Pass pass{};
    EXPECT_FALSE(RO::ParsePass("", pass));
    EXPECT_FALSE(RO::ParsePass("   ", pass));
    EXPECT_FALSE(RO::ParsePass("notapass", pass));
    EXPECT_FALSE(RO::ParsePass("ambientocclusion", pass)); // not a token we expose
}

TEST(McpRenderOverrides, PassTokensCoversEveryPassAndDescribePassesMatches)
{
    const auto tokens = RO::PassTokens();
    EXPECT_EQ(RO::kPasses.size(), tokens.size());

    const Json described = RO::DescribePasses();
    ASSERT_TRUE(described.is_array());
    ASSERT_EQ(RO::kPasses.size(), described.size());
    for (std::size_t i = 0; i < RO::kPasses.size(); ++i)
    {
        EXPECT_EQ(std::string(RO::kPasses[i].Token), described[i].at("name").get<std::string>());
        EXPECT_FALSE(described[i].at("description").get<std::string>().empty());
    }
}

// ---- Toggle result JSON --------------------------------------------------

TEST(McpRenderOverrides, ToggleResultJsonShape)
{
    RO::ToggleResult r;
    r.Pass = "bloom";
    r.Previous = false;
    r.Enabled = true;
    r.Changed = true;

    const Json j = RO::ToJson(r);
    EXPECT_EQ("bloom", j.at("pass").get<std::string>());
    EXPECT_TRUE(j.at("enabled").get<bool>());
    EXPECT_FALSE(j.at("previous").get<bool>());
    EXPECT_TRUE(j.at("changed").get<bool>());
    // No note -> the key is omitted, not present-as-empty.
    EXPECT_FALSE(j.contains("note"));
}

TEST(McpRenderOverrides, ToggleResultIncludesNoteWhenSet)
{
    RO::ToggleResult r;
    r.Pass = "ssr";
    r.Previous = false;
    r.Enabled = true;
    r.Changed = true;
    r.Note = "ssr renders only in the Deferred rendering path (current path: Forward).";

    const Json j = RO::ToJson(r);
    ASSERT_TRUE(j.contains("note"));
    EXPECT_NE(std::string::npos, j.at("note").get<std::string>().find("Deferred"));
}

TEST(McpRenderOverrides, ToggleResultUnchangedWhenStateMatches)
{
    RO::ToggleResult r;
    r.Pass = "fxaa";
    r.Previous = true;
    r.Enabled = true;
    r.Changed = false;

    const Json j = RO::ToJson(r);
    EXPECT_TRUE(j.at("enabled").get<bool>());
    EXPECT_FALSE(j.at("changed").get<bool>());
}

// ---- Debug view name resolution ------------------------------------------

TEST(McpRenderOverrides, ParsesEveryDebugViewMode)
{
    for (const auto& info : RO::kDebugViews)
    {
        RO::DebugView view{};
        EXPECT_TRUE(RO::ParseDebugView(info.Token, view)) << "mode: " << info.Token;
        EXPECT_EQ(info.Id, view) << "mode: " << info.Token;
        EXPECT_EQ(std::string(info.Token), std::string(RO::DebugViewToken(info.Id)));
    }
}

TEST(McpRenderOverrides, ParseDebugViewTreatsEmptyAndOffAsNone)
{
    RO::DebugView view = RO::DebugView::SSAO;
    EXPECT_TRUE(RO::ParseDebugView("", view));
    EXPECT_EQ(RO::DebugView::None, view);

    view = RO::DebugView::SSR;
    EXPECT_TRUE(RO::ParseDebugView("off", view));
    EXPECT_EQ(RO::DebugView::None, view);

    view = RO::DebugView::GTAO;
    EXPECT_TRUE(RO::ParseDebugView("OFF", view)); // case insensitive
    EXPECT_EQ(RO::DebugView::None, view);
}

TEST(McpRenderOverrides, ParseDebugViewRejectsUnknown)
{
    RO::DebugView view{};
    EXPECT_FALSE(RO::ParseDebugView("bloom", view)); // a pass, not a debug view
    EXPECT_FALSE(RO::ParseDebugView("normals", view));
}

// ---- Debug view result JSON ----------------------------------------------

TEST(McpRenderOverrides, DebugViewResultJsonShapeActive)
{
    RO::DebugViewResult r;
    r.Mode = "ssao";
    r.SSAODebugView = true;
    r.PassEnabled = true;

    const Json j = RO::ToJson(r);
    EXPECT_EQ("ssao", j.at("mode").get<std::string>());
    EXPECT_TRUE(j.at("ssaoDebugView").get<bool>());
    EXPECT_FALSE(j.at("gtaoDebugView").get<bool>());
    EXPECT_FALSE(j.at("ssrDebugView").get<bool>());
    EXPECT_FALSE(j.at("ssgiDebugView").get<bool>());
    EXPECT_TRUE(j.at("passEnabled").get<bool>());
    EXPECT_FALSE(j.contains("note"));
}

TEST(McpRenderOverrides, DebugViewResultNotePresentWhenPassDisabled)
{
    RO::DebugViewResult r;
    r.Mode = "ssr";
    r.SSRDebugView = true;
    r.PassEnabled = false;
    r.Note = "SSR is not active; enable it with olo_render_toggle_pass { name: 'ssr' } (Deferred path only).";

    const Json j = RO::ToJson(r);
    EXPECT_TRUE(j.at("ssrDebugView").get<bool>());
    EXPECT_FALSE(j.at("passEnabled").get<bool>());
    ASSERT_TRUE(j.contains("note"));
    EXPECT_NE(std::string::npos, j.at("note").get<std::string>().find("not active"));
}

TEST(McpRenderOverrides, DebugViewModesCoversEveryModeAndDescribeMatches)
{
    const auto modes = RO::DebugViewModes();
    EXPECT_EQ(RO::kDebugViews.size(), modes.size());
    EXPECT_EQ("none", modes.front()); // None is first / the default

    const Json described = RO::DescribeDebugViews();
    ASSERT_EQ(RO::kDebugViews.size(), described.size());
    for (std::size_t i = 0; i < RO::kDebugViews.size(); ++i)
        EXPECT_EQ(std::string(RO::kDebugViews[i].Token), described[i].at("name").get<std::string>());
}

// ---- Misc helpers --------------------------------------------------------

TEST(McpRenderOverrides, JoinTokensProducesCommaSeparatedList)
{
    EXPECT_EQ("", RO::JoinTokens({}));
    EXPECT_EQ("a", RO::JoinTokens({ "a" }));
    EXPECT_EQ("a, b, c", RO::JoinTokens({ "a", "b", "c" }));
    // The real error message joins every pass token; sanity-check it mentions one.
    EXPECT_NE(std::string::npos, RO::JoinTokens(RO::PassTokens()).find("bloom"));
}

// ---- Sun-angle -> time-of-day solver ---------------------------------------
// Pure math behind olo_scene_set_sun_angle (issue #633): the ephemeral sun
// override is retired and the serialized TimeOfDayComponent is the single sun
// source, so the tool SOLVES for the clock time whose ephemeris sun best
// matches a requested elevation/azimuth and writes that into the component.
// The live component write is verified over the MCP attach loop; this pins the
// solver — the branch selection (east = morning), the clamp-to-achievable
// behaviour, and agreement with the engine ephemeris the solved hours feed.

namespace
{
    // The engine-side sun state for a solved time (north offset 0, so the
    // solver's frame and the ephemeris frame coincide).
    OloEngine::SunMoonState EphemerisStateFor(float hours, int dayOfYear, float latitudeDeg)
    {
        OloEngine::EphemerisInputs inputs;
        inputs.TimeOfDayHours = hours;
        inputs.DayOfYear = dayOfYear;
        inputs.LatitudeDegrees = latitudeDeg;
        inputs.NorthOffsetDegrees = 0.0f;
        return OloEngine::Ephemeris::ComputeSunMoon(inputs);
    }

    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
} // namespace

TEST(McpRenderOverridesSunSolve, SouthNoonElevationSolvesToNoon)
{
    // Day 80 (~March equinox) at latitude 48 N: the noon maximum elevation is
    // ~90 - 48 + declination(80) ~= 41.5 deg, due south. Asking for exactly
    // that elevation from the south must land on (solar) noon.
    const RO::SunAngleSolve s = RO::SolveTimeForSunAngle(41.5f, 180.0f, 80, 48.0f);
    EXPECT_NEAR(12.0, static_cast<double>(s.Hours), 0.2);
    EXPECT_TRUE(std::isfinite(s.AchievedElevationDeg));
    EXPECT_NEAR(41.5, static_cast<double>(s.AchievedElevationDeg), 0.5);
}

TEST(McpRenderOverridesSunSolve, EastIsMorningWestIsAfternoon)
{
    // Same reachable elevation, opposite sides of the sky: the east request
    // must resolve before noon, the west request after — and the two must
    // mirror around noon (cos is even, only the branch sign differs).
    const RO::SunAngleSolve morning = RO::SolveTimeForSunAngle(20.0f, 90.0f /*east*/, 172, 48.0f);
    const RO::SunAngleSolve afternoon = RO::SolveTimeForSunAngle(20.0f, 270.0f /*west*/, 172, 48.0f);

    EXPECT_FALSE(morning.Clamped);
    EXPECT_FALSE(afternoon.Clamped);
    EXPECT_LT(morning.Hours, 12.0f);
    EXPECT_GT(afternoon.Hours, 12.0f);
    EXPECT_NEAR(24.0, static_cast<double>(morning.Hours + afternoon.Hours), 1e-3);
}

TEST(McpRenderOverridesSunSolve, UnachievableElevationClampsToFiniteNoon)
{
    // Day 355 (~December solstice) at latitude 48 N: the noon maximum is
    // ~90 - 48 - 23.44 ~= 18.6 deg, so 80 deg is far out of range. The solver
    // must clamp to the day's maximum (solar noon) instead of NaN-ing on acos.
    const RO::SunAngleSolve s = RO::SolveTimeForSunAngle(80.0f, 180.0f, 355, 48.0f);
    EXPECT_TRUE(s.Clamped);
    EXPECT_TRUE(std::isfinite(s.Hours));
    EXPECT_TRUE(std::isfinite(s.AchievedElevationDeg));
    EXPECT_NEAR(12.0, static_cast<double>(s.Hours), 0.2); // the max sits at solar noon
    EXPECT_NEAR(18.6, static_cast<double>(s.AchievedElevationDeg), 0.5);
    EXPECT_LT(s.AchievedElevationDeg, 80.0f); // reported honestly, not parroted
}

TEST(McpRenderOverridesSunSolve, SolvedHoursStayInRange)
{
    const RO::SunAngleSolve high = RO::SolveTimeForSunAngle(89.0f, 180.0f, 172, 0.1f);
    EXPECT_GE(high.Hours, 0.0f);
    EXPECT_LT(high.Hours, 24.0f);

    const RO::SunAngleSolve low = RO::SolveTimeForSunAngle(-89.0f, 270.0f, 355, 48.0f);
    EXPECT_GE(low.Hours, 0.0f);
    EXPECT_LT(low.Hours, 24.0f);
}

TEST(McpRenderOverridesSunSolve, DegenerateInputsStayFinite)
{
    // Agent JSON is untrusted: non-finite angles and out-of-range day/latitude
    // must still produce a finite, in-range clock time.
    const RO::SunAngleSolve s = RO::SolveTimeForSunAngle(std::numeric_limits<float>::quiet_NaN(),
                                                         std::numeric_limits<float>::infinity(), 4000, 95.0f);
    EXPECT_TRUE(std::isfinite(s.Hours));
    EXPECT_GE(s.Hours, 0.0f);
    EXPECT_LT(s.Hours, 24.0f);
    EXPECT_TRUE(std::isfinite(s.AchievedElevationDeg));
}

TEST(McpRenderOverridesSunSolve, RoundTripsThroughEngineEphemeris)
{
    struct Case
    {
        float ElevationDeg;
        float AzimuthDeg;
        int DayOfYear;
        float LatitudeDeg;
    };
    // Reachable elevations on both sides of the sky, both hemispheres. Avoid
    // near-noon azimuths (180/0), where the east/west side of the achieved sun
    // is numerically degenerate.
    const Case cases[] = {
        { 20.0f, 90.0f, 172, 48.0f },   // northern summer, east -> morning
        { 10.0f, 135.0f, 100, 35.0f },  // spring, south-east
        { 5.0f, 250.0f, 250, 48.0f },   // late summer, west -> afternoon
        { 30.0f, 300.0f, 200, -33.0f }, // southern winter, west
    };
    for (const Case& c : cases)
    {
        const RO::SunAngleSolve s =
            RO::SolveTimeForSunAngle(c.ElevationDeg, c.AzimuthDeg, c.DayOfYear, c.LatitudeDeg);
        ASSERT_FALSE(s.Clamped) << "case elev=" << c.ElevationDeg << " az=" << c.AzimuthDeg;

        // Feeding the solved hours back into the ENGINE ephemeris (the exact
        // math TimeOfDaySystem::Apply runs on the written component) must
        // reproduce the elevation the solver promised...
        const OloEngine::SunMoonState state = EphemerisStateFor(s.Hours, c.DayOfYear, c.LatitudeDeg);
        const double achievedDeg = static_cast<double>(state.SunElevationRadians) * kRadToDeg;
        EXPECT_NEAR(static_cast<double>(s.AchievedElevationDeg), achievedDeg, 0.5)
            << "case elev=" << c.ElevationDeg << " az=" << c.AzimuthDeg;
        EXPECT_NEAR(static_cast<double>(c.ElevationDeg), achievedDeg, 0.5)
            << "case elev=" << c.ElevationDeg << " az=" << c.AzimuthDeg;

        // ...and place the sun on the requested east/west side (azimuth from
        // +Z north toward +X east: sin > 0 = east).
        const double requestedSide = std::sin(static_cast<double>(c.AzimuthDeg) / kRadToDeg);
        const double achievedSide = std::sin(static_cast<double>(state.SunAzimuthRadians));
        EXPECT_GT(requestedSide * achievedSide, 0.0)
            << "case elev=" << c.ElevationDeg << " az=" << c.AzimuthDeg;
    }
}
