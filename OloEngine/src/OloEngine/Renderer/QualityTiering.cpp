#include "OloEnginePCH.h"
#include "QualityTiering.h"

#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine
{
    QualityTieringSettings QualityTieringSettings::HighDefaults()
    {
        return GetPresetSettings(QualityPreset::High);
    }

    QualityTieringSettings GetPresetSettings(QualityPreset preset)
    {
        OLO_PROFILE_FUNCTION();

        QualityTieringSettings s{};
        s.Preset = preset;

        switch (preset)
        {
            case QualityPreset::Low:
                s.ShadowResolution = 1024;
                s.ShadowSoftness = 0.0f;
                s.ShadowEnabled = false;
                s.SoftShadows = false; // hard PCF is cheapest
                s.AO = AOTechnique::None;
                s.SSAOSamples = 16;
                s.SSAORadius = 0.3f;
                s.SSAOBias = 0.025f;
                s.GTAODenoisePasses = 2;
                s.GTAORadius = 0.5f;
                s.GTAOPower = 2.2f;
                s.BloomEnabled = false;
                s.BloomIterations = 3;
                s.FXAAEnabled = false;
                s.DOFEnabled = false;
                s.MotionBlurEnabled = false;
                s.VignetteEnabled = false;
                s.ChromaticAberrationEnabled = false;
                s.DDGIEnabled = false; // realtime GI is the first cost to shed on Low
                s.DDGIBudgetScale = 0.5f;
                break;

            case QualityPreset::Medium:
                s.ShadowResolution = 2048;
                s.ShadowSoftness = 0.5f;
                s.ShadowEnabled = true;
                s.AO = AOTechnique::SSAO;
                s.SSAOSamples = 16;
                s.SSAORadius = 0.5f;
                s.SSAOBias = 0.025f;
                s.GTAODenoisePasses = 3;
                s.GTAORadius = 0.5f;
                s.GTAOPower = 2.2f;
                s.BloomEnabled = true;
                s.BloomIterations = 3;
                s.FXAAEnabled = false;
                s.DOFEnabled = false;
                s.MotionBlurEnabled = false;
                s.VignetteEnabled = false;
                s.ChromaticAberrationEnabled = false;
                s.DDGIEnabled = true;
                s.DDGIBudgetScale = 0.5f; // halved capture/relight budgets
                break;

            case QualityPreset::High:
                s.ShadowResolution = 4096;
                s.ShadowSoftness = 1.0f;
                s.ShadowEnabled = true;
                s.AO = AOTechnique::GTAO;
                s.SSAOSamples = 32;
                s.SSAORadius = 0.5f;
                s.SSAOBias = 0.025f;
                s.GTAODenoisePasses = 3;
                s.GTAORadius = 0.5f;
                s.GTAOPower = 2.2f;
                s.BloomEnabled = true;
                s.BloomIterations = 5;
                s.FXAAEnabled = true;
                s.DOFEnabled = false;
                s.MotionBlurEnabled = false;
                s.VignetteEnabled = false;
                s.ChromaticAberrationEnabled = false;
                break;

            case QualityPreset::Ultra:
                s.ShadowResolution = 4096;
                s.ShadowSoftness = 1.0f;
                s.ShadowEnabled = true;
                s.SoftShadows = true; // PCSS: contact-hardening penumbra, Ultra-only cost

                s.AO = AOTechnique::GTAO;
                s.SSAOSamples = 64;
                s.SSAORadius = 0.5f;
                s.SSAOBias = 0.025f;
                s.GTAODenoisePasses = 4;
                s.GTAORadius = 0.5f;
                s.GTAOPower = 2.2f;
                s.BloomEnabled = true;
                s.BloomIterations = 7;
                s.FXAAEnabled = true;
                s.DOFEnabled = true;
                s.MotionBlurEnabled = true;
                s.VignetteEnabled = false;
                s.ChromaticAberrationEnabled = true;
                s.DDGIEnabled = true;
                s.DDGIBudgetScale = 2.0f; // faster capture convergence on Ultra
                break;

            case QualityPreset::Custom:
            {
                // Custom returns High defaults with Preset preserved as Custom.
                auto settings = GetPresetSettings(QualityPreset::High);
                settings.Preset = QualityPreset::Custom;
                return settings;
            }
        }

        return s;
    }

    void CopyTierPPFields(const QualityTieringSettings& tiering, PostProcessSettings& pp)
    {
        // AO technique
        pp.ActiveAOTechnique = tiering.AO;
        pp.SSAOEnabled = (tiering.AO == AOTechnique::SSAO);
        pp.GTAOEnabled = (tiering.AO == AOTechnique::GTAO);
        pp.SSAOSamples = tiering.SSAOSamples;
        pp.SSAORadius = tiering.SSAORadius;
        pp.SSAOBias = tiering.SSAOBias;
        pp.GTAODenoisePasses = tiering.GTAODenoisePasses;
        pp.GTAORadius = tiering.GTAORadius;
        pp.GTAOPower = tiering.GTAOPower;

        // Post-process toggles
        pp.BloomEnabled = tiering.BloomEnabled;
        pp.BloomIterations = tiering.BloomIterations;
        pp.FXAAEnabled = tiering.FXAAEnabled;
        pp.DOFEnabled = tiering.DOFEnabled;
        pp.MotionBlurEnabled = tiering.MotionBlurEnabled;
        pp.VignetteEnabled = tiering.VignetteEnabled;
        pp.ChromaticAberrationEnabled = tiering.ChromaticAberrationEnabled;
    }

    void ApplyTieringToSettings(const QualityTieringSettings& tiering, PostProcessSettings& pp, ShadowSettings& shadow)
    {
        OLO_PROFILE_FUNCTION();

        // Shadows
        shadow.Resolution = tiering.ShadowResolution;
        shadow.Softness = tiering.ShadowSoftness;
        shadow.Enabled = tiering.ShadowEnabled;
        shadow.SoftShadows = tiering.SoftShadows;

        CopyTierPPFields(tiering, pp);
    }

    void ApplyTieringToRendererSettings(const QualityTieringSettings& tiering, RendererSettings& renderer)
    {
        OLO_PROFILE_FUNCTION();

        renderer.EnableDDGI = tiering.DDGIEnabled;
        renderer.DDGIBudgetScale = tiering.DDGIBudgetScale;
    }

    std::string_view QualityPresetToString(QualityPreset preset)
    {
        OLO_PROFILE_FUNCTION();

        switch (preset)
        {
            case QualityPreset::Low:
                return "Low";
            case QualityPreset::Medium:
                return "Medium";
            case QualityPreset::High:
                return "High";
            case QualityPreset::Ultra:
                return "Ultra";
            case QualityPreset::Custom:
                return "Custom";
        }
        OLO_CORE_WARN("QualityPresetToString: unrecognized preset value {}, defaulting to \"High\"",
                      static_cast<int>(preset));
        return "High";
    }

    QualityPreset QualityPresetFromString(std::string_view str)
    {
        OLO_PROFILE_FUNCTION();

        if (str == "Low")
            return QualityPreset::Low;
        if (str == "Medium")
            return QualityPreset::Medium;
        if (str == "High")
            return QualityPreset::High;
        if (str == "Ultra")
            return QualityPreset::Ultra;
        if (str == "Custom")
            return QualityPreset::Custom;
        OLO_CORE_WARN("QualityPresetFromString: unrecognized preset \"{}\", defaulting to High", str);
        return QualityPreset::High;
    }

    PostProcessSettings StripTieringOverlay(const PostProcessSettings& rendererPP, const PostProcessSettings& scenePP)
    {
        OLO_PROFILE_FUNCTION();

        // Start with the renderer's PP (has user edits to non-tier fields + tiering overlay).
        // Replace tier-owned fields with the scene's stored (un-tiered) values.
        PostProcessSettings result = rendererPP;

        // Build a pseudo-tiering struct from the scene's PP to reuse the shared mapping.
        QualityTieringSettings sceneTier{};
        sceneTier.AO = scenePP.ActiveAOTechnique;
        sceneTier.SSAOSamples = scenePP.SSAOSamples;
        sceneTier.SSAORadius = scenePP.SSAORadius;
        sceneTier.SSAOBias = scenePP.SSAOBias;
        sceneTier.GTAODenoisePasses = scenePP.GTAODenoisePasses;
        sceneTier.GTAORadius = scenePP.GTAORadius;
        sceneTier.GTAOPower = scenePP.GTAOPower;
        sceneTier.BloomEnabled = scenePP.BloomEnabled;
        sceneTier.BloomIterations = scenePP.BloomIterations;
        sceneTier.FXAAEnabled = scenePP.FXAAEnabled;
        sceneTier.DOFEnabled = scenePP.DOFEnabled;
        sceneTier.MotionBlurEnabled = scenePP.MotionBlurEnabled;
        sceneTier.VignetteEnabled = scenePP.VignetteEnabled;
        sceneTier.ChromaticAberrationEnabled = scenePP.ChromaticAberrationEnabled;

        CopyTierPPFields(sceneTier, result);
        return result;
    }

} // namespace OloEngine
