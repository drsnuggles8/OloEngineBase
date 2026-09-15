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
               Math::BitwiseEqual(ThicknessScale, other.ThicknessScale);
    }

    bool SkinProfile::SetParameters(const SkinProfileParameters& parameters)
    {
        m_Parameters = parameters;
        return m_Parameters.Sanitize();
    }

} // namespace OloEngine
