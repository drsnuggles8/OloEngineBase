#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace OloEngine
{
    // @brief Colour-vision deficiency the presentation is adapted for (issue #458).
    //
    // The three dichromacies, in the order the literature names them by the
    // missing cone: L (long / red) = protan, M (medium / green) = deuteran,
    // S (short / blue) = tritan. `None` is the identity and disables the pass.
    enum class ColorBlindMode : i32
    {
        None = 0,
        Protanopia = 1,   // missing/impaired L cone — red-blind
        Deuteranopia = 2, // missing/impaired M cone — green-blind
        Tritanopia = 3    // missing/impaired S cone — blue-blind
    };

    // @brief What the colour-blind stage does with the mode above.
    //
    // `Correct` (daltonization) is the ACCESSIBILITY feature: it measures the
    // colour information the dichromat loses and redistributes it into channels
    // they can still separate, so two hues that would collapse to the same
    // percept stay distinguishable. `Simulate` is the AUTHORING tool: it shows a
    // trichromat what the dichromat sees, so art can be checked for
    // colour-only signalling. Both are driven by the same simulation matrices,
    // which is why they share one enum rather than living in two features.
    enum class ColorBlindAdaptation : i32
    {
        Correct = 0,
        Simulate = 1
    };

    // Bounds for the global UI text scale. The lower bound is not 0: a scale
    // that can reach zero makes every label vanish with no way back from inside
    // the game, which is an accessibility regression rather than a setting.
    inline constexpr f32 kMinUITextScale = 0.5f;
    inline constexpr f32 kMaxUITextScale = 3.0f;

    // The issue's "minimum legible size" requirement. Applied AFTER the scale
    // multiplier, so shrinking the global scale can never drive a label below
    // something readable, while an author who deliberately set a large font
    // keeps it. Expressed in the same screen-pixel unit as
    // UITextComponent::m_FontSize.
    //
    // The DEFAULT is 0 — the floor is off until a player asks for it. A non-zero
    // default would silently enlarge any existing label authored below it, and
    // because UILayoutSystem does not reflow on font size that label could then
    // overflow its rect. That is a layout change imposed on projects that never
    // opted into anything, which is exactly what this feature must not do.
    // kRecommendedMinimumFontSize is what the editor offers as a starting point.
    inline constexpr f32 kDefaultMinimumFontSize = 0.0f;
    inline constexpr f32 kRecommendedMinimumFontSize = 12.0f;
    inline constexpr f32 kMaxMinimumFontSize = 96.0f;

    // @brief Process-global accessibility settings (issue #458).
    //
    // Deliberately NOT a scene-level settings struct and NOT part of
    // PostProcessSettings, for three reasons:
    //
    //  * These are PLAYER preferences, not scene art direction. A scene-level
    //    home means loading a level authored by a trichromat silently resets a
    //    colour-blind player's mode — an accessibility bug that only shows up
    //    for the people the feature exists for.
    //  * A scene-level settings struct must be carried by Scene::Copy() or it
    //    is dropped the instant Play starts, invisibly to headless tests (see
    //    docs/agent-rules/scene-copy-must-carry-scene-level-settings.md). A
    //    process-global has no such touch-point.
    //  * QualityTiering rewrites PostProcessSettings fields (VignetteEnabled and
    //    friends) when the tier drops. Accessibility must survive a quality
    //    downgrade; staying out of that struct makes it structurally impossible
    //    to tier-gate by accident.
    //
    // Persistence is a small standalone YAML file (see AccessibilitySettings.cpp),
    // not scene YAML and not the save-game archive — same reasoning.
    struct AccessibilitySettings
    {
        // --- Subtitles / captions ---
        // Master toggle for the caption overlay (SubtitleSystem). Off by
        // default so existing projects render exactly as before.
        bool SubtitlesEnabled = false;
        // Show "Speaker: " ahead of the line. Separate from the master toggle
        // because speaker attribution is its own accessibility axis.
        bool SubtitleShowSpeaker = true;
        // Caption font size in screen pixels, BEFORE UITextScale is applied.
        f32 SubtitleFontSize = 26.0f;
        // Opacity of the letterbox behind the caption. Captions over a bright
        // frame are unreadable without a backing plate.
        f32 SubtitleBackgroundOpacity = 0.65f;

        // --- Global UI text scale ---
        // Multiplier applied to every UI text font size at RENDER time. The
        // authored UITextComponent::m_FontSize is never mutated, so toggling
        // the scale is lossless and scene data stays author-intent.
        f32 UITextScale = 1.0f;
        // Floor applied after the multiplier — 0 (off) by default, see
        // kDefaultMinimumFontSize for why.
        f32 MinimumFontSize = kDefaultMinimumFontSize;

        // --- Colour-vision deficiency ---
        ColorBlindMode ColorBlind = ColorBlindMode::None;
        ColorBlindAdaptation ColorBlindMethod = ColorBlindAdaptation::Correct;
        // 0 = no adaptation (identity), 1 = full dichromacy. Anomalous
        // trichromacy (the common, partial form) sits in between, which is why
        // this is a slider and not a checkbox.
        f32 ColorBlindSeverity = 1.0f;

        // NOTE: there is deliberately no ColorBlindGamma here. The stage decodes
        // and re-encodes around the DISPLAY gamma, and that value has exactly one
        // correct source: PostProcessSettings::Gamma, the number ToneMapPass just
        // encoded with. A second copy could only ever drift from it — and the
        // failure is silent (neutrals tint, the daltonization mis-weights, every
        // test stays green because they all use the default 2.2). The render pass
        // reads it from PostProcessSettings at upload time instead; see
        // MakeColorBlindUBOData.

        bool operator==(const AccessibilitySettings&) const = default;
    };

    // Clamp every field to a finite, sane range. Call after reading settings
    // from disk, per the CLAUDE.md rule that floats from external data are
    // validated with std::isfinite. An out-of-range enum falls back to the
    // safe default rather than saturating, because both enums are
    // discriminated values where a neighbouring case is a DIFFERENT valid
    // setting, not a nearby one.
    inline void SanitizeAccessibilitySettings(AccessibilitySettings& s) noexcept
    {
        const auto finite = [](f32 v, f32 fallback) noexcept
        { return std::isfinite(v) ? v : fallback; };

        s.SubtitleFontSize = std::clamp(finite(s.SubtitleFontSize, 26.0f), 8.0f, 200.0f);
        s.SubtitleBackgroundOpacity = std::clamp(finite(s.SubtitleBackgroundOpacity, 0.65f), 0.0f, 1.0f);

        s.UITextScale = std::clamp(finite(s.UITextScale, 1.0f), kMinUITextScale, kMaxUITextScale);
        s.MinimumFontSize = std::clamp(finite(s.MinimumFontSize, kDefaultMinimumFontSize), 0.0f, kMaxMinimumFontSize);

        if (const i32 mode = static_cast<i32>(s.ColorBlind);
            mode < static_cast<i32>(ColorBlindMode::None) || mode > static_cast<i32>(ColorBlindMode::Tritanopia))
        {
            s.ColorBlind = ColorBlindMode::None;
        }
        if (const i32 method = static_cast<i32>(s.ColorBlindMethod);
            method < static_cast<i32>(ColorBlindAdaptation::Correct) || method > static_cast<i32>(ColorBlindAdaptation::Simulate))
        {
            s.ColorBlindMethod = ColorBlindAdaptation::Correct;
        }

        s.ColorBlindSeverity = std::clamp(finite(s.ColorBlindSeverity, 1.0f), 0.0f, 1.0f);
    }

    // @brief Resolve an authored UI font size to the size actually rendered.
    //
    // The ONE place the text-scale policy lives, so the four UIRenderer draw
    // sites and the tests cannot disagree about it. Order matters: scale first,
    // then apply the legibility floor — flooring first would let the multiplier
    // drag the result back under the floor.
    //
    // A non-finite or non-positive authored size is passed through untouched:
    // that is malformed scene data, and silently promoting it to the floor
    // would draw text where the author asked for none.
    [[nodiscard]] inline f32 ResolveUIFontSize(f32 authoredFontSize, const AccessibilitySettings& s) noexcept
    {
        if (!std::isfinite(authoredFontSize) || authoredFontSize <= 0.0f)
            return authoredFontSize;

        const f32 scale = std::isfinite(s.UITextScale) ? s.UITextScale : 1.0f;
        const f32 floorSize = (std::isfinite(s.MinimumFontSize) && s.MinimumFontSize > 0.0f) ? s.MinimumFontSize : 0.0f;
        return std::max(authoredFontSize * scale, floorSize);
    }

    // @brief Process-global accessibility settings accessor.
    //
    // A static holder rather than a Scene member — see the struct comment. The
    // renderer, the UI renderer, the subtitle system and the editor panel all
    // read the same instance, so a change is visible everywhere on the next
    // frame with no propagation step to forget.
    //
    // NOT THREAD-SAFE, by design: Get()/Set() are unsynchronised. Every current
    // caller is on the game/main thread — the UIRenderer draw sites (main-thread
    // only, like Renderer3D::BeginScene/EndScene), the Subtitles scheduler node
    // (unmarked, so never dispatched to a worker), the editor panel, and
    // Application's startup load. The render path does not read the live global
    // at all: RenderPipeline::ConfigurePassesForFrame snapshots it into the pass.
    // If a worker ever needs these, add synchronisation here rather than
    // assuming the current call graph holds.
    class Accessibility
    {
      public:
        [[nodiscard]] static const AccessibilitySettings& Get() noexcept;

        // Assigns after sanitizing, so a caller can never install NaN/Inf or an
        // out-of-range enum.
        static void Set(const AccessibilitySettings& settings) noexcept;

        // Convenience for the hot path — UIRenderer calls this per draw.
        [[nodiscard]] static f32 ResolveFontSize(f32 authoredFontSize) noexcept
        {
            return ResolveUIFontSize(authoredFontSize, Get());
        }

        // Restore construction defaults. Used by tests and by "reset to
        // defaults" in the editor.
        static void Reset() noexcept;

        // --- Persistence ---
        // A standalone YAML file so accessibility preferences outlive any one
        // scene or save slot. Both return false (and log) on I/O or parse
        // failure; Load leaves the current settings untouched when it fails, so
        // a corrupt prefs file degrades to defaults rather than to garbage.
        [[nodiscard]] static bool SaveToFile(const std::filesystem::path& path);
        [[nodiscard]] static bool LoadFromFile(const std::filesystem::path& path);

        // Default prefs location, relative to the working directory. Matches
        // the engine's other user-facing files, which resolve against the
        // OloEditor/ working directory.
        [[nodiscard]] static std::filesystem::path DefaultSettingsPath();
    };

    // ------------------------------------------------------------------------
    // Colour-vision adaptation math — mirrored by PostProcess_ColorBlind.glsl
    // ------------------------------------------------------------------------
    //
    // Kept on the CPU so the contract is pinned WITHOUT a GL context
    // (ColorBlindMathTest), the same arrangement CAS/EASU/GTAO use. Every
    // matrix below is row-major in the mathematical sense and stored
    // column-major in glm; the helpers hide that so the shader and the tests
    // can be compared term by term.

    // Hunt-Pointer-Estevez style linear-RGB → LMS transform, in the
    // normalisation Viénot/Brettel-derived daltonization implementations use.
    [[nodiscard]] inline glm::mat3 LinearRGBToLMS() noexcept
    {
        // glm::mat3 columns; written transposed from the usual row form.
        return glm::mat3(
            glm::vec3(17.8824f, 3.45565f, 0.0299566f), // column 0 (R contribution)
            glm::vec3(43.5161f, 27.1554f, 0.184309f),  // column 1 (G contribution)
            glm::vec3(4.11935f, 3.86714f, 1.46709f));  // column 2 (B contribution)
    }

    [[nodiscard]] inline glm::mat3 LMSToLinearRGB() noexcept
    {
        return glm::mat3(
            glm::vec3(0.0809444479f, -0.0102485335f, -0.000365296938f),
            glm::vec3(-0.130504409f, 0.0540193266f, -0.00412161469f),
            glm::vec3(0.116721066f, -0.113614708f, 0.693511405f));
    }

    // The dichromat projection in LMS space: the missing cone's response is
    // reconstructed as a linear combination of the two remaining ones, which
    // collapses the 3D colour space onto the 2D surface the dichromat can
    // actually distinguish.
    [[nodiscard]] inline glm::mat3 DichromacyLMSProjection(ColorBlindMode mode) noexcept
    {
        switch (mode)
        {
            case ColorBlindMode::Protanopia:
                // L is reconstructed from M and S.
                return glm::mat3(
                    glm::vec3(0.0f, 0.0f, 0.0f),
                    glm::vec3(2.02344f, 1.0f, 0.0f),
                    glm::vec3(-2.52581f, 0.0f, 1.0f));
            case ColorBlindMode::Deuteranopia:
                // M is reconstructed from L and S.
                return glm::mat3(
                    glm::vec3(1.0f, 0.494207f, 0.0f),
                    glm::vec3(0.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 1.24827f, 1.0f));
            case ColorBlindMode::Tritanopia:
                // S is reconstructed from L and M.
                return glm::mat3(
                    glm::vec3(1.0f, 0.0f, -0.395913f),
                    glm::vec3(0.0f, 1.0f, 0.801109f),
                    glm::vec3(0.0f, 0.0f, 0.0f));
            case ColorBlindMode::None:
            default:
                return glm::mat3(1.0f);
        }
    }

    // Full simulation operator on LINEAR RGB, severity-interpolated toward
    // identity. severity 0 → identity (no adaptation), 1 → full dichromacy.
    [[nodiscard]] inline glm::mat3 ColorBlindSimulationMatrix(ColorBlindMode mode, f32 severity) noexcept
    {
        if (mode == ColorBlindMode::None)
            return glm::mat3(1.0f);

        const f32 t = std::clamp(std::isfinite(severity) ? severity : 1.0f, 0.0f, 1.0f);
        const glm::mat3 full = LMSToLinearRGB() * DichromacyLMSProjection(mode) * LinearRGBToLMS();
        return glm::mat3(1.0f) * (1.0f - t) + full * t;
    }

    // Where the daltonization error is redistributed. The lost signal is pushed
    // into the channel pairs the viewer CAN separate — for protan/deuteran that
    // is blue-vs-yellow, for tritan it is red-vs-green. Standard
    // Fidaner/Lin/Ozguven shift matrix.
    [[nodiscard]] inline glm::mat3 ColorBlindErrorShiftMatrix(ColorBlindMode mode) noexcept
    {
        switch (mode)
        {
            case ColorBlindMode::Protanopia:
            case ColorBlindMode::Deuteranopia:
                // Drop the (unrecoverable) red error, fold it into green+blue.
                return glm::mat3(
                    glm::vec3(0.0f, 0.7f, 0.7f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(0.0f, 0.0f, 1.0f));
            case ColorBlindMode::Tritanopia:
                // Drop the blue error, fold it into red+green.
                return glm::mat3(
                    glm::vec3(1.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(0.7f, 0.7f, 0.0f));
            case ColorBlindMode::None:
            default:
                return glm::mat3(0.0f);
        }
    }

    // @brief Adapt one LINEAR-RGB colour exactly as PostProcess_ColorBlind.glsl does.
    //
    // This is the CPU twin the shader is checked against. Keep the two in step:
    // ColorBlindMathTest compares term by term, and a divergence here is a
    // silently-wrong frame, not a compile error.
    [[nodiscard]] inline glm::vec3 AdaptColorLinear(const glm::vec3& linearColor,
                                                    ColorBlindMode mode,
                                                    ColorBlindAdaptation method,
                                                    f32 severity) noexcept
    {
        if (mode == ColorBlindMode::None)
            return linearColor;

        const glm::vec3 simulated = ColorBlindSimulationMatrix(mode, severity) * linearColor;
        if (method == ColorBlindAdaptation::Simulate)
            return simulated;

        // Daltonize: what the dichromat cannot see, expressed in channels they can.
        const glm::vec3 error = linearColor - simulated;
        return linearColor + ColorBlindErrorShiftMatrix(mode) * error;
    }

    // @brief GPU-side UBO layout for the colour-blind stage (std140, binding UBO_COLORBLIND).
    //
    // A single vec4 rather than four scalars so the std140 layout is trivially
    // 16-byte aligned and cannot drift out of step with the GLSL block. The
    // matrices are rebuilt in-shader from mode+severity (three mat3 multiplies
    // on a full-screen pass is free) rather than uploaded, so the shader and
    // AdaptColorLinear share one source of truth for the constants.
    struct ColorBlindUBOData
    {
        // x = ColorBlindMode as float, y = severity in [0,1],
        // z = ColorBlindAdaptation as float (0 = correct, 1 = simulate),
        // w = display gamma to decode/encode around.
        glm::vec4 Params = glm::vec4(0.0f, 1.0f, 0.0f, 2.2f);

        static constexpr u32 GetSize()
        {
            return sizeof(ColorBlindUBOData);
        }
    };

    static_assert(sizeof(ColorBlindUBOData) % 16 == 0, "ColorBlindUBOData must be 16-byte aligned for std140");
    static_assert(sizeof(ColorBlindUBOData) == 16, "ColorBlindUBOData std140 size drifted — update PostProcess_ColorBlind.glsl layout");

    // Build the UBO payload. Shared by the render pass and its tests so "what
    // reaches the GPU" has exactly one definition.
    //
    // `displayGamma` MUST be the same value ToneMapPass encoded with
    // (PostProcessSettings::Gamma) — it is threaded in rather than stored on
    // AccessibilitySettings precisely so the two cannot disagree. Clamped away
    // from zero because the shader divides by it.
    [[nodiscard]] inline ColorBlindUBOData MakeColorBlindUBOData(const AccessibilitySettings& s,
                                                                 f32 displayGamma) noexcept
    {
        ColorBlindUBOData data;
        data.Params = glm::vec4(static_cast<f32>(static_cast<i32>(s.ColorBlind)),
                                std::clamp(std::isfinite(s.ColorBlindSeverity) ? s.ColorBlindSeverity : 1.0f, 0.0f, 1.0f),
                                static_cast<f32>(static_cast<i32>(s.ColorBlindMethod)),
                                std::isfinite(displayGamma) ? std::clamp(displayGamma, 0.1f, 5.0f) : 2.2f);
        return data;
    }
} // namespace OloEngine
