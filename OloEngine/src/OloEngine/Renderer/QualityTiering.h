#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <string_view>

namespace OloEngine
{
    enum class QualityPreset : u8
    {
        Low,
        Medium,
        High,
        Ultra,
        Custom
    };

    struct QualityTieringSettings
    {
        QualityPreset Preset = QualityPreset::High;

        // Shadow
        u32 ShadowResolution = 4096;
        f32 ShadowSoftness = 1.0f;
        bool ShadowEnabled = true;
        // Percentage-Closer Soft Shadows (contact-hardening variable penumbra).
        // Ultra-tier only — PCSS measured as the single largest scene-pass cost
        // (SponzaCSM A/B, 2026-07-02), so Low/Medium/High use hardware PCF.
        bool SoftShadows = false;

        // AO
        AOTechnique AO = AOTechnique::GTAO;
        i32 SSAOSamples = 32;
        f32 SSAORadius = 0.5f;
        f32 SSAOBias = 0.025f;
        i32 GTAODenoisePasses = 3;
        f32 GTAORadius = 0.5f;
        f32 GTAOPower = 2.2f;

        // Post-process toggles
        bool BloomEnabled = true;
        i32 BloomIterations = 5;
        bool FXAAEnabled = true;
        bool DOFEnabled = false;
        bool MotionBlurEnabled = false;
        bool VignetteEnabled = false;
        bool ChromaticAberrationEnabled = false;

        // Realtime DDGI (issue #632). Applied to RendererSettings via
        // ApplyTieringToRendererSettings — one-directional (not part of the
        // per-scene PP overlay StripTieringOverlay round-trips), since
        // RendererSettings::EnableDDGI/DDGIBudgetScale are runtime knobs.
        bool DDGIEnabled = true;
        f32 DDGIBudgetScale = 1.0f;

        // Canonical High-preset defaults (single source of truth for struct initializers).
        [[nodiscard]] static QualityTieringSettings HighDefaults();
    };

    // Return the canonical preset values for a named tier (Custom returns High defaults).
    [[nodiscard]] QualityTieringSettings GetPresetSettings(QualityPreset preset);

    // Write tiering values into the runtime structs.
    void ApplyTieringToSettings(const QualityTieringSettings& tiering, PostProcessSettings& pp, ShadowSettings& shadow);

    // Write the tier's realtime-DDGI knobs into RendererSettings (issue #632).
    // Separate from ApplyTieringToSettings because RendererSettings is not part
    // of the per-scene PP overlay round-trip — call it alongside at apply sites.
    struct RendererSettings;
    void ApplyTieringToRendererSettings(const QualityTieringSettings& tiering, RendererSettings& renderer);

    // Copy tier-owned PP fields from tiering into pp (single mapping used by Apply and Strip).
    void CopyTierPPFields(const QualityTieringSettings& tiering, PostProcessSettings& pp);

    // Build a saveable PP by taking user-edited non-tier fields from rendererPP
    // while preserving tier-owned fields from scenePP (the un-tiered original).
    [[nodiscard]] PostProcessSettings StripTieringOverlay(const PostProcessSettings& rendererPP, const PostProcessSettings& scenePP);

    // String conversions
    [[nodiscard]] std::string_view QualityPresetToString(QualityPreset preset);
    [[nodiscard]] QualityPreset QualityPresetFromString(std::string_view str);

} // namespace OloEngine
