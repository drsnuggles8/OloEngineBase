#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // One channel of a linear Rec.709 colour. Non-finite is replaced by the
        // default rather than clamped: NaN compares false against every bound,
        // so `std::clamp` would pass it straight through.
        [[nodiscard]] bool SanitizeUnitChannel(f32& value, f32 fallback)
        {
            if (!std::isfinite(value))
            {
                value = fallback;
                return false;
            }
            const f32 clamped = std::clamp(value, 0.0f, 1.0f);
            if (clamped != value)
            {
                value = clamped;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool SanitizeUnitColor(glm::vec3& color, const glm::vec3& fallback)
        {
            bool ok = SanitizeUnitChannel(color.x, fallback.x);
            ok = SanitizeUnitChannel(color.y, fallback.y) && ok;
            ok = SanitizeUnitChannel(color.z, fallback.z) && ok;
            return ok;
        }

        [[nodiscard]] bool SanitizeRange(f32& value, f32 minValue, f32 maxValue, f32 fallback)
        {
            if (!std::isfinite(value))
            {
                value = fallback;
                return false;
            }
            const f32 clamped = std::clamp(value, minValue, maxValue);
            if (clamped != value)
            {
                value = clamped;
                return false;
            }
            return true;
        }
    } // namespace

    bool SkinTransmissionParameters::Sanitize()
    {
        const SkinTransmissionParameters defaults{};
        bool ok = true;

        ok = SanitizeRange(Strength, kMinSkinTransmissionStrength, kMaxSkinTransmissionStrength, defaults.Strength) && ok;
        ok = SanitizeRange(Anisotropy, kMinSkinTransmissionAnisotropy, kMaxSkinTransmissionAnisotropy,
                           defaults.Anisotropy) &&
             ok;
        ok = SanitizeRange(Power, kMinSkinTransmissionPower, kMaxSkinTransmissionPower, defaults.Power) && ok;

        return ok;
    }

    bool SkinTransmissionParameters::operator==(const SkinTransmissionParameters& other) const noexcept
    {
        // Bit-exact, for the reason SkinProfileParameters::operator== states.
        return Math::BitwiseEqual(Strength, other.Strength) && Math::BitwiseEqual(Anisotropy, other.Anisotropy) &&
               Math::BitwiseEqual(Power, other.Power);
    }

    bool SkinSpecularParameters::Sanitize()
    {
        const SkinSpecularParameters defaults{};
        bool ok = true;

        ok = SanitizeRange(LobeMix, kMinSkinLobeMix, kMaxSkinLobeMix, defaults.LobeMix) && ok;
        ok = SanitizeRange(LobeRoughnessScale, kMinSkinLobeRoughnessScale, kMaxSkinLobeRoughnessScale,
                           defaults.LobeRoughnessScale) &&
             ok;
        ok = SanitizeRange(NormalVarianceStrength, kMinSkinNormalVarianceStrength,
                           kMaxSkinNormalVarianceStrength, defaults.NormalVarianceStrength) &&
             ok;
        ok = SanitizeRange(DetailStrength, kMinSkinDetailStrength, kMaxSkinDetailStrength,
                           defaults.DetailStrength) &&
             ok;
        ok = SanitizeRange(ExpressionDetailGain, kMinSkinDetailStrength, kMaxSkinDetailStrength,
                           defaults.ExpressionDetailGain) &&
             ok;

        return ok;
    }

    bool SkinSpecularParameters::operator==(const SkinSpecularParameters& other) const noexcept
    {
        // Bit-exact, for the reason SkinProfileParameters::operator== states.
        return Math::BitwiseEqual(LobeMix, other.LobeMix) &&
               Math::BitwiseEqual(LobeRoughnessScale, other.LobeRoughnessScale) &&
               Math::BitwiseEqual(NormalVarianceStrength, other.NormalVarianceStrength) &&
               Math::BitwiseEqual(DetailStrength, other.DetailStrength) &&
               Math::BitwiseEqual(ExpressionDetailGain, other.ExpressionDetailGain);
    }

    bool SkinOralParameters::Sanitize()
    {
        const SkinOralParameters defaults{};
        bool ok = true;

        ok = SanitizeRange(CoatStrength, kMinSkinOralCoatStrength, kMaxSkinOralCoatStrength,
                           defaults.CoatStrength) &&
             ok;
        ok = SanitizeRange(CoatRoughness, kMinSkinOralCoatRoughness, kMaxSkinOralCoatRoughness,
                           defaults.CoatRoughness) &&
             ok;
        ok = SanitizeRange(CoatIor, kMinSkinOralCoatIor, kMaxSkinOralCoatIor, defaults.CoatIor) && ok;
        ok = SanitizeRange(CavityOcclusion, kMinSkinOralCavityOcclusion, kMaxSkinOralCavityOcclusion,
                           defaults.CavityOcclusion) &&
             ok;

        return ok;
    }

    bool SkinOralParameters::operator==(const SkinOralParameters& other) const noexcept
    {
        // Bit-exact, for the reason SkinProfileParameters::operator== states.
        return Math::BitwiseEqual(CoatStrength, other.CoatStrength) &&
               Math::BitwiseEqual(CoatRoughness, other.CoatRoughness) &&
               Math::BitwiseEqual(CoatIor, other.CoatIor) &&
               Math::BitwiseEqual(CavityOcclusion, other.CavityOcclusion);
    }

    bool SkinOcularParameters::Sanitize()
    {
        const SkinOcularParameters defaults{};
        bool ok = true;

        ok = SanitizeRange(OcularStrength, 0.0f, 1.0f, defaults.OcularStrength) && ok;
        ok = SanitizeRange(RefractionStrength, 0.0f, 1.0f, defaults.RefractionStrength) && ok;

        ok = SanitizeRange(EyeRadiusMM, kMinSkinEyeRadiusMM, kMaxSkinEyeRadiusMM, defaults.EyeRadiusMM) && ok;
        ok = SanitizeRange(CorneaRadiusMM, kMinSkinCorneaRadiusMM, kMaxSkinCorneaRadiusMM,
                           defaults.CorneaRadiusMM) &&
             ok;
        ok = SanitizeRange(IrisRadiusMM, kMinSkinIrisRadiusMM, kMaxSkinIrisRadiusMM, defaults.IrisRadiusMM) && ok;
        ok = SanitizeRange(PupilRadiusMM, kMinSkinPupilRadiusMM, kMaxSkinPupilRadiusMM,
                           defaults.PupilRadiusMM) &&
             ok;
        ok = SanitizeRange(IrisPlaneDepthMM, kMinSkinIrisPlaneDepthMM, kMaxSkinIrisPlaneDepthMM,
                           defaults.IrisPlaneDepthMM) &&
             ok;
        ok = SanitizeRange(CorneaIor, kMinSkinCorneaIor, kMaxSkinCorneaIor, defaults.CorneaIor) && ok;

        ok = SanitizeRange(LimbalRingWidthMM, kMinSkinLimbalRingWidthMM, kMaxSkinLimbalRingWidthMM,
                           defaults.LimbalRingWidthMM) &&
             ok;
        ok = SanitizeRange(LimbalRingStrength, kMinSkinLimbalRingStrength, kMaxSkinLimbalRingStrength,
                           defaults.LimbalRingStrength) &&
             ok;
        ok = SanitizeRange(PupilDarkening, 0.0f, 1.0f, defaults.PupilDarkening) && ok;
        ok = SanitizeRange(IrisConcavity, kMinSkinIrisConcavity, kMaxSkinIrisConcavity,
                           defaults.IrisConcavity) &&
             ok;
        ok = SanitizeUnitColor(IrisColor, defaults.IrisColor) && ok;

        // THE ONE CROSS-FIELD RULE IN THIS STRUCT, and it is here rather than
        // in the lane packers because a lane packer cannot report. Three of the
        // lengths are RATIOS in disguise and each has a length it must not
        // exceed:
        //
        //   * an iris wider than the globe puts the limbus past the equator,
        //     where sin(limbus) leaves [0, 1] and the cosine goes imaginary;
        //   * a pupil wider than the iris leaves no iris;
        //   * a cornea FLATTER than the globe is not an eye, and its curvature
        //     ratio would invert the normal bend.
        //
        // Clamped to the bounding length rather than to the default, because
        // the author's intent in every one of these cases is "as large as it
        // can be" and snapping to 5.85 mm when they typed 20 would be a
        // different kind of wrong. Reported through the return value either way.
        if (IrisRadiusMM > EyeRadiusMM)
        {
            IrisRadiusMM = EyeRadiusMM;
            ok = false;
        }
        if (PupilRadiusMM > IrisRadiusMM)
        {
            PupilRadiusMM = IrisRadiusMM;
            ok = false;
        }
        if (CorneaRadiusMM > EyeRadiusMM)
        {
            CorneaRadiusMM = EyeRadiusMM;
            ok = false;
        }

        return ok;
    }

    bool SkinOcularParameters::operator==(const SkinOcularParameters& other) const noexcept
    {
        // Bit-exact, for the reason SkinProfileParameters::operator== states.
        return Math::BitwiseEqual(OcularStrength, other.OcularStrength) &&
               Math::BitwiseEqual(RefractionStrength, other.RefractionStrength) &&
               Math::BitwiseEqual(EyeRadiusMM, other.EyeRadiusMM) &&
               Math::BitwiseEqual(CorneaRadiusMM, other.CorneaRadiusMM) &&
               Math::BitwiseEqual(IrisRadiusMM, other.IrisRadiusMM) &&
               Math::BitwiseEqual(PupilRadiusMM, other.PupilRadiusMM) &&
               Math::BitwiseEqual(IrisPlaneDepthMM, other.IrisPlaneDepthMM) &&
               Math::BitwiseEqual(CorneaIor, other.CorneaIor) &&
               Math::BitwiseEqual(LimbalRingWidthMM, other.LimbalRingWidthMM) &&
               Math::BitwiseEqual(LimbalRingStrength, other.LimbalRingStrength) &&
               Math::BitwiseEqual(PupilDarkening, other.PupilDarkening) &&
               Math::BitwiseEqual(IrisConcavity, other.IrisConcavity) &&
               Math::BitwiseEqual(IrisColor, other.IrisColor);
    }

    bool SkinProfileParameters::Sanitize()
    {
        const SkinProfileParameters defaults{};
        bool ok = true;

        if (!IsValidSkinEvaluationModel(static_cast<i32>(EvaluationModel)))
        {
            OLO_CORE_WARN("SkinProfile - evaluation model {} is out of range; falling back to {}.",
                          static_cast<i32>(EvaluationModel), ToString(defaults.EvaluationModel));
            EvaluationModel = defaults.EvaluationModel;
            ok = false;
        }

        ok = SanitizeUnitColor(ScatterColor, defaults.ScatterColor) && ok;
        ok = SanitizeUnitColor(SpecularTint, defaults.SpecularTint) && ok;

        ok = SanitizeRange(ScatterRadiusMM.x, kMinSkinScatterRadiusMM, kMaxSkinScatterRadiusMM, defaults.ScatterRadiusMM.x) && ok;
        ok = SanitizeRange(ScatterRadiusMM.y, kMinSkinScatterRadiusMM, kMaxSkinScatterRadiusMM, defaults.ScatterRadiusMM.y) && ok;
        ok = SanitizeRange(ScatterRadiusMM.z, kMinSkinScatterRadiusMM, kMaxSkinScatterRadiusMM, defaults.ScatterRadiusMM.z) && ok;

        ok = SanitizeRange(ThicknessScale, kMinSkinThicknessScale, kMaxSkinThicknessScale, defaults.ThicknessScale) && ok;

        // The transmission lobe (issue #1242). Routed through the profile's
        // Sanitize rather than validated by its own callers, because this is the
        // ONE validation gate this header promises: the YAML reader, the
        // asset-pack reader, the save-game reader and the editor inspector all
        // reach the transmission fields through here and nowhere else.
        ok = Transmission.Sanitize() && ok;

        // The layered surface response (issue #1243), through the same one gate
        // and for the same reason.
        ok = Specular.Sanitize() && ok;

        // The wet coat and the cavity weight (issue #1245), through the same one
        // gate and for the same reason.
        ok = Oral.Sanitize() && ok;

        // The cornea, the iris and the tear line (issue #1244), through the same
        // one gate and for the same reason.
        ok = Ocular.Sanitize() && ok;

        return ok;
    }

    bool SkinProfileParameters::operator==(const SkinProfileParameters& other) const noexcept
    {
        // Bit-exact comparison, never `==` on the glm vectors directly
        // (CLAUDE.md → Conventions). The question this answers is "is this the
        // same authored record?", which is an identity question, not a
        // tolerance one.
        return EvaluationModel == other.EvaluationModel &&
               Math::BitwiseEqual(ScatterColor, other.ScatterColor) &&
               Math::BitwiseEqual(ScatterRadiusMM, other.ScatterRadiusMM) &&
               Math::BitwiseEqual(SpecularTint, other.SpecularTint) &&
               Math::BitwiseEqual(ThicknessScale, other.ThicknessScale) &&
               Transmission == other.Transmission && Specular == other.Specular &&
               Oral == other.Oral && Ocular == other.Ocular;
    }

    bool SkinProfile::SetParameters(const SkinProfileParameters& parameters)
    {
        m_Parameters = parameters;
        return m_Parameters.Sanitize();
    }

} // namespace OloEngine
