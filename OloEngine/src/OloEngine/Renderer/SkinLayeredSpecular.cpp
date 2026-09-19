#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{

    f32 SkinFilteredAlpha(f32 alpha, f32 dNdxLengthSq, f32 dNdyLengthSq, f32 varianceStrength) noexcept
    {
        // The expression include/SkinLayeredSpecular.glsl transcribes, in this
        // order. Reassociating it would make the parity test's float comparison
        // a tolerance argument instead of an equality one.
        const f32 variance = varianceStrength * (dNdxLengthSq + dNdyLengthSq);
        const f32 kernel = std::min(2.0f * variance, kSkinVarianceKernelClamp);

        // NOT max(kernel, 0): a negative kernel cannot arise from a sanitized
        // strength and two squared lengths, and adding a guard here that the
        // shader does not have is exactly the asymmetry the parity test exists
        // to catch. The strength is held non-negative by Sanitize.
        const f32 filtered = std::sqrt(alpha * alpha + kernel);

        // The clamp IS in the shader too. alpha is the GGX alpha and above 1 the
        // NDF stops being normalizable, so this is a domain bound rather than a
        // taste one.
        return std::clamp(filtered, 0.0f, 1.0f);
    }

    SkinSpecularLobePair SkinSpecularLobesFor(f32 filteredRoughness,
                                              const SkinSpecularParameters& parameters) noexcept
    {
        SkinSpecularLobePair lobes;
        lobes.NarrowRoughness = filteredRoughness;
        // Clamped at 1 rather than allowed to run past it: roughness > 1 squares
        // to an alpha outside the NDF's domain. The clamp is also why
        // kMaxSkinLobeRoughnessScale can be a round 4 without anybody having to
        // reason about what 4x a rough skin means.
        //
        // Mirrors oloSkinBroadRoughness in include/SkinLayeredSpecular.glsl,
        // operation for operation.
        lobes.BroadRoughness = std::clamp(filteredRoughness * parameters.LobeRoughnessScale, 0.0f, 1.0f);
        lobes.BroadWeight = parameters.LobeMix;
        return lobes;
    }

    f32 SkinSpecularMix(f32 narrow, f32 broad, f32 broadWeight) noexcept
    {
        // THE convex combination. Written as `narrow + w * (broad - narrow)`
        // rather than `(1 - w) * narrow + w * broad` for one reason that is
        // worth a line: at w == 0 this returns `narrow` EXACTLY, with no
        // rounding, because `narrow + 0 * x` is `narrow` in IEEE 754 for every
        // finite narrow. The other spelling computes `1.0f - 0.0f` and
        // multiplies, which is also exact here — but stops being exact the
        // moment anyone refactors w into a value computed from a lerp.
        //
        // That exactness is what makes the A/B control the acceptance criteria
        // ask for a true identity: LobeMix 0 is not "almost the old frame", it
        // is the old frame, bit for bit, and a golden image can say so.
        return narrow + broadWeight * (broad - narrow);
    }

    glm::vec3 SkinSpecularMix(const glm::vec3& narrow, const glm::vec3& broad, f32 broadWeight) noexcept
    {
        return glm::vec3(SkinSpecularMix(narrow.x, broad.x, broadWeight),
                         SkinSpecularMix(narrow.y, broad.y, broadWeight),
                         SkinSpecularMix(narrow.z, broad.z, broadWeight));
    }

    f32 SkinExpressionDetailWeight(std::span<const f32> appliedWeights) noexcept
    {
        f32 total = 0.0f;
        for (const f32 weight : appliedWeights)
        {
            // SKIPPED, not propagated. MorphTargetComponent::SetWeight rejects
            // non-finite weights at the setter, so one arriving here means a
            // path that bypassed it — and a NaN would take this material's whole
            // specular term with it, on every pixel, for as long as the
            // expression is held.
            if (!std::isfinite(weight))
                continue;
            total += std::abs(weight);
        }

        // Clamped rather than normalized by the count. Dividing by the number of
        // targets would make the SAME expression score differently on two rigs
        // that happen to expose different numbers of unused blend shapes, which
        // is a property of the export and not of the face.
        return std::clamp(total, 0.0f, 1.0f);
    }

    f32 SkinDetailStrength(const SkinSpecularParameters& parameters, f32 expressionWeight) noexcept
    {
        if (!std::isfinite(expressionWeight))
            return parameters.DetailStrength;

        const f32 weight = std::clamp(expressionWeight, 0.0f, 1.0f);
        const f32 strength = parameters.DetailStrength + parameters.ExpressionDetailGain * weight;
        // The SUM is clamped, not each term: two individually legal fields can
        // add to a value neither of them could hold, and -1 in particular has to
        // stay a hard floor because below it the "detail" is re-added with the
        // sign flipped — a surface whose pores are bumps.
        return std::clamp(strength, kMinSkinDetailStrength, kMaxSkinDetailStrength);
    }

    glm::vec4 SkinSpecularLane(const SkinProfileParameters& parameters) noexcept
    {
        return glm::vec4(parameters.Specular.LobeMix,
                         parameters.Specular.LobeRoughnessScale,
                         parameters.Specular.NormalVarianceStrength,
                         0.0f);
    }

} // namespace OloEngine
