#pragma once

// =============================================================================
// ReferenceBRDF.h — the C++ mirror of the engine's shading BRDF (issue #709)
//
// The offline reference path tracer only means anything if it shades with the
// SAME BRDF the raster path does: "a divergence between raster and reference
// *is* the bug" only holds when the two agree by construction. This header is
// therefore a function-for-function port of the BRDF half of
// OloEditor/assets/shaders/include/PBRCommon.glsl (plus the sampling
// primitives it pulls in from include/MathCommon.glsl). Every function names
// its GLSL counterpart, and the port is pinned against the real compiled
// shader by ReferenceBRDFGpuParityTest — a GPU probe that evaluates the GLSL
// over a parameter grid and compares it texel-for-texel against the functions
// below. If you change PBRCommon.glsl's BRDF, that test fails until you change
// this file too. That is the whole point.
//
// Deliberate fidelity notes — these are ports of the engine's math, not of
// textbook math, so the quirks are reproduced on purpose:
//
//   * `DistributionGGX` clamps its denominator with max(denom, EPSILON) where
//     EPSILON is 1e-4, NOT a smaller float epsilon. At roughness -> 0 that
//     clamp is what bounds the NDF spike; a "cleaner" 1e-8 here would make the
//     reference brighter than the renderer at mirror roughness.
//   * `GeometrySmith` uses the Schlick-GGX k = (r+1)^2/8 remap (the UE4 direct-
//     lighting form), not the height-correlated Smith. PBRCommon has BOTH
//     (`visibilitySmithGGXCorrelated`, mirrored below as
//     `VisibilitySmithGGXCorrelated`); `cookTorranceBRDF` — the function the
//     lit passes actually call — uses this one. See THE ALPHA LEDGER in
//     PBRCommon.glsl's GEOMETRY FUNCTIONS section for why the two differ and
//     why that difference is deliberate rather than the #904 bug.
//   * `CookTorranceBRDF` computes kD = 1 - F with F evaluated at the HALF
//     vector, which is the common (mildly non-reciprocal) formulation. It is
//     what ships, so it is what the reference integrates.
//
// Everything here is header-only, allocation-free and GL-independent so the
// tracer and its contract tests run headless (ADR 0002).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine::PathTracing
{
    // -------------------------------------------------------------------------
    // Constants — mirrors of the #defines at the top of PBRCommon.glsl. The
    // literals are copied verbatim (not recomputed) so the C++ and GLSL
    // evaluations start from bit-identical constants.
    // -------------------------------------------------------------------------
    inline constexpr f32 kPi = 3.14159265359f;
    inline constexpr f32 kTwoPi = 6.28318530718f;
    inline constexpr f32 kInvPi = 0.31830988618f;

    // GLSL `EPSILON`. Load-bearing: it is the denominator clamp in
    // distributionGGX / geometrySchlickGGX / the specular divide.
    inline constexpr f32 kEpsilon = 0.0001f;

    // GLSL `DEFAULT_DIELECTRIC_F0` / `MIN_ROUGHNESS`.
    inline constexpr f32 kDefaultDielectricF0 = 0.04f;
    inline constexpr f32 kMinRoughness = 0.04f;

    // -------------------------------------------------------------------------
    // Small helpers (GLSL: MathCommon.glsl Pow5, PBRCommon.glsl SATURATE)
    // -------------------------------------------------------------------------

    [[nodiscard]] inline constexpr f32 Pow2(f32 x) noexcept
    {
        return x * x;
    }

    [[nodiscard]] inline constexpr f32 Pow5(f32 x) noexcept
    {
        const f32 x2 = x * x;
        return x2 * x2 * x;
    }

    [[nodiscard]] inline f32 Saturate(f32 x) noexcept
    {
        return std::clamp(x, 0.0f, 1.0f);
    }

    // Luminance under Rec. 709 primaries. Not a PBRCommon function — used by
    // the integrator for Russian-roulette / MIS lobe weights, where a scalar
    // "how much energy is left" is needed.
    [[nodiscard]] inline f32 Luminance(const glm::vec3& c) noexcept
    {
        return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    }

    // -------------------------------------------------------------------------
    // Fresnel (GLSL: fresnelSchlick)
    // -------------------------------------------------------------------------
    [[nodiscard]] inline glm::vec3 FresnelSchlick(f32 cosTheta, const glm::vec3& f0) noexcept
    {
        return f0 + (glm::vec3(1.0f) - f0) * Pow5(Saturate(1.0f - cosTheta));
    }

    // -------------------------------------------------------------------------
    // Normal distribution (GLSL: distributionGGX)
    // -------------------------------------------------------------------------
    [[nodiscard]] inline f32 DistributionGGX(const glm::vec3& n, const glm::vec3& h, f32 roughness) noexcept
    {
        const f32 a = roughness * roughness;
        const f32 a2 = a * a;
        const f32 nDotH = std::max(glm::dot(n, h), 0.0f);
        const f32 nDotH2 = nDotH * nDotH;

        const f32 num = a2;
        f32 denom = (nDotH2 * (a2 - 1.0f) + 1.0f);
        denom = kPi * denom * denom;

        return num / std::max(denom, kEpsilon);
    }

    // The TRUE (unclamped) GGX NDF, on a cosine rather than a pair of vectors.
    //
    // This is NOT the function the renderer evaluates — `DistributionGGX`
    // above is, complete with its 1e-4 denominator clamp — and the difference
    // is deliberate and load-bearing. This one exists solely to express the
    // density of `ImportanceSampleGGX`, and that sampler draws from the
    // STANDARD, unclamped GGX distribution: its inversion
    // `cosTheta = sqrt((1 - xi) / (1 + (a^2 - 1) xi))` is derived from the
    // unclamped NDF and knows nothing about EPSILON.
    //
    // Using the clamped form as the PDF is a silent, catastrophic bias. Below
    // roughness ~0.27 the clamp caps D near the peak, so a clamped PDF
    // UNDER-reports the true sampling density exactly where the sampler
    // concentrates its samples — and f / pdf explodes. Measured on the white
    // furnace: the directional albedo comes out 2.18 instead of 0.96 at
    // roughness 0.1 (metallic 0), and 1.00 instead of 0.035 at roughness 0.1
    // (metallic 1) — a reference that CREATES energy, which would have blessed
    // every too-bright specular bug it exists to catch. With the true density
    // both match the analytic hemisphere integral to five decimals.
    //
    // The lesson generalises: a PDF describes the SAMPLER, never the
    // integrand. Reusing an evaluation-side numerical guard in a density is
    // always wrong, and it is invisible — the image just converges brighter.
    // The denominator guard here is a denormal floor, not a value clamp;
    // `SamplingRoughness` floors alpha well above where it could engage.
    [[nodiscard]] inline f32 DistributionGGXSamplingDensity(f32 nDotH, f32 roughness) noexcept
    {
        const f32 a = roughness * roughness;
        const f32 a2 = a * a;
        const f32 c = std::max(nDotH, 0.0f);
        f32 denom = (c * c * (a2 - 1.0f) + 1.0f);
        denom = kPi * denom * denom;
        return a2 / std::max(denom, std::numeric_limits<f32>::min());
    }

    // -------------------------------------------------------------------------
    // Geometry / masking-shadowing (GLSL: geometrySchlickGGX, geometrySmith)
    // -------------------------------------------------------------------------
    [[nodiscard]] inline f32 GeometrySchlickGGX(f32 nDotV, f32 roughness) noexcept
    {
        const f32 r = (roughness + 1.0f);
        const f32 k = (r * r) / 8.0f;

        const f32 num = nDotV;
        const f32 denom = nDotV * (1.0f - k) + k;

        return num / std::max(denom, kEpsilon);
    }

    [[nodiscard]] inline f32 GeometrySmith(const glm::vec3& n, const glm::vec3& v, const glm::vec3& l, f32 roughness) noexcept
    {
        const f32 nDotV = std::max(glm::dot(n, v), 0.0f);
        const f32 nDotL = std::max(glm::dot(n, l), 0.0f);
        const f32 ggx2 = GeometrySchlickGGX(nDotV, roughness);
        const f32 ggx1 = GeometrySchlickGGX(nDotL, roughness);

        return ggx1 * ggx2;
    }

    // -------------------------------------------------------------------------
    // The BRDF the lit passes evaluate (GLSL: cookTorranceBRDF)
    //
    // Returns f(l, v) — diffuse + specular, WITHOUT the cosine term. Callers
    // multiply by (n · l) themselves, exactly as the GLSL call sites do.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline glm::vec3 CookTorranceBRDF(const glm::vec3& n, const glm::vec3& v, const glm::vec3& l,
                                                    const glm::vec3& albedo, f32 metallic, f32 roughness) noexcept
    {
        const glm::vec3 h = glm::normalize(v + l);

        glm::vec3 f0 = glm::vec3(kDefaultDielectricF0);
        f0 = glm::mix(f0, albedo, metallic);

        const f32 ndf = DistributionGGX(n, h, roughness);
        const f32 g = GeometrySmith(n, v, l, roughness);
        const glm::vec3 f = FresnelSchlick(std::max(glm::dot(h, v), 0.0f), f0);

        const glm::vec3 numerator = ndf * g * f;
        const f32 denominator = 4.0f * std::max(glm::dot(n, v), 0.0f) * std::max(glm::dot(n, l), 0.0f) + kEpsilon;
        const glm::vec3 specular = numerator / denominator;

        glm::vec3 kD = glm::vec3(1.0f) - f;
        kD *= 1.0f - metallic;

        return kD * albedo * kInvPi + specular;
    }

    // -------------------------------------------------------------------------
    // Height-correlated Smith, and the Lambda it must agree with (issue #904)
    //
    // These two are NOT on the shipping lit path — `CookTorranceBRDF` above is,
    // and it uses the UE4 k remap. They are mirrored here because they are the
    // pair whose alpha convention #904 was about, and because ReferenceBRDF.h
    // is where this repo pins GLSL functions against C++: an unmirrored GLSL
    // function is one nothing can detect drift in.
    // -------------------------------------------------------------------------

    // Smith's Lambda for GGX from the cosine with the macrosurface normal
    // (GLSL: ggxSmithLambda). Takes ALPHA, not roughness — matching the GLSL,
    // whose callers pass roughness * roughness.
    //
    // THERE IS A SECOND C++ MIRROR OF THIS FUNCTION: `Vndf::SmithLambda` in
    // OloEngine/tests/Rendering/StochasticSamplerTest.cpp. That is deliberate,
    // not an oversight to be tidied away — it is f64 because it backs a
    // brute-force reference integrator whose whole value is precision, and
    // narrowing it to this f32 form would weaken the test it exists for. Each
    // copy carries its own guard (this one by
    // HeightCorrelatedVisibilityMatchesTheVndfLambda, that one by
    // VndfEstimatorMatchesBruteForce), so neither can drift silently. If you
    // add a THIRD, stop and reuse one of these instead.
    [[nodiscard("The Lambda is the masking term; discarding it drops the shadowing.")]] inline f32
    GgxSmithLambda(f32 nDotX, f32 alpha) noexcept
    {
        const f32 c = std::clamp(std::abs(nDotX), 1.0e-4f, 1.0f);
        const f32 c2 = c * c;
        const f32 tan2 = (1.0f - c2) / c2;
        return 0.5f * (-1.0f + std::sqrt(1.0f + alpha * alpha * tan2));
    }

    // Height-correlated Smith VISIBILITY (GLSL: visibilitySmithGGXCorrelated).
    //
    // Returns V = G2 / (4 * nDotV * nDotL) — the Cook-Torrance denominator is
    // folded in and cancels. Callers multiply: D * V * F. Do not divide by
    // 4*nDotV*nDotL again; that double-divide is precisely the bug #904 fixed
    // on the GLSL side.
    //
    // alpha = roughness^2, so a2 = roughness^4, matching DistributionGGX.
    //
    // Takes COSINES where the GLSL takes vectors, which is the one place this
    // port deliberately does not mirror its counterpart's signature. Three
    // adjacent `const glm::vec3&` parameters are silently swappable at a call
    // site — and the GLSL's own body immediately reduces them to two cosines
    // anyway, so nothing is lost. `DistributionGGXSamplingDensity` above sets
    // the same precedent. The `max(dot(...), 0)` clamp the GLSL applies is kept
    // INSIDE this function rather than pushed onto callers, so the quirk still
    // lives in the mirror where it belongs.
    [[nodiscard("This is the visibility term; discarding it silently drops masking-shadowing.")]] inline f32
    VisibilitySmithGGXCorrelated(f32 nDotV, f32 nDotL, f32 roughness) noexcept
    {
        nDotV = std::max(nDotV, 0.0f);
        nDotL = std::max(nDotL, 0.0f);

        const f32 alpha = roughness * roughness;
        const f32 a2 = alpha * alpha;
        const f32 ggxV = nDotL * std::sqrt(nDotV * nDotV * (1.0f - a2) + a2);
        const f32 ggxL = nDotV * std::sqrt(nDotL * nDotL * (1.0f - a2) + a2);

        return 0.5f / std::max(ggxV + ggxL, kEpsilon);
    }

    // -------------------------------------------------------------------------
    // Sampling primitives (GLSL: MathCommon.glsl OrthonormalBasis /
    // ImportanceSampleGGX). Ported so the reference importance-samples the
    // SAME lobe shape the IBL bake and the probe shaders do.
    // -------------------------------------------------------------------------

    // Duff et al. 2017 branch-free orthonormal basis (GLSL: OrthonormalBasis).
    inline void OrthonormalBasis(const glm::vec3& n, glm::vec3& outTangent, glm::vec3& outBitangent) noexcept
    {
        const f32 s = n.z >= 0.0f ? 1.0f : -1.0f;
        const f32 a = -1.0f / (s + n.z);
        const f32 c = n.x * n.y * a;
        outTangent = glm::vec3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
        outBitangent = glm::vec3(c, s + n.y * n.y * a, -n.y);
    }

    // GGX half-vector importance sample (GLSL: ImportanceSampleGGX).
    [[nodiscard]] inline glm::vec3 ImportanceSampleGGX(const glm::vec2& xi, const glm::vec3& n, f32 roughness) noexcept
    {
        const f32 a = roughness * roughness;

        const f32 phi = 2.0f * kPi * xi.x;
        const f32 cosTheta = std::sqrt(std::max(0.0f, (1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y)));
        const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

        const glm::vec3 h(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

        glm::vec3 tangent;
        glm::vec3 bitangent;
        OrthonormalBasis(n, tangent, bitangent);
        return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
    }

    // Solid-angle PDF of a direction produced by reflecting `v` about a
    // half-vector drawn from ImportanceSampleGGX. The NDF sample has density
    // D(h) * (n·h) over half-vectors; the Jacobian of the reflection is
    // 1 / (4 (v·h)). No GLSL counterpart — the shaders importance-sample but
    // never need the density (they use the NdotL-weighted-average estimator);
    // an unbiased integrator does.
    [[nodiscard]] inline f32 PdfGGX(f32 nDotH, f32 vDotH, f32 roughness) noexcept
    {
        if (vDotH <= 0.0f)
            return 0.0f;
        return DistributionGGXSamplingDensity(nDotH, roughness) * std::max(nDotH, 0.0f) / (4.0f * vDotH);
    }

    // Cosine-weighted hemisphere sample about `n` (Malley's method).
    [[nodiscard]] inline glm::vec3 CosineSampleHemisphere(const glm::vec2& xi, const glm::vec3& n) noexcept
    {
        const f32 r = std::sqrt(std::max(0.0f, xi.x));
        const f32 phi = kTwoPi * xi.y;
        const f32 x = r * std::cos(phi);
        const f32 y = r * std::sin(phi);
        const f32 z = std::sqrt(std::max(0.0f, 1.0f - xi.x));

        glm::vec3 tangent;
        glm::vec3 bitangent;
        OrthonormalBasis(n, tangent, bitangent);
        return glm::normalize(tangent * x + bitangent * y + n * z);
    }

    [[nodiscard]] inline f32 PdfCosineHemisphere(f32 nDotL) noexcept
    {
        return std::max(nDotL, 0.0f) * kInvPi;
    }

    // Uniform sphere direction — used to sample a spherical area emitter and
    // (in the tests) to integrate a hemisphere without importance sampling.
    [[nodiscard]] inline glm::vec3 UniformSampleSphere(const glm::vec2& xi) noexcept
    {
        const f32 z = 1.0f - 2.0f * xi.x;
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 phi = kTwoPi * xi.y;
        return glm::vec3(r * std::cos(phi), r * std::sin(phi), z);
    }

    // -------------------------------------------------------------------------
    // Multiple-importance-sampling weight (Veach & Guibas power heuristic,
    // beta = 2). No GLSL counterpart; used to combine next-event estimation
    // with BSDF sampling in the integrator.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline f32 PowerHeuristic(f32 pdfA, f32 pdfB) noexcept
    {
        const f32 a2 = pdfA * pdfA;
        const f32 b2 = pdfB * pdfB;
        const f32 denom = a2 + b2;
        if (!(denom > 0.0f))
            return 0.0f;
        return a2 / denom;
    }

    // -------------------------------------------------------------------------
    // Lobe selection probability for one-sample BSDF sampling.
    //
    // The integrator draws from ONE of the two lobes per bounce but always
    // evaluates the FULL CookTorranceBRDF and divides by the COMBINED density
    //     pdf = pSpecular * pdfGGX + (1 - pSpecular) * pdfCosine
    // which is the standard one-sample MIS estimator over a lobe mixture — it
    // is unbiased for any pSpecular in (0, 1). The value below is only a
    // variance choice: the specular lobe's share of the reflectance at normal
    // incidence, floored/ceilinged away from 0 and 1 so neither lobe can ever
    // be unreachable (a zero probability on a lobe that carries energy is the
    // classic silent bias in this construction).
    // -------------------------------------------------------------------------
    [[nodiscard]] inline f32 SpecularLobeProbability(const glm::vec3& albedo, f32 metallic) noexcept
    {
        const glm::vec3 f0 = glm::mix(glm::vec3(kDefaultDielectricF0), albedo, metallic);
        const glm::vec3 diffuse = albedo * (1.0f - metallic);
        const f32 specularWeight = Luminance(f0);
        const f32 diffuseWeight = Luminance(diffuse);
        const f32 total = specularWeight + diffuseWeight;
        if (!(total > 0.0f))
            return 0.5f;
        return std::clamp(specularWeight / total, 0.1f, 0.9f);
    }

    // -------------------------------------------------------------------------
    // Punctual-light distance attenuation (GLSL: calculateAttenuation)
    //
    // Ported deliberately, quirks and all. The engine's point/spot falloff is
    // NOT inverse-square: it is a configurable constant/linear/quadratic term
    // multiplied by a squared smooth range cutoff. If the reference used the
    // physically-correct 1/d^2 instead, every raster-vs-reference comparison
    // would show a distance-dependent divergence that is a *convention*
    // difference, not a bug — and it would mask the transport bugs the
    // instrument exists to find. The reference differs from the raster path in
    // TRANSPORT (shadows, bounces), never in the light model.
    //
    // `attenuationParams` is (constant, linear, quadratic, range), matching the
    // LightData UBO packing.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline f32 CalculateAttenuation(const glm::vec3& lightPos, const glm::vec3& fragPos,
                                                  const glm::vec4& attenuationParams) noexcept
    {
        const f32 distance = glm::length(lightPos - fragPos);
        const f32 range = attenuationParams.w;

        if (distance > range)
            return 0.0f;

        const f32 constant = attenuationParams.x;
        const f32 linear = attenuationParams.y;
        const f32 quadratic = attenuationParams.z;

        const f32 attenuation = 1.0f / (constant + linear * distance + quadratic * (distance * distance));

        const f32 falloff = Saturate(1.0f - Pow2(Pow2(distance / range)));
        return attenuation * falloff * falloff;
    }

    // Spot cone term (GLSL: calculateSpotIntensity). `spotParams` is
    // (innerCutoff, outerCutoff, falloff, enabled) as cosines.
    [[nodiscard]] inline f32 CalculateSpotIntensity(const glm::vec3& l, const glm::vec3& spotDir,
                                                    const glm::vec4& spotParams) noexcept
    {
        const f32 innerCutoff = spotParams.x;
        const f32 outerCutoff = spotParams.y;

        const f32 theta = glm::dot(l, glm::normalize(-spotDir));
        const f32 epsilon = innerCutoff - outerCutoff;
        if (!(std::abs(epsilon) > 0.0f))
            return theta >= innerCutoff ? 1.0f : 0.0f;
        const f32 intensity = Saturate((theta - outerCutoff) / epsilon);

        return intensity * intensity;
    }

    // -------------------------------------------------------------------------
    // Display transform (GLSL: reinhardToneMapping / acesToneMapping /
    // linearToSRGB). PBRCommon's uncharted2ToneMapping and postProcessColor are
    // deliberately NOT ported — nothing the reference encodes needs them, and an
    // unused port is one more thing that can silently drift from the shader.
    //
    // The reference renders LINEAR radiance; these exist so a reference image
    // can be encoded the same way the raster path's composite is, which is the
    // only way an absolute raster-vs-reference pixel comparison means anything.
    // Note the gamma here is PBRCommon's fixed 2.2 power curve, not the piecewise
    // sRGB EOTF — again a port of what ships, not of the standard.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline glm::vec3 ReinhardToneMapping(const glm::vec3& color) noexcept
    {
        return color / (color + glm::vec3(1.0f));
    }

    [[nodiscard]] inline glm::vec3 AcesToneMapping(const glm::vec3& color) noexcept
    {
        constexpr f32 a = 2.51f;
        constexpr f32 b = 0.03f;
        constexpr f32 c = 2.43f;
        constexpr f32 d = 0.59f;
        constexpr f32 e = 0.14f;

        const glm::vec3 mapped = (color * (a * color + b)) / (color * (c * color + d) + e);
        return glm::clamp(mapped, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    [[nodiscard]] inline glm::vec3 LinearToSRGB(const glm::vec3& color) noexcept
    {
        constexpr f32 invGamma = 0.45454545455f;
        return glm::vec3(std::pow(std::max(color.x, 0.0f), invGamma),
                         std::pow(std::max(color.y, 0.0f), invGamma),
                         std::pow(std::max(color.z, 0.0f), invGamma));
    }
} // namespace OloEngine::PathTracing
