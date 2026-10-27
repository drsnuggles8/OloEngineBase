#include "OloEnginePCH.h"
#include "QualityTiering.h"

#include "OloEngine/Core/CVar.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <array>
#include <format>
#include <optional>
#include <string>

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
                // ON, and tightest, on the weakest tier. DDGI above is turned
                // OFF on Low because realtime GI is a whole feature to shed;
                // the population budget is the opposite kind of knob — it is
                // what makes a herd affordable at all, so the low tier is the
                // one that needs it most and the small allowance is the point.
                s.AnimalSchedulingEnabled = true;
                s.AnimalFrameBudgetUnits = 2000.0f;
                // A coat sheds its strands at twice the apparent size.
                s.GroomDetailBias = 0.5f;
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
                s.AnimalSchedulingEnabled = true;
                s.AnimalFrameBudgetUnits = 4000.0f;
                s.GroomDetailBias = 0.75f;
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
                s.AnimalSchedulingEnabled = true;
                s.AnimalFrameBudgetUnits = 6000.0f;
                s.GroomDetailBias = 1.0f; // every coat as authored
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
                s.AnimalSchedulingEnabled = true;
                s.AnimalFrameBudgetUnits = 12000.0f;
                // Strands down to two thirds of the authored hand-over size.
                s.GroomDetailBias = 1.5f;
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
        // A scene that does not author an AO technique follows the active
        // quality tier, like every other tier-owned field here; that is what
        // the tier is for, not a compatibility path (decided on #1502). A scene
        // with an authored selection (including None) must retain it:
        // otherwise loading it immediately re-enables the tier's AO pass.
        if (!pp.m_AOTechniqueOverride)
        {
            pp.ActiveAOTechnique = tiering.AO;
            pp.SSAOEnabled = (tiering.AO == AOTechnique::SSAO);
            pp.GTAOEnabled = (tiering.AO == AOTechnique::GTAO);
        }
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
        renderer.AnimalSchedulingEnabled = tiering.AnimalSchedulingEnabled;
        renderer.AnimalFrameBudgetUnits = tiering.AnimalFrameBudgetUnits;
        renderer.GroomDetailBias = tiering.GroomDetailBias;
    }

    namespace
    {
        // A whole-number RendererSettings field behind a console variable. The
        // registry hands the binding an opaque context; this row is it.
        struct GroomCountCVar
        {
            std::string_view Name;
            std::string_view Help;
            u32 RendererSettings::* Field;
            u32 Max;
        };

        const std::array<GroomCountCVar, 2> kGroomCountCVars{ {
            { "OLO_GROOM_LOD_CROSSFADE_FRAMES",
              "Frames a groom's LOD hand-over is dithered across (#1612): both tiers drawn through complementary "
              "per-pixel masks while the incoming tier's share ramps up. 0 is a hard switch; range [0, 120].",
              &RendererSettings::GroomLodCrossFadeFrames, 120u },
            { "OLO_GROOM_LOD_MAX_CROSSFADES",
              "Groom coats that may cross-fade at once (#1612), which bounds what a transition frame costs. A "
              "hand-over past it hard-switches and is reported; range [0, 64].",
              &RendererSettings::GroomLodMaxCrossFades, 64u },
        } };

        void RegisterGroomCountCVar(const GroomCountCVar& row)
        {
            CVars::CVarBinding binding;
            binding.Name = row.Name;
            binding.Help = row.Help;
            binding.Type = CVars::CVarType::Int;
            binding.Context = const_cast<GroomCountCVar*>(&row);
            binding.Render = [](void* context)
            {
                const auto* self = static_cast<const GroomCountCVar*>(context);
                return std::to_string(Renderer3D::GetRendererSettings().*(self->Field));
            };
            binding.IsDefault = [](void* context)
            {
                const auto* self = static_cast<const GroomCountCVar*>(context);
                return Renderer3D::GetRendererSettings().*(self->Field) == RendererSettings{}.*(self->Field);
            };
            binding.Parse = [](void* context, std::string_view raw, std::string& error)
            {
                const auto* self = static_cast<const GroomCountCVar*>(context);
                const std::optional<i64> parsed = CVars::ParseIntText(raw);
                if (!parsed || *parsed < 0 || *parsed > static_cast<i64>(self->Max))
                {
                    error = std::format("expected a whole number in [0, {}]", self->Max);
                    return false;
                }
                Renderer3D::GetRendererSettings().*(self->Field) = static_cast<u32>(*parsed);
                return true;
            };
            (void)CVars::Register(binding);
        }
    } // namespace

    void RegisterGroomCVars()
    {
        static const bool s_CountsRegistered = []
        {
            for (const GroomCountCVar& row : kGroomCountCVars)
            {
                RegisterGroomCountCVar(row);
            }
            return true;
        }();
        (void)s_CountsRegistered;

        static const CVars::CVarHandle s_Handle = []
        {
            CVars::CVarBinding binding;
            binding.Name = "OLO_GROOM_DETAIL_BIAS";
            binding.Help = "Global groom detail bias (#1616): divides every pixel threshold a groom's detail is "
                           "chosen by (GroomLodComponent tiers and budgets, the coat shadow LOD, the ray-traced "
                           "proxy tier). 2 keeps strands to half the apparent size, 0.5 hands over at twice it; "
                           "1 is the authored behaviour. The quality preset writes it; range [0.25, 4].";
            binding.Type = CVars::CVarType::Float;
            // Shortest round-trip, CVar<f32>'s rendering: the registry decides
            // a change happened by comparing these strings.
            binding.Render = [](void*)
            { return std::format("{}", Renderer3D::GetRendererSettings().GroomDetailBias); };
            binding.IsDefault = [](void*)
            { return Math::BitwiseEqual(Renderer3D::GetRendererSettings().GroomDetailBias, 1.0f); };
            binding.Parse = [](void*, std::string_view raw, std::string& error)
            {
                const std::string buffer(CVars::TrimText(raw));
                const std::optional<f32> parsed =
                    Levers::ParseNumberLever(buffer.c_str(), kGroomDetailBiasMin, kGroomDetailBiasMax);
                if (!parsed)
                {
                    error = std::format("expected a finite number in [{}, {}]", kGroomDetailBiasMin,
                                        kGroomDetailBiasMax);
                    return false;
                }
                Renderer3D::GetRendererSettings().GroomDetailBias = *parsed;
                return true;
            };
            return CVars::Register(binding);
        }();
        (void)s_Handle;
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
        // In particular, an AO selector change in the panel marks the renderer
        // settings as authored, so CopyTierPPFields deliberately preserves it.
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
