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

#include <cmath>
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

// ---- Sun / time-of-day override ------------------------------------------
// Pure sun-direction math behind olo_scene_set_time_of_day / olo_scene_set_sun_angle.
// The live bake/override is verified over the MCP attach loop; this pins the
// time -> direction / angle -> direction mapping and the result JSON shape.

TEST(McpRenderOverridesSun, TimeOfDayNoonIsOverhead)
{
    const RO::SunVec3 d = RO::SunDirectionFromTimeOfDay(12.0);
    EXPECT_NEAR(0.0, d.X, 1e-9);
    EXPECT_NEAR(1.0, d.Y, 1e-9);
    EXPECT_NEAR(0.0, d.Z, 1e-9);
    EXPECT_NEAR(90.0, RO::SunElevationDegrees(d), 1e-6);
}

TEST(McpRenderOverridesSun, TimeOfDaySunriseAndSunsetSitOnHorizon)
{
    const RO::SunVec3 sunrise = RO::SunDirectionFromTimeOfDay(6.0);
    EXPECT_NEAR(0.0, RO::SunElevationDegrees(sunrise), 1e-6); // on the horizon
    EXPECT_GT(sunrise.X, 0.5);                                // rises in the east (+X)

    const RO::SunVec3 sunset = RO::SunDirectionFromTimeOfDay(18.0);
    EXPECT_NEAR(0.0, RO::SunElevationDegrees(sunset), 1e-6);
    EXPECT_LT(sunset.X, -0.5); // sets in the west (-X)
}

TEST(McpRenderOverridesSun, TimeOfDayNightIsBelowHorizonAndDayIsAbove)
{
    EXPECT_LT(RO::SunElevationDegrees(RO::SunDirectionFromTimeOfDay(0.0)), 0.0);  // midnight
    EXPECT_LT(RO::SunElevationDegrees(RO::SunDirectionFromTimeOfDay(24.0)), 0.0); // wraps to midnight
    EXPECT_LT(RO::SunElevationDegrees(RO::SunDirectionFromTimeOfDay(3.0)), 0.0);  // pre-dawn
    EXPECT_GT(RO::SunElevationDegrees(RO::SunDirectionFromTimeOfDay(9.0)), 0.0);  // mid-morning
    EXPECT_GT(RO::SunElevationDegrees(RO::SunDirectionFromTimeOfDay(15.0)), 0.0); // mid-afternoon
}

TEST(McpRenderOverridesSun, EveryDirectionIsUnitLength)
{
    // Integer step counter (avoids a floating-point loop counter); samples
    // h = 0.0, 1.5, ... 24.0 inclusive.
    for (std::size_t step = 0; step <= 16; ++step)
    {
        const double h = static_cast<double>(step) * 1.5;
        const RO::SunVec3 d = RO::SunDirectionFromTimeOfDay(h);
        EXPECT_NEAR(1.0, std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z), 1e-9) << "hours=" << h;
    }
}

TEST(McpRenderOverridesSun, SunAnglesRoundTripThroughDirection)
{
    struct Case
    {
        double Yaw;
        double Pitch;
    };
    // Avoid pitch == +/-90 where azimuth is degenerate (the pole).
    const Case cases[] = { { 0.0, 0.0 }, { 90.0, 0.0 }, { 45.0, 30.0 }, { 200.0, -20.0 }, { 270.0, 15.0 } };
    for (const Case& c : cases)
    {
        const RO::SunVec3 d = RO::SunDirectionFromAngles(c.Yaw, c.Pitch);
        EXPECT_NEAR(c.Pitch, RO::SunElevationDegrees(d), 1e-6) << "yaw=" << c.Yaw;
        EXPECT_NEAR(c.Yaw, RO::SunAzimuthDegrees(d), 1e-6) << "yaw=" << c.Yaw;
    }
}

TEST(McpRenderOverridesSun, SunAnglePitchStraightUpIsOverhead)
{
    const RO::SunVec3 d = RO::SunDirectionFromAngles(123.0 /*any yaw*/, 90.0);
    EXPECT_NEAR(0.0, d.X, 1e-9);
    EXPECT_NEAR(1.0, d.Y, 1e-9);
    EXPECT_NEAR(0.0, d.Z, 1e-9);
}

TEST(McpRenderOverridesSun, NormalizeSunCollapsesZeroToUp)
{
    const RO::SunVec3 d = RO::NormalizeSun(RO::SunVec3{ 0.0, 0.0, 0.0 });
    EXPECT_NEAR(0.0, d.X, 1e-12);
    EXPECT_NEAR(1.0, d.Y, 1e-12);
    EXPECT_NEAR(0.0, d.Z, 1e-12);
}

TEST(McpRenderOverridesSun, OverrideJsonActiveFromTimeOfDay)
{
    RO::SunOverrideResult r;
    r.Active = true;
    r.Direction = RO::SunDirectionFromTimeOfDay(12.0);
    r.HasHours = true;
    r.Hours = 12.0;
    r.Source = "timeOfDay";

    const Json j = RO::ToJson(r);
    EXPECT_TRUE(j.at("active").get<bool>());
    EXPECT_EQ("timeOfDay", j.at("source").get<std::string>());
    ASSERT_TRUE(j.at("sunDirection").is_array());
    ASSERT_EQ(3u, j.at("sunDirection").size());
    EXPECT_NEAR(90.0, j.at("elevationDegrees").get<double>(), 1e-6);
    EXPECT_TRUE(j.contains("azimuthDegrees"));
    EXPECT_DOUBLE_EQ(12.0, j.at("hours").get<double>());
    EXPECT_FALSE(j.contains("cleared")); // only present when a clear happened
    EXPECT_FALSE(j.contains("note"));
}

TEST(McpRenderOverridesSun, OverrideJsonClearedOmitsDirection)
{
    RO::SunOverrideResult r;
    r.Active = false;
    r.Cleared = true;
    r.Source = "cleared";

    const Json j = RO::ToJson(r);
    EXPECT_FALSE(j.at("active").get<bool>());
    EXPECT_TRUE(j.at("cleared").get<bool>());
    EXPECT_EQ("cleared", j.at("source").get<std::string>());
    // No direction-derived fields when no override is active.
    EXPECT_FALSE(j.contains("sunDirection"));
    EXPECT_FALSE(j.contains("elevationDegrees"));
    EXPECT_FALSE(j.contains("azimuthDegrees"));
    EXPECT_FALSE(j.contains("hours"));
}

TEST(McpRenderOverridesSun, OverrideJsonAngleSourceOmitsHoursAndKeepsNote)
{
    RO::SunOverrideResult r;
    r.Active = true;
    r.Direction = RO::SunDirectionFromAngles(45.0, 30.0);
    r.Source = "sunAngle";
    r.Note = "No ProceduralSkyComponent in the active scene, so this sun override has no visible effect yet.";

    const Json j = RO::ToJson(r);
    EXPECT_TRUE(j.at("active").get<bool>());
    EXPECT_FALSE(j.contains("hours")); // HasHours is false for the angle path
    ASSERT_TRUE(j.contains("note"));
    EXPECT_NE(std::string::npos, j.at("note").get<std::string>().find("ProceduralSky"));
}
