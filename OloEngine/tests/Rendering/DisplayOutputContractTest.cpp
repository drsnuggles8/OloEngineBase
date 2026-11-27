// OLO_TEST_LAYER: L1
// =============================================================================
// DisplayOutputContractTest.cpp — the HDR display output contract (#1353).
//
// The CPU half of DisplayOutput.h, which DisplayEncode.glsl mirrors term by
// term (the shader tests hold the GLSL to it):
//
//  * transfer and gamut: SMPTE ST 2084 against its published code values,
//    BT.2087's Rec.709 -> Rec.2020 matrix;
//  * the BT.2390 EETF and the HDR tone map: identity below the knee, the
//    display peak reached and never exceeded, hue kept, monotonic;
//  * a calibrated ramp of known-nit patches through the whole chain
//    (scene -> nits -> composition -> swapchain signal -> nits back);
//  * the diagnosed mode selection, every branch with its reason;
//  * the settings lifecycle: sanitize, generation, YAML round trip.
// =============================================================================
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/DisplayOutput.h"
#include "TestTempDir.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // The process-global settings and active selection reach every later
    // renderer test (an HDR active mode switches the post chain to RGBA16F),
    // so each case here starts and ends at the defaults.
    class DisplayOutputContract : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            DisplayOutput::Reset();
        }
        void TearDown() override
        {
            DisplayOutput::Reset();
        }
    };

    [[nodiscard]] DisplayMapping Hdr10Mapping(f32 paper = 200.0f, f32 ui = 200.0f, f32 peak = 1000.0f, f32 black = 0.0f)
    {
        DisplayMapping m;
        m.Mode = DisplayOutputMode::HDR10;
        m.PaperWhiteNits = paper;
        m.UIWhiteNits = ui;
        m.PeakNits = peak;
        m.MinNits = black;
        return m;
    }

    [[nodiscard]] DisplayCapabilities HdrDisplay(DisplayHdrState os, bool hdr10 = true, bool scrgb = true)
    {
        DisplayCapabilities caps;
        caps.BackendHasHdrRoute = true;
        caps.OffersHDR10 = hdr10;
        caps.OffersScRGB = scrgb;
        caps.OsHdr = os;
        return caps;
    }
} // namespace

// SMPTE ST 2084 against its published signal values (BT.2100 / the widely
// quoted 10-bit codes: 100 nits = 520, 1000 nits = 769 of 1023).
TEST_F(DisplayOutputContract, PqMatchesTheStandardsCodeValues)
{
    EXPECT_NEAR(PqEncodeNits(0.0f), 0.0f, 1.0e-5f);
    EXPECT_NEAR(PqEncodeNits(100.0f), 0.508078f, 2.0e-5f);
    EXPECT_NEAR(PqEncodeNits(203.0f), 0.580690f, 2.0e-5f); // BT.2408 HDR reference white, 58 %
    EXPECT_NEAR(PqEncodeNits(1000.0f), 0.751827f, 2.0e-5f);
    EXPECT_NEAR(PqEncodeNits(10000.0f), 1.0f, 1.0e-6f);
    EXPECT_EQ(std::lround(PqEncodeNits(100.0f) * 1023.0f), 520);
    EXPECT_EQ(std::lround(PqEncodeNits(1000.0f) * 1023.0f), 769);

    // The EOTF is the inverse, across five decades.
    for (f32 nits = 0.01f; nits <= 10000.0f; nits *= 1.7f)
        EXPECT_NEAR(PqDecodeToNits(PqEncodeNits(nits)), nits, nits * 1.0e-3f) << nits << " nits";
    // Out of range and non-finite inputs clamp, never NaN.
    EXPECT_EQ(PqEncodeNits(-5.0f), PqEncodeNits(0.0f));
    EXPECT_EQ(PqEncodeNits(1.0e6f), 1.0f);
    EXPECT_TRUE(std::isfinite(PqEncodeNits(std::numeric_limits<f32>::quiet_NaN())));
}

TEST_F(DisplayOutputContract, Rec709ToRec2020KeepsWhiteAndMovesThePrimariesInward)
{
    const glm::vec3 white = Rec709ToRec2020(glm::vec3(1.0f));
    EXPECT_NEAR(white.r, 1.0f, 1.0e-6f);
    EXPECT_NEAR(white.g, 1.0f, 1.0e-6f);
    EXPECT_NEAR(white.b, 1.0f, 1.0e-6f);
    // BT.2087 column 0: where pure Rec.709 red sits inside Rec.2020.
    const glm::vec3 red = Rec709ToRec2020({ 1.0f, 0.0f, 0.0f });
    EXPECT_NEAR(red.r, 0.6274f, 1.0e-4f);
    EXPECT_NEAR(red.g, 0.0691f, 1.0e-4f);
    EXPECT_NEAR(red.b, 0.0164f, 1.0e-4f);
    // Rec.709 is inside Rec.2020: no primary leaves the container.
    for (const glm::vec3 primary : { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) })
    {
        const glm::vec3 c = Rec709ToRec2020(primary);
        EXPECT_GE(std::min({ c.r, c.g, c.b }), 0.0f);
        EXPECT_LE(std::max({ c.r, c.g, c.b }), 1.0f);
    }
}

TEST_F(DisplayOutputContract, TheEetfIsIdentityBelowTheKneeAndReachesThePeakExactly)
{
    constexpr f32 kPeak = 1000.0f;
    const f32 maxLum = PqEncodeNits(kPeak);
    const f32 knee = 1.5f * maxLum - 0.5f;
    // Below the knee the signal is untouched (black at 0 nits: no lift).
    for (f32 e = 0.0f; e < knee; e += 0.01f)
        EXPECT_NEAR(Bt2390Eetf(e, 0.0f, kPeak), e, 1.0e-6f) << e;
    // The mastering peak lands on the display peak.
    EXPECT_NEAR(Bt2390Eetf(1.0f, 0.0f, kPeak), maxLum, 1.0e-6f);
    // Monotonic over the whole range, and never above the display peak.
    f32 previous = -1.0f;
    for (u32 i = 0; i <= 1000u; ++i)
    {
        const f32 out = Bt2390Eetf(static_cast<f32>(i) / 1000.0f, 0.0f, kPeak);
        EXPECT_GE(out, previous) << i;
        EXPECT_LE(out, maxLum + 1.0e-6f) << i;
        previous = out;
    }
    // The black lift: signal 0 lands on the display's black.
    EXPECT_NEAR(PqDecodeToNits(Bt2390Eetf(0.0f, 0.1f, kPeak)), 0.1f, 1.0e-3f);
}

TEST_F(DisplayOutputContract, ToneMapPutsReferenceWhiteAtPaperWhiteAndHighlightsAtMostAtThePeak)
{
    const DisplayMapping m = Hdr10Mapping(200.0f, 200.0f, 1000.0f);
    // Reference white (scene 1.0 after exposure) is paper white: below the knee.
    const glm::vec3 white = ToneMapForHdrDisplay(glm::vec3(1.0f), m, false);
    EXPECT_NEAR(white.g, 200.0f, 0.2f);
    // A highlight a hundred times brighter rolls off to the peak, not past it.
    const glm::vec3 sun = ToneMapForHdrDisplay(glm::vec3(100.0f), m, false);
    EXPECT_LE(sun.r, 1000.0f + 0.5f);
    EXPECT_GT(sun.r, 900.0f);
    // The None operator clips each channel at the peak instead.
    const glm::vec3 clipped = ToneMapForHdrDisplay({ 100.0f, 2.0f, 0.5f }, m, true);
    EXPECT_FLOAT_EQ(clipped.r, 1000.0f);
    EXPECT_FLOAT_EQ(clipped.g, 400.0f);
    EXPECT_FLOAT_EQ(clipped.b, 100.0f);
    // The rolloff acts on max(R, G, B): channel ratios (the hue) survive, and
    // no channel exceeds the peak.
    const glm::vec3 orange = ToneMapForHdrDisplay({ 40.0f, 20.0f, 5.0f }, m, false);
    EXPECT_NEAR(orange.g / orange.r, 0.5f, 1.0e-4f);
    EXPECT_NEAR(orange.b / orange.r, 0.125f, 1.0e-4f);
    EXPECT_LE(orange.r, 1000.0f + 0.5f);
    // Bad input degrades to black, never NaN.
    const glm::vec3 bad = ToneMapForHdrDisplay({ std::numeric_limits<f32>::quiet_NaN(), -3.0f, 0.0f }, m, false);
    EXPECT_TRUE(std::isfinite(bad.r) && std::isfinite(bad.g) && std::isfinite(bad.b));
    EXPECT_NEAR(bad.r, 0.0f, 1.0e-4f);
}

// The calibrated ramp: known-nit grey patches from 0.05 to 10000 nits through
// scene -> display nits -> composition -> HDR10 signal, decoded back. Below the
// knee the patch comes back at its own luminance; above it, monotonic and
// capped at the peak. The same ramp for scRGB, whose 1.0 is 80 nits.
TEST_F(DisplayOutputContract, KnownNitPatchesSurviveTheWholeChain)
{
    constexpr f32 kGamma = 2.2f;
    const DisplayMapping hdr10 = Hdr10Mapping(203.0f, 160.0f, 600.0f);
    DisplayMapping scrgb = hdr10;
    scrgb.Mode = DisplayOutputMode::ScRGB;
    const f32 kneeNits = PqDecodeToNits(1.5f * PqEncodeNits(hdr10.PeakNits) - 0.5f);

    f32 previousHdr10 = -1.0f;
    for (f32 nits = 0.05f; nits <= 10000.0f; nits *= 1.5f)
    {
        const glm::vec3 scene(nits / hdr10.PaperWhiteNits);
        const glm::vec3 display = ToneMapForHdrDisplay(scene, hdr10, false);
        const glm::vec3 composition = NitsToComposition(display, hdr10, kGamma);

        const glm::vec3 signal = EncodeCompositionForDisplay(composition, hdr10, kGamma);
        const f32 back = PqDecodeToNits(signal.g);
        if (nits < kneeNits * 0.98f)
            EXPECT_NEAR(back, nits, nits * 2.0e-3f + 1.0e-4f) << nits << " nits below the knee";
        EXPECT_LE(back, hdr10.PeakNits * 1.001f) << nits;
        EXPECT_GE(back, previousHdr10) << nits;
        previousHdr10 = back;

        const glm::vec3 linear = EncodeCompositionForDisplay(composition, scrgb, kGamma);
        EXPECT_NEAR(linear.g * kScRgbReferenceNits, display.g, display.g * 2.0e-3f + 1.0e-4f) << nits;
    }
}

TEST_F(DisplayOutputContract, UiWhiteIsIndependentOfPaperWhiteAndExposure)
{
    constexpr f32 kGamma = 2.2f;
    // A UI pixel at composition 1.0 is UI white, whatever the scene's paper
    // white is: the two numbers do not interact.
    for (const f32 paper : { 100.0f, 203.0f, 400.0f })
    {
        const DisplayMapping m = Hdr10Mapping(paper, 250.0f, 1000.0f);
        const glm::vec3 signal = EncodeCompositionForDisplay(glm::vec3(1.0f), m, kGamma);
        EXPECT_NEAR(PqDecodeToNits(signal.g), 250.0f, 0.5f) << "paper white " << paper;
    }
    // And the scene's reference white tracks paper white, not UI white.
    const DisplayMapping m = Hdr10Mapping(300.0f, 120.0f, 1000.0f);
    const glm::vec3 composition = NitsToComposition(ToneMapForHdrDisplay(glm::vec3(1.0f), m, false), m, kGamma);
    EXPECT_NEAR(PqDecodeToNits(EncodeCompositionForDisplay(composition, m, kGamma).g), 300.0f, 0.6f);
}

TEST_F(DisplayOutputContract, EncodeClipsAtThePeakAndLeavesSdrUntouched)
{
    constexpr f32 kGamma = 2.2f;
    const DisplayMapping hdr10 = Hdr10Mapping(200.0f, 200.0f, 600.0f);
    // Composition far above the peak (8x UI white): clamped per channel in
    // Rec.2020 to the peak.
    const glm::vec3 hot = EncodeCompositionForDisplay(glm::pow(glm::vec3(8.0f), glm::vec3(1.0f / kGamma)), hdr10, kGamma);
    EXPECT_NEAR(PqDecodeToNits(hot.r), 600.0f, 0.5f);
    // Gamut: Rec.709 red at UI white arrives as Rec.2020's coordinates of it.
    const glm::vec3 red = EncodeCompositionForDisplay({ 1.0f, 0.0f, 0.0f }, hdr10, kGamma);
    EXPECT_NEAR(PqDecodeToNits(red.r), 0.6274f * 200.0f, 0.2f);
    EXPECT_NEAR(PqDecodeToNits(red.g), 0.0691f * 200.0f, 0.05f);
    EXPECT_NEAR(PqDecodeToNits(red.b), 0.0164f * 200.0f, 0.02f);
    // SDR is the identity: the swapchain stores the composition as is.
    const DisplayMapping sdr;
    const glm::vec3 c(0.25f, 0.5f, 0.75f);
    EXPECT_EQ(EncodeCompositionForDisplay(c, sdr, kGamma), c);
}

// --- The diagnosed selection ------------------------------------------------

TEST_F(DisplayOutputContract, AutoFollowsTheOperatingSystem)
{
    EXPECT_EQ(SelectDisplayOutputMode(DisplayOutputPreference::Auto, HdrDisplay(DisplayHdrState::On)).Mode,
              DisplayOutputMode::HDR10);
    EXPECT_EQ(SelectDisplayOutputMode(DisplayOutputPreference::Auto, HdrDisplay(DisplayHdrState::On, false, true)).Mode,
              DisplayOutputMode::ScRGB);
    const auto off = SelectDisplayOutputMode(DisplayOutputPreference::Auto, HdrDisplay(DisplayHdrState::Off));
    EXPECT_EQ(off.Mode, DisplayOutputMode::SDR);
    EXPECT_FALSE(off.PreferenceNotHonoured);
    EXPECT_NE(off.Reason.ToStdString().find("off"), std::string::npos) << *off.Reason;
    EXPECT_EQ(SelectDisplayOutputMode(DisplayOutputPreference::Auto, HdrDisplay(DisplayHdrState::Unknown)).Mode,
              DisplayOutputMode::SDR);
    // An HDR display whose surface offers no HDR format.
    const auto none = SelectDisplayOutputMode(DisplayOutputPreference::Auto, HdrDisplay(DisplayHdrState::On, false, false));
    EXPECT_EQ(none.Mode, DisplayOutputMode::SDR);
    EXPECT_NE(none.Reason.ToStdString().find("no HDR format"), std::string::npos) << *none.Reason;
}

TEST_F(DisplayOutputContract, AnExplicitPreferenceIsHonouredOrDiagnosed)
{
    // Honoured, with the OS state named when it is off.
    const auto forced = SelectDisplayOutputMode(DisplayOutputPreference::HDR10, HdrDisplay(DisplayHdrState::Off));
    EXPECT_EQ(forced.Mode, DisplayOutputMode::HDR10);
    EXPECT_FALSE(forced.PreferenceNotHonoured);
    EXPECT_NE(forced.Reason.ToStdString().find("OS HDR is off"), std::string::npos) << *forced.Reason;
    // The other HDR route when the requested one is missing.
    const auto swapped = SelectDisplayOutputMode(DisplayOutputPreference::ScRGB, HdrDisplay(DisplayHdrState::On, true, false));
    EXPECT_EQ(swapped.Mode, DisplayOutputMode::HDR10);
    EXPECT_TRUE(swapped.PreferenceNotHonoured);
    // SDR when no route exists at all, and the backend's own reason is carried.
    DisplayCapabilities gl;
    gl.NoRouteReason = "no swapchain colour space";
    gl.OsHdr = DisplayHdrState::On;
    const auto refused = SelectDisplayOutputMode(DisplayOutputPreference::HDR10, gl);
    EXPECT_EQ(refused.Mode, DisplayOutputMode::SDR);
    EXPECT_TRUE(refused.PreferenceNotHonoured);
    EXPECT_NE(refused.Reason.ToStdString().find("no swapchain colour space"), std::string::npos) << *refused.Reason;
    // SDR asked for is SDR everywhere.
    EXPECT_EQ(SelectDisplayOutputMode(DisplayOutputPreference::SDR, HdrDisplay(DisplayHdrState::On)).Mode,
              DisplayOutputMode::SDR);
}

TEST_F(DisplayOutputContract, TheMappingUsesTheDisplaysLuminanceUnlessOverridden)
{
    DisplayCapabilities caps = HdrDisplay(DisplayHdrState::On);
    caps.PeakNits = 308.5f; // this box's BenQ EW3270U, as DXGI reports it
    caps.MinNits = 0.0917f;
    DisplayOutputSettings s;
    DisplayMapping m = ResolveDisplayMapping(DisplayOutputMode::HDR10, s, caps);
    EXPECT_FLOAT_EQ(m.PeakNits, 308.5f);
    EXPECT_FLOAT_EQ(m.MinNits, 0.0917f);
    EXPECT_FLOAT_EQ(m.PaperWhiteNits, 200.0f);
    s.PeakNitsOverride = 1500.0f;
    EXPECT_FLOAT_EQ(ResolveDisplayMapping(DisplayOutputMode::HDR10, s, caps).PeakNits, 1500.0f);
    // No report and no override: HDR10's 1000-nit reference.
    EXPECT_FLOAT_EQ(ResolveDisplayMapping(DisplayOutputMode::HDR10, DisplayOutputSettings{}, HdrDisplay(DisplayHdrState::On)).PeakNits,
                    kFallbackPeakNits);
    // White never above the peak; a nonsense black report is capped.
    s = {};
    s.PaperWhiteNits = 450.0f;
    caps.PeakNits = 300.0f;
    caps.MinNits = 90.0f;
    m = ResolveDisplayMapping(DisplayOutputMode::HDR10, s, caps);
    EXPECT_FLOAT_EQ(m.PaperWhiteNits, 300.0f);
    EXPECT_LE(m.MinNits, 0.05f * m.PaperWhiteNits);
}

// --- The settings lifecycle ---------------------------------------------------

TEST_F(DisplayOutputContract, SanitizeRepairsWhatAFileCanHold)
{
    DisplayOutputSettings s;
    s.Preference = static_cast<DisplayOutputPreference>(42);
    s.PaperWhiteNits = std::numeric_limits<f32>::quiet_NaN();
    s.UIWhiteNits = 9000.0f;
    s.PeakNitsOverride = -std::numeric_limits<f32>::infinity();
    SanitizeDisplayOutputSettings(s);
    EXPECT_EQ(s.Preference, DisplayOutputPreference::Auto);
    EXPECT_FLOAT_EQ(s.PaperWhiteNits, 200.0f);
    EXPECT_FLOAT_EQ(s.UIWhiteNits, kMaxWhiteNits);
    EXPECT_FLOAT_EQ(s.PeakNitsOverride, 0.0f);
}

TEST_F(DisplayOutputContract, OnlyARealChangeMovesTheSettingsGeneration)
{
    const u64 start = DisplayOutput::GetSettingsGeneration();
    DisplayOutput::SetSettings(DisplayOutput::GetSettings());
    EXPECT_EQ(DisplayOutput::GetSettingsGeneration(), start) << "an unchanged set must not wake the swapchain";
    DisplayOutputSettings s = DisplayOutput::GetSettings();
    s.PaperWhiteNits = 250.0f;
    DisplayOutput::SetSettings(s);
    EXPECT_EQ(DisplayOutput::GetSettingsGeneration(), start + 1u);
}

TEST_F(DisplayOutputContract, PreferencesSurviveASaveAndLoad)
{
    DisplayOutputSettings authored;
    authored.Preference = DisplayOutputPreference::ScRGB;
    authored.PaperWhiteNits = 240.0f;
    authored.UIWhiteNits = 180.0f;
    authored.PeakNitsOverride = 750.0f;
    DisplayOutput::SetSettings(authored);
    const auto path = OloEngine::Tests::TempDir() / "config" / "display.yaml";
    ASSERT_TRUE(DisplayOutput::SaveToFile(path));

    DisplayOutput::Reset();
    ASSERT_NE(DisplayOutput::GetSettings(), authored);
    ASSERT_TRUE(DisplayOutput::LoadFromFile(path));
    EXPECT_EQ(DisplayOutput::GetSettings(), authored);

    // The file lives beside config/renderer.yaml, never in scene data.
    EXPECT_EQ(DisplayOutput::DefaultSettingsPath().filename(), "display.yaml");
    EXPECT_EQ(DisplayOutput::DefaultSettingsPath().parent_path().filename(), "config");
}

TEST_F(DisplayOutputContract, ABadFileLeavesTheSettingsAlone)
{
    DisplayOutputSettings authored;
    authored.Preference = DisplayOutputPreference::HDR10;
    authored.PaperWhiteNits = 300.0f;
    DisplayOutput::SetSettings(authored);

    EXPECT_FALSE(DisplayOutput::LoadFromFile(OloEngine::Tests::TempDir() / "missing.yaml"));
    const auto corrupt = OloEngine::Tests::TempDir() / "corrupt.yaml";
    {
        std::ofstream out(corrupt);
        out << "DisplayOutput: [unterminated";
    }
    EXPECT_FALSE(DisplayOutput::LoadFromFile(corrupt));
    EXPECT_EQ(DisplayOutput::GetSettings(), authored);

    // A hand-edited file: an omitted key keeps the user's value, a bad one is
    // ignored, an out-of-range one is sanitized.
    const auto edited = OloEngine::Tests::TempDir() / "edited.yaml";
    {
        std::ofstream out(edited);
        out << "DisplayOutput:\n  Preference: Plasma\n  UIWhiteNits: .nan\n  PeakNitsOverride: 99999\n";
    }
    ASSERT_TRUE(DisplayOutput::LoadFromFile(edited));
    EXPECT_EQ(DisplayOutput::GetSettings().Preference, DisplayOutputPreference::HDR10);
    EXPECT_FLOAT_EQ(DisplayOutput::GetSettings().PaperWhiteNits, 300.0f);
    EXPECT_FLOAT_EQ(DisplayOutput::GetSettings().UIWhiteNits, 200.0f);
    EXPECT_FLOAT_EQ(DisplayOutput::GetSettings().PeakNitsOverride, kMaxPqNits);
}
