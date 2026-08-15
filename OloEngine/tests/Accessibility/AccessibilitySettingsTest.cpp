// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "TestTempDir.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// =============================================================================
// AccessibilitySettings — settings contract, text-scale policy, persistence.
//
// Covers issue #458's global settings home. The three things that can silently
// go wrong here:
//
//  * A non-finite or out-of-range value reaching the shader / the UI renderer.
//    SanitizeAccessibilitySettings is the only thing standing between a
//    hand-edited prefs file and a NaN in a pow().
//  * The text-scale policy disagreeing with itself. ResolveUIFontSize is shared
//    by all four UIRenderer draw sites; if scale-then-floor ever became
//    floor-then-scale, shrinking the scale would quietly push labels back under
//    the legibility minimum, which is the exact regression the "minimum legible
//    size" acceptance criterion exists to prevent.
//  * A prefs file round-trip losing a field.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // Repo-relative paths must NOT be resolved against the process CWD: several
    // suites (the asset-manager-backed Functional ones) chdir during their run,
    // so a CWD-relative open here passes alone and fails in a full-suite run.
    // OLO_TEST_EDITOR_ROOT is the compile-time anchor the other file-reading
    // coverage tests use.
    [[nodiscard]] std::filesystem::path RepoRoot()
    {
        return std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
    }

    // Every test mutates the process-global settings, and gtest runs each case in
    // its own process — but --gtest_filter can put several in one, so reset
    // explicitly rather than relying on process isolation.
    class AccessibilitySettingsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Accessibility::Reset();
        }
        void TearDown() override
        {
            Accessibility::Reset();
        }
    };
} // namespace

TEST_F(AccessibilitySettingsTest, DefaultsAreOffAndNeutral)
{
    const auto& s = Accessibility::Get();

    // Every existing project must render byte-identically until someone opts in.
    EXPECT_FALSE(s.SubtitlesEnabled);
    EXPECT_EQ(s.ColorBlind, ColorBlindMode::None);
    EXPECT_FLOAT_EQ(s.UITextScale, 1.0f);
    EXPECT_FLOAT_EQ(s.MinimumFontSize, kDefaultMinimumFontSize);
    EXPECT_FLOAT_EQ(kDefaultMinimumFontSize, 0.0f)
        << "the legibility floor must default OFF — see the next test for why";
}

TEST_F(AccessibilitySettingsTest, DefaultSettingsDoNotResizeTextAuthoredBelowTheRecommendedFloor)
{
    // The no-opt-in invariant, at the one point it is easiest to break. A
    // non-zero default MinimumFontSize would silently enlarge any label authored
    // below it — and since UILayoutSystem does not reflow on font size, that
    // label could then overflow its rect. A project that never touched an
    // accessibility setting would get a layout change.
    //
    // Deliberately probing BELOW the recommended floor: a test that only sampled
    // sizes above it would pass with the floor defaulted on and prove nothing.
    const AccessibilitySettings defaults;
    for (const f32 authored : { 1.0f, 6.0f, 8.0f, 10.0f, kRecommendedMinimumFontSize - 0.5f })
    {
        EXPECT_FLOAT_EQ(ResolveUIFontSize(authored, defaults), authored)
            << "default settings resized a " << authored << "px label";
    }
}

TEST_F(AccessibilitySettingsTest, AnOptedInFloorDoesRaiseSmallText)
{
    // …and the floor still works when a player asks for it, which is what makes
    // the default-off choice a policy rather than a missing feature.
    AccessibilitySettings s;
    s.MinimumFontSize = kRecommendedMinimumFontSize;

    EXPECT_FLOAT_EQ(ResolveUIFontSize(8.0f, s), kRecommendedMinimumFontSize);
    EXPECT_FLOAT_EQ(ResolveUIFontSize(24.0f, s), 24.0f) << "the floor must not shrink text above it";
}

TEST_F(AccessibilitySettingsTest, SanitizeRejectsNonFiniteAndOutOfRangeValues)
{
    AccessibilitySettings s;
    s.UITextScale = std::numeric_limits<f32>::quiet_NaN();
    s.MinimumFontSize = std::numeric_limits<f32>::infinity();
    s.SubtitleFontSize = -5.0f;
    s.SubtitleBackgroundOpacity = 4.0f;
    s.ColorBlindSeverity = std::numeric_limits<f32>::quiet_NaN();

    SanitizeAccessibilitySettings(s);

    EXPECT_TRUE(std::isfinite(s.UITextScale));
    EXPECT_GE(s.UITextScale, kMinUITextScale);
    EXPECT_LE(s.UITextScale, kMaxUITextScale);
    EXPECT_TRUE(std::isfinite(s.MinimumFontSize));
    EXPECT_LE(s.MinimumFontSize, kMaxMinimumFontSize);
    EXPECT_GT(s.SubtitleFontSize, 0.0f);
    EXPECT_LE(s.SubtitleBackgroundOpacity, 1.0f);
    EXPECT_FLOAT_EQ(s.ColorBlindSeverity, 1.0f);
}

TEST_F(AccessibilitySettingsTest, SanitizeFallsBackRatherThanSaturatingDiscriminatedEnums)
{
    // Saturating an out-of-range MODE would silently select a DIFFERENT valid
    // deficiency (e.g. 99 → Tritanopia), which is worse than doing nothing:
    // the player would be adapted for a condition they don't have.
    AccessibilitySettings s;
    s.ColorBlind = static_cast<ColorBlindMode>(99);
    s.ColorBlindMethod = static_cast<ColorBlindAdaptation>(-3);

    SanitizeAccessibilitySettings(s);

    EXPECT_EQ(s.ColorBlind, ColorBlindMode::None);
    EXPECT_EQ(s.ColorBlindMethod, ColorBlindAdaptation::Correct);
}

TEST_F(AccessibilitySettingsTest, SetSanitizesSoTheGlobalIsNeverGarbage)
{
    AccessibilitySettings bad;
    bad.UITextScale = std::numeric_limits<f32>::quiet_NaN();
    bad.ColorBlind = static_cast<ColorBlindMode>(42);

    Accessibility::Set(bad);

    EXPECT_TRUE(std::isfinite(Accessibility::Get().UITextScale));
    EXPECT_EQ(Accessibility::Get().ColorBlind, ColorBlindMode::None);
}

TEST_F(AccessibilitySettingsTest, ResolveUIFontSizeScalesThenAppliesTheLegibilityFloor)
{
    AccessibilitySettings s;
    s.UITextScale = 2.0f;
    s.MinimumFontSize = 12.0f;

    EXPECT_FLOAT_EQ(ResolveUIFontSize(10.0f, s), 20.0f);

    // Scaling DOWN must still clear the floor. Order matters: flooring first and
    // scaling after would give 12 * 0.5 = 6 — under the legible minimum.
    s.UITextScale = 0.5f;
    EXPECT_FLOAT_EQ(ResolveUIFontSize(10.0f, s), 12.0f) << "the legibility floor must be applied AFTER the scale";
}

TEST_F(AccessibilitySettingsTest, ResolveUIFontSizeIsIdentityAtDefaultSettingsAboveTheFloor)
{
    // The no-opt-in guarantee: an authored size above the floor is untouched.
    const AccessibilitySettings s;
    EXPECT_FLOAT_EQ(ResolveUIFontSize(24.0f, s), 24.0f);
}

TEST_F(AccessibilitySettingsTest, ResolveUIFontSizePassesMalformedAuthoredSizesThrough)
{
    AccessibilitySettings s;
    s.UITextScale = 2.0f;
    s.MinimumFontSize = 12.0f;

    // Zero/negative is malformed scene data. Promoting it to the floor would
    // draw text where the author asked for none.
    EXPECT_FLOAT_EQ(ResolveUIFontSize(0.0f, s), 0.0f);
    EXPECT_FLOAT_EQ(ResolveUIFontSize(-4.0f, s), -4.0f);
    EXPECT_TRUE(std::isnan(ResolveUIFontSize(std::numeric_limits<f32>::quiet_NaN(), s)));
}

TEST_F(AccessibilitySettingsTest, MinimumFontSizeOfZeroDisablesTheFloor)
{
    AccessibilitySettings s;
    s.UITextScale = 0.5f;
    s.MinimumFontSize = 0.0f;

    EXPECT_FLOAT_EQ(ResolveUIFontSize(10.0f, s), 5.0f);
}

TEST_F(AccessibilitySettingsTest, UBOPayloadMirrorsTheSettings)
{
    AccessibilitySettings s;
    s.ColorBlind = ColorBlindMode::Deuteranopia;
    s.ColorBlindSeverity = 0.6f;
    s.ColorBlindMethod = ColorBlindAdaptation::Simulate;

    const ColorBlindUBOData data = MakeColorBlindUBOData(s, 2.4f);

    EXPECT_FLOAT_EQ(data.Params.x, 2.0f);
    EXPECT_FLOAT_EQ(data.Params.y, 0.6f);
    EXPECT_FLOAT_EQ(data.Params.z, 1.0f);
    EXPECT_FLOAT_EQ(data.Params.w, 2.4f);
}

TEST_F(AccessibilitySettingsTest, UBOPayloadSubstitutesSafeValuesForGarbage)
{
    AccessibilitySettings s;
    s.ColorBlind = ColorBlindMode::Protanopia;
    s.ColorBlindSeverity = std::numeric_limits<f32>::quiet_NaN();

    const ColorBlindUBOData data = MakeColorBlindUBOData(s, std::numeric_limits<f32>::quiet_NaN());

    EXPECT_FLOAT_EQ(data.Params.y, 1.0f);
    EXPECT_FLOAT_EQ(data.Params.w, 2.2f);
}

TEST_F(AccessibilitySettingsTest, UBOPayloadTakesTheDisplayGammaFromItsCaller)
{
    // The gamma the stage decodes with has exactly one source: the value
    // ToneMapPass encoded with (PostProcessSettings::Gamma). It is a PARAMETER
    // rather than a field on AccessibilitySettings so a second copy cannot exist
    // to drift — this test pins that it is threaded, not defaulted.
    AccessibilitySettings s;
    s.ColorBlind = ColorBlindMode::Deuteranopia;

    EXPECT_FLOAT_EQ(MakeColorBlindUBOData(s, 1.8f).Params.w, 1.8f);
    EXPECT_FLOAT_EQ(MakeColorBlindUBOData(s, 2.2f).Params.w, 2.2f);
    // Zero would divide by zero in the shader's encode.
    EXPECT_GE(MakeColorBlindUBOData(s, 0.0f).Params.w, 0.1f);
}

TEST_F(AccessibilitySettingsTest, SaveLoadRoundTripsEveryField)
{
    const auto path = OloEngine::Tests::TempDir() / "accessibility.yaml";

    AccessibilitySettings s;
    s.SubtitlesEnabled = true;
    s.SubtitleShowSpeaker = false;
    s.SubtitleFontSize = 33.0f;
    s.SubtitleBackgroundOpacity = 0.25f;
    s.UITextScale = 1.75f;
    s.MinimumFontSize = 18.0f;
    s.ColorBlind = ColorBlindMode::Tritanopia;
    s.ColorBlindMethod = ColorBlindAdaptation::Simulate;
    s.ColorBlindSeverity = 0.4f;
    Accessibility::Set(s);

    ASSERT_TRUE(Accessibility::SaveToFile(path));

    Accessibility::Reset();
    ASSERT_NE(Accessibility::Get().ColorBlind, ColorBlindMode::Tritanopia) << "Reset did not take effect";

    ASSERT_TRUE(Accessibility::LoadFromFile(path));

    const auto& r = Accessibility::Get();
    EXPECT_TRUE(r.SubtitlesEnabled);
    EXPECT_FALSE(r.SubtitleShowSpeaker);
    EXPECT_FLOAT_EQ(r.SubtitleFontSize, 33.0f);
    EXPECT_FLOAT_EQ(r.SubtitleBackgroundOpacity, 0.25f);
    EXPECT_FLOAT_EQ(r.UITextScale, 1.75f);
    EXPECT_FLOAT_EQ(r.MinimumFontSize, 18.0f);
    EXPECT_EQ(r.ColorBlind, ColorBlindMode::Tritanopia);
    EXPECT_EQ(r.ColorBlindMethod, ColorBlindAdaptation::Simulate);
    EXPECT_FLOAT_EQ(r.ColorBlindSeverity, 0.4f);
}

TEST_F(AccessibilitySettingsTest, LoadOfAMissingFileLeavesSettingsUntouched)
{
    AccessibilitySettings s;
    s.UITextScale = 1.5f;
    Accessibility::Set(s);

    EXPECT_FALSE(Accessibility::LoadFromFile(OloEngine::Tests::TempDir() / "does-not-exist.yaml"));
    EXPECT_FLOAT_EQ(Accessibility::Get().UITextScale, 1.5f) << "a first run with no prefs file must not clobber the current settings";
}

TEST_F(AccessibilitySettingsTest, LoadOfACorruptFileFailsWithoutCorruptingSettings)
{
    const auto path = OloEngine::Tests::TempDir() / "corrupt.yaml";
    {
        std::ofstream f(path);
        f << "Accessibility: [this is: not, a map\n  - broken";
    }

    AccessibilitySettings s;
    s.UITextScale = 1.25f;
    Accessibility::Set(s);

    EXPECT_FALSE(Accessibility::LoadFromFile(path));
    EXPECT_FLOAT_EQ(Accessibility::Get().UITextScale, 1.25f);
}

TEST_F(AccessibilitySettingsTest, LoadSanitizesHandEditedGarbage)
{
    // The prefs file is plain YAML a user can open, so ".nan" and an
    // out-of-range enum are reachable without any code path producing them.
    const auto path = OloEngine::Tests::TempDir() / "handedited.yaml";
    {
        std::ofstream f(path);
        f << "Accessibility:\n"
             "  UITextScale: .nan\n"
             "  ColorBlindMode: 77\n"
             "  ColorBlindSeverity: 12.0\n";
    }

    ASSERT_TRUE(Accessibility::LoadFromFile(path));

    const auto& r = Accessibility::Get();
    EXPECT_TRUE(std::isfinite(r.UITextScale));
    EXPECT_EQ(r.ColorBlind, ColorBlindMode::None);
    EXPECT_LE(r.ColorBlindSeverity, 1.0f);
}

TEST_F(AccessibilitySettingsTest, EveryUIRendererTextDrawSiteRoutesThroughTheScale)
{
    // Issue #458's second acceptance criterion is "a global text-scale setting
    // visibly scales ALL UI text", and "all" is the load-bearing word: there are
    // four draw sites (UIText, InputField, and two Dropdown paths) and a fifth
    // added later would silently opt out. Exercising the real renderer needs a GL
    // context, so this scans the source instead — the same technique
    // ColorBlindMathTest uses to keep the shader and its CPU twin in step.
    //
    // The rule: every read of a *FontSize member inside UIRenderer.cpp must be
    // wrapped in Accessibility::ResolveFontSize.
    const auto rendererPath = RepoRoot() / "OloEngine" / "src" / "OloEngine" / "UI" / "UIRenderer.cpp";
    std::ifstream file(rendererPath);
    ASSERT_TRUE(file.is_open()) << "UIRenderer.cpp not found at " << rendererPath.string();

    u32 lineNumber = 0;
    std::vector<std::string> unscaled;
    for (std::string line; std::getline(file, line);)
    {
        ++lineNumber;
        // Comments explain the policy and legitimately name the member.
        if (const auto firstNonSpace = line.find_first_not_of(" \t");
            firstNonSpace != std::string::npos && line.compare(firstNonSpace, 2, "//") == 0)
        {
            continue;
        }
        if (line.find("m_FontSize") == std::string::npos)
            continue;
        if (line.find("Accessibility::ResolveFontSize") != std::string::npos)
            continue;
        unscaled.push_back(std::to_string(lineNumber) + ": " + line);
    }

    EXPECT_TRUE(unscaled.empty())
        << "UIRenderer.cpp reads a font size without the global accessibility scale. "
           "Wrap it in Accessibility::ResolveFontSize(...) — an unwrapped draw site is text "
           "that ignores the player's setting, with nothing failing. Offending line(s):\n"
        << [&unscaled]
    {
        std::string joined;
        for (const auto& l : unscaled)
            joined += "  " + l + "\n";
        return joined;
    }();
}

TEST_F(AccessibilitySettingsTest, LoadOfAPartialFileKeepsUnmentionedPreferences)
{
    AccessibilitySettings s;
    s.SubtitlesEnabled = true;
    s.UITextScale = 1.5f;
    Accessibility::Set(s);

    const auto path = OloEngine::Tests::TempDir() / "partial.yaml";
    {
        std::ofstream f(path);
        f << "Accessibility:\n  UITextScale: 2.0\n";
    }

    ASSERT_TRUE(Accessibility::LoadFromFile(path));

    EXPECT_FLOAT_EQ(Accessibility::Get().UITextScale, 2.0f);
    EXPECT_TRUE(Accessibility::Get().SubtitlesEnabled) << "a key absent from the file must leave that preference alone — "
                                                          "YAML is self-describing, so a missing key is not a desync";
}
