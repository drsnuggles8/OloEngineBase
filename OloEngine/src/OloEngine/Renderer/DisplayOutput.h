#pragma once

#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>

// =============================================================================
// Display output (issue #1353): how the finished frame reaches the monitor.
//
// THE SPACES, in frame order. Every stage is named by the space it works in:
//
//  1. Scene-referred linear Rec.709 radiance. Lighting, bloom, DOF, fog and
//     colour grading run here; nothing in this file touches it, and the output
//     mode never changes it ("scene transport").
//  2. ToneMapPass multiplies by exposure, which makes 1.0 the scene's REFERENCE
//     WHITE: the level the display shows at PaperWhiteNits. SDR applies the
//     selected operator and clamps to [0, 1]; HDR maps luminance in nits to the
//     display's range with the BT.2390 EETF (ToneMapForHdrDisplay), so
//     highlights roll off to PeakNits instead of clipping at paper white.
//  3. ToneMapPass then expresses the result relative to UI WHITE and gamma-
//     encodes it with PostProcessSettings::Gamma. This is the COMPOSITION
//     space: display-referred, Rec.709 primaries, 1.0 = UIWhiteNits, values
//     above 1.0 only in HDR mode. Everything after the tone map composes here:
//     sharpening, vignette, FXAA, the selection outline, the game UI
//     (UIComposite), colour-vision adaptation (which decodes and re-encodes
//     around the same gamma), and in the editor the ImGui chrome.
//  4. The display encode (EncodeCompositionForDisplay, DisplayEncode.glsl) is
//     the last step before present: linearise, scale by UIWhiteNits, then
//     SDR: unchanged; HDR10: Rec.709 -> Rec.2020, clamp to PeakNits, ST 2084
//     (PQ); scRGB: clamp to PeakNits, divide by 80 nits.
//
// UI white and paper white are separate numbers on purpose: the UI must stay
// readable at its own brightness whatever the scene's exposure or reference
// white does (acceptance criterion 2). In SDR both are 1.0 in the composition
// space and the whole chain is bit-identical to the pre-#1353 renderer.
// =============================================================================

namespace OloEngine
{
    // What the swapchain carries.
    enum class DisplayOutputMode : i32
    {
        SDR = 0,   // 8-bit UNORM, sRGB non-linear colour space
        HDR10 = 1, // A2B10G10R10, HDR10 ST 2084 colour space (Rec.2020 primaries, PQ)
        ScRGB = 2  // R16G16B16A16_SFLOAT, extended sRGB linear (Rec.709 primaries, 1.0 = 80 nits)
    };

    // What the user asked for. Auto follows the operating system: HDR when the
    // window's display is in HDR mode, SDR otherwise.
    enum class DisplayOutputPreference : i32
    {
        Auto = 0,
        SDR = 1,
        HDR10 = 2,
        ScRGB = 3
    };

    // Whether the operating system composes the window's display in HDR.
    enum class DisplayHdrState : i32
    {
        Unknown = 0, // no platform query (non-Windows, or the query failed)
        Off = 1,
        On = 2
    };

    [[nodiscard]] const char* ToString(DisplayOutputMode mode) noexcept;
    [[nodiscard]] const char* ToString(DisplayOutputPreference preference) noexcept;
    [[nodiscard]] const char* ToString(DisplayHdrState state) noexcept;

    // What the backend and the platform report for the window's display. Filled
    // by the presenting backend (VulkanContext::CreateSwapchain); OpenGL fills
    // only BackendHasHdrRoute = false and the reason.
    struct DisplayCapabilities
    {
        // False when the backend cannot present HDR at all; the reason says why.
        bool BackendHasHdrRoute = false;
        FString NoRouteReason;
        // The swapchain surface offers the format + colour space pair.
        bool OffersHDR10 = false;
        bool OffersScRGB = false;
        DisplayHdrState OsHdr = DisplayHdrState::Unknown;
        // Display luminance from the platform, in nits; 0 = not reported.
        f32 PeakNits = 0.0f;
        f32 MaxFullFrameNits = 0.0f;
        f32 MinNits = 0.0f;
        // CIE xy chromaticities the display reports; all zero = not reported.
        glm::vec2 RedPrimary{ 0.0f };
        glm::vec2 GreenPrimary{ 0.0f };
        glm::vec2 BluePrimary{ 0.0f };
        glm::vec2 WhitePoint{ 0.0f };
        FString DisplayName;
    };

    // Authored output preferences. A PLAYER/USER preference, not scene art
    // direction (the same reasoning as AccessibilitySettings): it depends on
    // the monitor, so it lives in a per-machine file beside config/renderer.yaml
    // and never in scene YAML, the project or a save game.
    struct DisplayOutputSettings
    {
        DisplayOutputPreference Preference = DisplayOutputPreference::Auto;
        // The scene's reference white on an HDR display (exposure 1.0 maps here).
        f32 PaperWhiteNits = 200.0f;
        // The UI's white on an HDR display, independent of the scene.
        f32 UIWhiteNits = 200.0f;
        // 0 = use the luminance the display reports (kFallbackPeakNits if none).
        f32 PeakNitsOverride = 0.0f;

        // Change detection, not arithmetic: the same bits, field by field (a
        // settings change is what wakes the swapchain re-selection, and a value
        // the user set again must not count as one). No float `==`.
        [[nodiscard]] bool operator==(const DisplayOutputSettings& other) const noexcept
        {
            return Preference == other.Preference && std::bit_cast<u32>(PaperWhiteNits) == std::bit_cast<u32>(other.PaperWhiteNits) &&
                   std::bit_cast<u32>(UIWhiteNits) == std::bit_cast<u32>(other.UIWhiteNits) &&
                   std::bit_cast<u32>(PeakNitsOverride) == std::bit_cast<u32>(other.PeakNitsOverride);
        }
    };

    // operator== above lists every field by hand: a new field must be added to
    // it, or its change never wakes the swapchain re-selection.
    static_assert(sizeof(DisplayOutputSettings) == 16, "DisplayOutputSettings changed: update its operator==");

    inline constexpr f32 kMinWhiteNits = 80.0f;
    inline constexpr f32 kMaxWhiteNits = 500.0f;
    inline constexpr f32 kMaxPqNits = 10000.0f;
    // HDR10's reference mastering peak, used when the display reports none.
    inline constexpr f32 kFallbackPeakNits = 1000.0f;
    // scRGB's 1.0: the sRGB reference display white.
    inline constexpr f32 kScRgbReferenceNits = 80.0f;

    // Clamp every field to a finite, sane range (CLAUDE.md: floats from disk are
    // validated with std::isfinite). An out-of-range enum falls back to Auto.
    void SanitizeDisplayOutputSettings(DisplayOutputSettings& s) noexcept;

    // The mode the swapchain is created with, and why. Every branch states its
    // reason, so the log and the editor panel can say which preference was not
    // honoured and what decided it (acceptance criterion 3: "a diagnosed output
    // mode").
    struct DisplayOutputSelection
    {
        DisplayOutputMode Mode = DisplayOutputMode::SDR;
        // True when the preference named a mode and a different one was chosen.
        bool PreferenceNotHonoured = false;
        FString Reason;
    };

    [[nodiscard]] DisplayOutputSelection SelectDisplayOutputMode(DisplayOutputPreference preference,
                                                                 const DisplayCapabilities& caps);

    // Fills the platform half of `caps` for the display that holds most of the
    // window: OsHdr, luminances, primaries and the display name. Windows asks
    // DXGI (IDXGIOutput6::GetDesc1, matched by HMONITOR); elsewhere it leaves
    // OsHdr Unknown. `nativeWindow` is the GLFW window (null: the primary
    // display). Per-platform: Platform/<OS>/<OS>DisplayInfo.cpp.
    void QueryPlatformDisplayInfo(void* glfwWindow, DisplayCapabilities& caps);

    // Cheap per-frame check for a display change the swapchain must follow: the
    // window moved to another monitor, or the OS display configuration changed
    // (an HDR toggle; Windows reports it through IDXGIFactory::IsCurrent).
    // True once per change. Always false where there is no platform query.
    [[nodiscard]] bool PollPlatformDisplayChange(void* glfwWindow);

    // The numbers the tone map and the display encode read for one mode.
    struct DisplayMapping
    {
        DisplayOutputMode Mode = DisplayOutputMode::SDR;
        f32 PaperWhiteNits = 200.0f;
        f32 UIWhiteNits = 200.0f;
        f32 PeakNits = kFallbackPeakNits;
        f32 MinNits = 0.0f;

        [[nodiscard]] bool IsHdr() const noexcept
        {
            return Mode != DisplayOutputMode::SDR;
        }
    };

    // Resolve the effective luminances: peak = override, else the display's,
    // else kFallbackPeakNits; whites clamped to [kMinWhiteNits, kMaxWhiteNits]
    // and to the peak; black from the display, below the paper white.
    [[nodiscard]] DisplayMapping ResolveDisplayMapping(DisplayOutputMode mode, const DisplayOutputSettings& settings,
                                                       const DisplayCapabilities& caps) noexcept;

    // -------------------------------------------------------------------------
    // Transfer and gamut math -- mirrored term by term by
    // OloEditor/assets/shaders/include/DisplayEncode.glsl.
    // -------------------------------------------------------------------------

    // SMPTE ST 2084 inverse EOTF: absolute luminance in nits -> PQ signal [0, 1].
    [[nodiscard]] f32 PqEncodeNits(f32 nits) noexcept;
    // SMPTE ST 2084 EOTF: PQ signal [0, 1] -> nits.
    [[nodiscard]] f32 PqDecodeToNits(f32 signal) noexcept;

    // Linear Rec.709 -> linear Rec.2020, ITU-R BT.2087 (both D65).
    [[nodiscard]] glm::vec3 Rec709ToRec2020(const glm::vec3& rgb709) noexcept;

    // ITU-R BT.2390-3 EETF (section 5.4.1) mapping a PQ signal mastered for
    // [0, 10000] nits onto a display with [minNits, peakNits]. Identity below
    // the knee KS = 1.5 * maxLum - 0.5, a Hermite shoulder above it that
    // reaches exactly peakNits at 10000 nits, then the black-level lift.
    [[nodiscard]] f32 Bt2390Eetf(f32 pqSignal, f32 minNits, f32 peakNits) noexcept;

    // Step 2 for HDR: scene-referred linear Rec.709 (exposure applied; 1.0 =
    // reference white) -> display-referred linear Rec.709 in NITS. The EETF is
    // applied to max(R, G, B) and the colour scaled by the ratio, which keeps
    // the hue and guarantees no channel exceeds peakNits. clipOnly (the "None"
    // operator) clamps each channel to peakNits instead of rolling off.
    [[nodiscard]] glm::vec3 ToneMapForHdrDisplay(const glm::vec3& sceneLinear, const DisplayMapping& mapping,
                                                 bool clipOnly) noexcept;

    // Step 3: display nits -> composition (1.0 = UI white, gamma-encoded).
    [[nodiscard]] glm::vec3 NitsToComposition(const glm::vec3& nits, const DisplayMapping& mapping, f32 gamma) noexcept;

    // Step 4: composition -> the value the swapchain stores. SDR returns the
    // input unchanged; HDR10 returns the PQ signal of Rec.2020 nits; scRGB
    // returns linear Rec.709 in units of 80 nits.
    [[nodiscard]] glm::vec3 EncodeCompositionForDisplay(const glm::vec3& composition, const DisplayMapping& mapping,
                                                        f32 gamma) noexcept;

    // -------------------------------------------------------------------------
    // The process-global state: the authored settings, and what the presenting
    // backend selected. Main thread only, like Accessibility.
    // -------------------------------------------------------------------------
    class DisplayOutput
    {
      public:
        [[nodiscard]] static const DisplayOutputSettings& GetSettings() noexcept;
        // Sanitizes, then bumps the settings generation so the presenting
        // backend re-selects its swapchain mode at its next frame boundary.
        static void SetSettings(const DisplayOutputSettings& settings) noexcept;
        [[nodiscard]] static u64 GetSettingsGeneration() noexcept;

        // Published by the presenting backend after every swapchain (re)creation.
        static void PublishActive(const DisplayOutputSelection& selection, const DisplayCapabilities& caps);
        [[nodiscard]] static const DisplayOutputSelection& GetActiveSelection() noexcept;
        [[nodiscard]] static const DisplayCapabilities& GetActiveCapabilities() noexcept;
        // The mapping for the active mode and the current settings: what the
        // renderer uploads this frame.
        [[nodiscard]] static DisplayMapping GetActiveMapping() noexcept;

        // The gamma of the composition space: PostProcessSettings::Gamma, the
        // value the tone map encoded with. The renderer publishes it every
        // frame; the display encode decodes with it. Sanitized to [0.1, 5].
        static void SetCompositionGamma(f32 gamma) noexcept;
        [[nodiscard]] static f32 GetCompositionGamma() noexcept;

        // Tests and "reset to defaults": construction defaults, SDR active.
        static void Reset() noexcept;

        // --- Persistence: config/display.yaml beside config/renderer.yaml ---
        [[nodiscard]] static bool SaveToFile(const std::filesystem::path& path);
        // Leaves the current settings untouched (and returns false) on a missing
        // file, a parse error or a missing map.
        [[nodiscard]] static bool LoadFromFile(const std::filesystem::path& path);
        [[nodiscard]] static std::filesystem::path DefaultSettingsPath();
    };
} // namespace OloEngine
