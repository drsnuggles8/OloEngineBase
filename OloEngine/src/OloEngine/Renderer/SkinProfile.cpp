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
               Transmission == other.Transmission && Specular == other.Specular;
    }

    bool SkinProfile::SetParameters(const SkinProfileParameters& parameters)
    {
        m_Parameters = parameters;
        return m_Parameters.Sanitize();
    }

} // namespace OloEngine
