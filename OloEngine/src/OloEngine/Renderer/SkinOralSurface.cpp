#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinOralSurface.h"

#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{

    namespace
    {
        // The two constants include/SkinOralSurface.glsl shares with
        // include/PBRCommon.glsl, repeated here rather than included so this
        // file has no dependency on the shader tree. ShaderUnit_SkinOralSurface
        // asserts the two sides agree about both.
        constexpr f32 kEpsilon = 1.0e-4f; // PBRCommon's EPSILON
        constexpr f32 kPi = 3.14159265358979323846f;

        // The UNCLAMPED GGX, transcribed from distributionGGXUnclampedNH in
        // include/PBRCommon.glsl (the ClosureV2 NDF), in its cross-product form
        // and with the same smallest-normal floor. The Legacy distributionGGX
        // caps D below roughness ~0.27, which starved the coat's lobe to
        // 0.5 % of its energy at a saliva film's 0.06 (issue #1421).
        [[nodiscard]] f32 DistributionGGXUnclamped(const glm::vec3& normal, const glm::vec3& half, f32 roughness) noexcept
        {
            const f32 a = roughness * roughness;
            const f32 a2 = a * a;
            const f32 c = glm::dot(normal, half);
            if (c <= 0.0f)
                return a2 * (1.0f / kPi);
            const glm::vec3 t = glm::cross(normal, half);
            f32 denom = glm::dot(t, t) + a2 * c * c;
            denom = kPi * denom * denom;
            return a2 / std::max(denom, 1.17549435e-38f);
        }

        // Height-correlated Smith visibility, transcribed from
        // visibilitySmithGGXCorrelated in include/PBRCommon.glsl. Note this is
        // the VISIBILITY, i.e. G / (4 NdotV NdotL) — so the caller multiplies
        // D * Vis * F and does NOT divide by 4 NdotV NdotL again.
        [[nodiscard]] f32 VisibilitySmithGGXCorrelated(const glm::vec3& normal, const glm::vec3& view,
                                                       const glm::vec3& lightDir, f32 roughness) noexcept
        {
            const f32 NdotV = std::max(glm::dot(normal, view), 0.0f);
            const f32 NdotL = std::max(glm::dot(normal, lightDir), 0.0f);

            const f32 alpha = roughness * roughness;
            const f32 a2 = alpha * alpha;
            const f32 GGXV = NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
            const f32 GGXL = NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);

            return 0.5f / std::max(GGXV + GGXL, kEpsilon);
        }
    } // namespace

    f32 SkinOralCoatF0(f32 ior) noexcept
    {
        if (!std::isfinite(ior))
            return 0.0f;

        const f32 clamped = std::clamp(ior, kMinSkinOralCoatIor, kMaxSkinOralCoatIor);
        const f32 ratio = (clamped - kSkinOralAmbientIor) / (clamped + kSkinOralAmbientIor);
        return ratio * ratio;
    }

    f32 SkinOralCoatFresnel(f32 f0, f32 cosTheta) noexcept
    {
        if (!std::isfinite(f0) || !std::isfinite(cosTheta))
            return 0.0f;

        const f32 clampedF0 = std::clamp(f0, 0.0f, 1.0f);
        const f32 clampedCos = std::clamp(cosTheta, 0.0f, 1.0f);
        const f32 oneMinus = 1.0f - clampedCos;
        // pow(1 - cos, 5) as four multiplies, which is what the shader does and
        // what every Schlick in this engine does. `powf` here would agree to
        // within a couple of ulp and the parity test compares exactly.
        const f32 oneMinus2 = oneMinus * oneMinus;
        const f32 oneMinus5 = oneMinus2 * oneMinus2 * oneMinus;
        return clampedF0 + (1.0f - clampedF0) * oneMinus5;
    }

    f32 SkinOralCoatAttenuation(f32 strength, f32 fresnel) noexcept
    {
        if (!std::isfinite(strength) || !std::isfinite(fresnel))
            return 1.0f;

        // Clamped INDEPENDENTLY and then multiplied, so the product is in
        // [0, 1] and the result in [0, 1] — which is the whole contract of this
        // function and the reason the clamps are here rather than at the call
        // sites, where there are five of them across three lighting paths.
        const f32 taken = std::clamp(strength, 0.0f, 1.0f) * std::clamp(fresnel, 0.0f, 1.0f);
        return 1.0f - taken;
    }

    f32 SkinOralCoatSpecular(const glm::vec3& normal, const glm::vec3& view,
                             const glm::vec3& lightDir, f32 coatRoughness, f32 coatF0) noexcept
    {
        if (!Math::IsFinite(normal) || !Math::IsFinite(view) || !Math::IsFinite(lightDir))
            return 0.0f;
        if (!std::isfinite(coatRoughness) || !std::isfinite(coatF0))
            return 0.0f;

        const glm::vec3 sum = view + lightDir;
        const f32 lenSq = glm::dot(sum, sum);
        // A view and light direction that cancel have no half-vector. Guarded
        // with the `!(x > eps)` form the shader's degeneracy guards use, so a
        // NaN input takes the fallback instead of being normalized into one.
        if (!(lenSq > 1.0e-20f))
            return 0.0f;

        const glm::vec3 half = sum * (1.0f / std::sqrt(lenSq));

        const f32 roughness = std::clamp(coatRoughness, kMinSkinOralCoatRoughness, kMaxSkinOralCoatRoughness);
        const f32 D = DistributionGGXUnclamped(normal, half, roughness);
        const f32 Vis = VisibilitySmithGGXCorrelated(normal, view, lightDir, roughness);
        // dot(V, H) — the angle of INCIDENCE on the microfacet that reflected
        // this light. See SkinOralCoatFresnel's contract, and note that every
        // other Fresnel in this engine passes the same quantity (PBRCommon.glsl
        // spells it `dot(H, V)` / `VdotH` at all six of its call sites).
        const f32 F = SkinOralCoatFresnel(coatF0, glm::dot(view, half));

        return D * Vis * F;
    }

    SkinOralCoatResult ApplySkinOralCoatAmbient(const glm::vec3& baseDiffuse, const glm::vec3& baseSpecular,
                                                const glm::vec4& oralLane, const glm::vec2& coatEnvBRDF,
                                                const glm::vec3& coatPrefiltered) noexcept
    {
        SkinOralCoatResult result{ baseDiffuse, baseSpecular };
        const f32 strength = oralLane.x;
        if (!(strength > 0.0f))
            return result;
        if (!Math::IsFinite(baseDiffuse) || !Math::IsFinite(baseSpecular) || !Math::IsFinite(coatPrefiltered) ||
            !std::isfinite(coatEnvBRDF.x) || !std::isfinite(coatEnvBRDF.y) || !std::isfinite(oralLane.z))
            return result;

        const f32 albedo =
            std::clamp(std::clamp(oralLane.z, 0.0f, 1.0f) * coatEnvBRDF.x + coatEnvBRDF.y, 0.0f, 1.0f);
        const f32 reflected = std::clamp(strength, 0.0f, 1.0f) * albedo;
        const f32 attenuation = 1.0f - reflected;
        result.Diffuse = baseDiffuse * attenuation;
        result.Specular = baseSpecular * attenuation + glm::max(coatPrefiltered, glm::vec3(0.0f)) * reflected;
        return result;
    }

    SkinOralCoatResult ApplySkinOralCoat(const glm::vec3& baseDiffuse, const glm::vec3& baseSpecular,
                                         const glm::vec3& normal, const glm::vec3& view,
                                         const glm::vec3& lightDir, const glm::vec3& radiance,
                                         const glm::vec4& oralLane) noexcept
    {
        SkinOralCoatResult result{ baseDiffuse, baseSpecular };

        const f32 strength = oralLane.x;
        // A zero strength returns the base UNTOUCHED — not multiplied by an
        // attenuation that happens to be 1, untouched — so a profile whose
        // author left the coat off is bit-identical to the version-3 frame and
        // costs one compare rather than a GGX evaluation. That exactness is the
        // neutral-identity arm the acceptance criteria are demonstrated against.
        if (!(strength > 0.0f))
            return result;

        if (!Math::IsFinite(radiance) || !Math::IsFinite(baseDiffuse) || !Math::IsFinite(baseSpecular))
            return result;

        const glm::vec3 sum = view + lightDir;
        const f32 lenSq = glm::dot(sum, sum);
        if (!(lenSq > 1.0e-20f))
            return result;
        const glm::vec3 half = sum * (1.0f / std::sqrt(lenSq));

        // dot(V, H), matching SkinOralCoatSpecular above — the attenuation and
        // the lobe MUST use one Fresnel, or the partition they form stops
        // summing to one.
        const f32 fresnel = SkinOralCoatFresnel(oralLane.z, glm::dot(view, half));
        const f32 attenuation = SkinOralCoatAttenuation(strength, fresnel);

        const f32 coat = SkinOralCoatSpecular(normal, view, lightDir, oralLane.y, oralLane.z);
        const f32 clampedStrength = std::clamp(strength, 0.0f, 1.0f);

        result.Diffuse = baseDiffuse * attenuation;
        result.Specular = baseSpecular * attenuation + glm::max(radiance, glm::vec3(0.0f)) * (coat * clampedStrength);
        return result;
    }

    f32 SkinOralCavityWeight(f32 occlusion, f32 cavityOcclusion) noexcept
    {
        if (!std::isfinite(occlusion) || !std::isfinite(cavityOcclusion))
            return 1.0f;

        const f32 ao = std::clamp(occlusion, 0.0f, 1.0f);
        const f32 amount = std::clamp(cavityOcclusion, 0.0f, 1.0f);
        // `1 + amount * (ao - 1)` rather than `(1 - amount) + amount * ao`, for
        // the reason SkinSpecularMix is written the way it is: at amount == 0
        // the first returns 1 EXACTLY for every finite ao, which is what makes
        // "cavity off" a true identity rather than a similar-looking frame.
        return 1.0f + amount * (ao - 1.0f);
    }

    glm::vec4 SkinOralLane(const SkinProfileParameters& parameters) noexcept
    {
        return glm::vec4(parameters.Oral.CoatStrength,
                         parameters.Oral.CoatRoughness,
                         SkinOralCoatF0(parameters.Oral.CoatIor),
                         parameters.Oral.CavityOcclusion);
    }

} // namespace OloEngine
