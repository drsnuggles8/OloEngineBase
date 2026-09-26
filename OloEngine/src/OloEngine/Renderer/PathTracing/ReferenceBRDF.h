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
//     vector. It is what ships, so it is what the reference integrates. That
//     formulation IS reciprocal: dot(h, v) == dot(h, l), and every other factor
//     is symmetric in (v, l) — BsdfIdentityOracleTest.ClosuresAreReciprocal
//     measures a worst asymmetry of 5e-7 (issue #1347).
//
// Everything here is header-only, allocation-free and GL-independent so the
// tracer and its contract tests run headless (ADR 0002).
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PathTracing/GgxEnergyTables.h"

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
    //
    // COSINE-ONLY CALLERS. This scalar form cancels near the peak of a sharp
    // lobe (see the vector overload below): every caller that holds n and h
    // uses that one. It stays for callers that only hold a cosine — a chart
    // density, a finite-difference probe — and nothing on a shading path.
    [[nodiscard]] inline f32 DistributionGGXSamplingDensity(f32 nDotH, f32 roughness) noexcept
    {
        const f32 a = roughness * roughness;
        const f32 a2 = a * a;
        const f32 c = std::max(nDotH, 0.0f);
        f32 denom = (c * c * (a2 - 1.0f) + 1.0f);
        denom = kPi * denom * denom;
        return a2 / std::max(denom, std::numeric_limits<f32>::min());
    }

    // The same density from the VECTORS, which is what every caller that has
    // them should use (issue #1347). The scalar form's denominator
    // c^2 (a^2 - 1) + 1 is 1 - c^2 + a^2 c^2: near the lobe peak it subtracts
    // two numbers within ~a^2 of each other, and c = dot(n, h) has already lost
    // the half-vector's small tangential components to f32 rounding of h.z.
    // Measured against the f64 oracle, D averaged over the lobe read +3.5 % at
    // roughness 0.04 and +1.3 % at 0.05 (single points up to +17 %), so the v2
    // white furnace and both specular pdfs integrated above 1. sin^2 taken as
    // |n x h|^2 keeps those components at full relative precision:
    // BsdfIdentityOracleTest measures 1.0000 after. For unit n and h the two
    // forms are the same function.
    [[nodiscard]] inline f32 DistributionGGXSamplingDensity(const glm::vec3& n, const glm::vec3& h, f32 roughness) noexcept
    {
        const f32 a = roughness * roughness;
        const f32 a2 = a * a;
        const f32 c = glm::dot(n, h);
        if (c <= 0.0f)
            return a2 * kInvPi; // the scalar form's value at max(nDotH, 0) = 0
        const glm::vec3 t = glm::cross(n, h);
        f32 denom = glm::dot(t, t) + a2 * c * c;
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
        // cos^2 and sin^2 from the one denominator (1 + (a^2 - 1) xi) >= a^2 > 0.
        // sin is NOT sqrt(1 - cos^2): near the pole cos^2 rounds to 1.0f in f32
        // and that form puts a fraction 2^-25 / (a^2 + 2^-25) of all draws
        // exactly on the normal — 1.2 % at the 0.04 sampling floor (issue #1347,
        // BsdfSamplingDistributionTest). a^2 xi / denom has no cancellation.
        const f32 denom = 1.0f + (a * a - 1.0f) * xi.y;
        const f32 cosTheta = std::sqrt(std::max(0.0f, (1.0f - xi.y) / denom));
        const f32 sinTheta = std::sqrt(std::max(0.0f, a * a * xi.y / denom));

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
    // Cosine-only form (the cos-form D cancels at low roughness); shading paths use
    // the vector overload below.
    [[nodiscard]] inline f32 PdfGGX(f32 nDotH, f32 vDotH, f32 roughness) noexcept
    {
        if (vDotH <= 0.0f)
            return 0.0f;
        return DistributionGGXSamplingDensity(nDotH, roughness) * std::max(nDotH, 0.0f) / (4.0f * vDotH);
    }

    // PdfGGX from the vectors, with the cancellation-free D (see the vector
    // DistributionGGXSamplingDensity). BSDF::Pdf uses this one.
    [[nodiscard]] inline f32 PdfGGX(const glm::vec3& n, const glm::vec3& v, const glm::vec3& h, f32 roughness) noexcept
    {
        const f32 vDotH = glm::dot(v, h);
        if (vDotH <= 0.0f)
            return 0.0f;
        return DistributionGGXSamplingDensity(n, h, roughness) * std::max(glm::dot(n, h), 0.0f) / (4.0f * vDotH);
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

    // =========================================================================
    // PBR CLOSURE V2 (issue #975) — twins of the PBR CLOSURE V2 section in
    // PBRCommon.glsl, in the same order. The Legacy functions above are frozen;
    // these are the versioned opt-in closure the reference tracer dispatches to
    // through PBRClosureBSDF.h when ReferenceMaterial::Model == ClosureV2.
    // Pinned by ReferenceBRDFGpuParityTest's v2 probe channels.
    // =========================================================================

    // GLSL: closureV2Roughness. The ONLY roughness guard in the v2 closure —
    // applied identically before evaluating, sampling and computing the
    // density, which is what lets ONE D serve all three.
    [[nodiscard]] inline f32 ClosureV2Roughness(f32 roughness) noexcept
    {
        return std::clamp(roughness, kMinRoughness, 1.0f);
    }

    // GLSL: ggxEnergyTableCoordinate — where a mu or roughness value lands on
    // the generated table's axis. The nodes are node-centred and square-root
    // spaced, value_j = (j / (N - 1))^2 (issue #1478; GgxEnergyTables.h says
    // why), so the coordinate is sqrt(value) * (N - 1) and both endpoints are
    // nodes: nothing clamps short of 0 or 1 and nothing extrapolates.
    [[nodiscard]] inline f32 GgxEnergyTableCoordinate(f32 value) noexcept
    {
        return std::sqrt(std::clamp(value, 0.0f, 1.0f)) * static_cast<f32>(kGgxEnergyTableSize - 1);
    }

    // The inverse: the mu or roughness value node `node` of a `gridSize` grid
    // sits at, (node / (gridSize - 1))^2. No GLSL twin — shaders only ever go
    // value -> coordinate. The generator bakes at these values (with its own
    // --grid) and ClosureV2Test recomputes entries at them.
    [[nodiscard]] inline f32 GgxEnergyNodeValue(u32 node, u32 gridSize = kGgxEnergyTableSize) noexcept
    {
        const f32 x = static_cast<f32>(node) / static_cast<f32>(gridSize - 1);
        return x * x;
    }

    // GLSL: ggxEnergy — bilinear lookup of both single-scatter moments,
    // x = 1 - Ess(mu, r) and y = Schlick(mu, r) (GgxEnergyTables.h).
    // `roughness` is AUTHORED perceptual roughness; the table rows bake the v2
    // alpha clamp in. The lower cell index stops at N - 2 so value 1 reads the
    // last node with weight 1 instead of indexing past it.
    [[nodiscard]] inline glm::vec2 GgxEnergy(f32 mu, f32 roughness) noexcept
    {
        const f32 x = GgxEnergyTableCoordinate(mu);
        const f32 y = GgxEnergyTableCoordinate(roughness);
        const u32 x0 = std::min(static_cast<u32>(x), kGgxEnergyTableSize - 2);
        const u32 y0 = std::min(static_cast<u32>(y), kGgxEnergyTableSize - 2);
        const f32 fx = x - static_cast<f32>(x0);
        const f32 fy = y - static_cast<f32>(y0);
        const glm::vec2 v00 = GgxEnergyEntry(y0 * kGgxEnergyTableSize + x0);
        const glm::vec2 v10 = GgxEnergyEntry(y0 * kGgxEnergyTableSize + x0 + 1);
        const glm::vec2 v01 = GgxEnergyEntry((y0 + 1) * kGgxEnergyTableSize + x0);
        const glm::vec2 v11 = GgxEnergyEntry((y0 + 1) * kGgxEnergyTableSize + x0 + 1);
        return glm::mix(glm::mix(v00, v10, fx), glm::mix(v01, v11, fx), fy);
    }

    // GLSL: ggxEnergyAverage — linear lookup of the averages row,
    // x = 1 - E_avg(r), y = Schlick_avg(r).
    [[nodiscard]] inline glm::vec2 GgxEnergyAverage(f32 roughness) noexcept
    {
        const f32 y = GgxEnergyTableCoordinate(roughness);
        const u32 y0 = std::min(static_cast<u32>(y), kGgxEnergyTableSize - 2);
        const f32 fy = y - static_cast<f32>(y0);
        return glm::mix(GgxEnergyAvgEntry(y0), GgxEnergyAvgEntry(y0 + 1), fy);
    }

    // GLSL: closureV2SpecularAlbedo — the directional albedo of the WHOLE v2
    // specular lobe from one table read `energy` = (1 - Ess, Schlick):
    //
    //   E_spec = F0 (Ess - Schlick) + Schlick   single scatter, Schlick F
    //          + F_ms (1 - Ess)                 the Kulla-Conty lobe's integral
    //
    // The second line is exact: the lobe below integrates to F_ms (1 - Ess(mu))
    // because its l-dependence is (1 - Ess(mu_l)) / (1 - E_avg) and
    // 2 int (1 - Ess) mu dmu = 1 - E_avg. The same expression on the averages
    // row gives the cosine-averaged E_spec_avg.
    [[nodiscard]] inline glm::vec3 ClosureV2SpecularAlbedo(const glm::vec2& energy, const glm::vec3& f0,
                                                           const glm::vec3& fresnelMs) noexcept
    {
        return f0 * (1.0f - energy.x - energy.y) + energy.y + fresnelMs * energy.x;
    }

    // What the energy tables give ClosureV2Evaluate for one (v, l) pair. GLSL
    // twin: the ClosureV2Energy struct.
    struct ClosureV2EnergyTerms
    {
        // The Kulla-Conty multiple-scattering lobe, cosine NOT included.
        glm::vec3 MultiScatter{ 0.0f };
        // The Lambert weight that replaces (1 - F(v.h)): see ClosureV2Energy.
        glm::vec3 DiffuseCoupling{ 1.0f };
    };

    // GLSL: closureV2Energy — every table-driven term of the v2 closure, from
    // one lookup per direction and one of the averages row.
    //
    // (a) The Kulla-Conty multiple-scattering lobe:
    //
    //   f_ms = F_ms * (1 - Ess(NdotV)) * (1 - Ess(NdotL)) / (pi * (1 - E_avg))
    //   F_ms = F_avg^2 * E_avg / (1 - F_avg * (1 - E_avg)), F_avg = F0 + (1-F0)/21
    //
    // Symmetric in NdotV/NdotL, so it preserves reciprocity. With F_avg == 1
    // its hemispherical cosine integral is exactly 1 - Ess(NdotV), which is
    // what closes the white furnace and what the furnace test asserts.
    //
    // (b) The energy-conserving diffuse coupling (issue #1479), which replaces
    // the (1 - F(v.h)) Lambert weight:
    //
    //   w_d = (1 - E_spec(NdotV)) (1 - E_spec(NdotL)) / (1 - E_spec_avg)
    //
    // per channel, E_spec from ClosureV2SpecularAlbedo. Its hemispherical
    // cosine integral over l is exactly 1 - E_spec(NdotV), so a white
    // dielectric reflects what it receives: specular E_spec plus diffuse
    // 1 - E_spec. (1 - F(v.h)) subtracts the Fresnel of each (v, l) pair's own
    // half vector instead, which at grazing view leaves the diffuse almost all
    // its energy while the specular lobe takes ~0.9 of it: 1.83x at roughness
    // 0.05. Symmetric in NdotV/NdotL, so v2 stays reciprocal. The guards: a
    // negative 1 - E_spec (bilinear overshoot) reads as 0, and the denominator
    // is floored at 1e-4, which only an F0 -> 1 channel reaches — where the
    // numerator is ~0 too, so the floor darkens, never brightens.
    [[nodiscard]] inline ClosureV2EnergyTerms ClosureV2Energy(f32 nDotV, f32 nDotL, f32 roughness,
                                                              const glm::vec3& f0) noexcept
    {
        const glm::vec2 average = GgxEnergyAverage(roughness);
        const glm::vec2 energyV = GgxEnergy(nDotV, roughness);
        const glm::vec2 energyL = GgxEnergy(nDotL, roughness);
        const f32 lossAvg = average.x;

        ClosureV2EnergyTerms terms;
        glm::vec3 fresnelMs(0.0f);
        // Below the table's resolution the lobe is near-mirror and sheds
        // nothing worth compensating; also guards the 1/lossAvg denominator.
        if (lossAvg >= 1.0e-4f)
        {
            const f32 eAvg = 1.0f - lossAvg;
            const glm::vec3 fAvg = f0 + (glm::vec3(1.0f) - f0) * (1.0f / 21.0f);
            fresnelMs = fAvg * fAvg * eAvg / (glm::vec3(1.0f) - fAvg * lossAvg);
            terms.MultiScatter = fresnelMs * (energyV.x * energyL.x) / (kPi * lossAvg);
        }

        const glm::vec3 remainingV = glm::max(glm::vec3(1.0f) - ClosureV2SpecularAlbedo(energyV, f0, fresnelMs), 0.0f);
        const glm::vec3 remainingL = glm::max(glm::vec3(1.0f) - ClosureV2SpecularAlbedo(energyL, f0, fresnelMs), 0.0f);
        const glm::vec3 remainingAvg =
            glm::max(glm::vec3(1.0f) - ClosureV2SpecularAlbedo(average, f0, fresnelMs), 1.0e-4f);
        terms.DiffuseCoupling = remainingV * remainingL / remainingAvg;
        return terms;
    }

    // The multiple-scattering lobe alone, for the tests that integrate it.
    // GLSL: closureV2Energy(...).MultiScatter.
    [[nodiscard]] inline glm::vec3 ClosureV2MultiScatter(f32 nDotV, f32 nDotL, f32 roughness,
                                                         const glm::vec3& f0) noexcept
    {
        return ClosureV2Energy(nDotV, nDotL, roughness, f0).MultiScatter;
    }

    // GLSL: closureV2Evaluate — the v2 Evaluate: f(v, l) WITHOUT the cosine,
    // matching CookTorranceBRDF's convention. One geometry term
    // (height-correlated Smith visibility), alpha-clamped unclamped-denominator
    // D, Kulla-Conty compensation, and a Lambert diffuse weighted by the
    // energy-conserving coupling (1 - metallic) w_d of ClosureV2Energy.
    [[nodiscard]] inline glm::vec3 ClosureV2Evaluate(const glm::vec3& n, const glm::vec3& v, const glm::vec3& l,
                                                     const glm::vec3& albedo, f32 metallic, f32 roughness) noexcept
    {
        const f32 r = ClosureV2Roughness(roughness);
        const glm::vec3 h = glm::normalize(v + l);
        const f32 nDotV = std::max(glm::dot(n, v), 0.0f);
        const f32 nDotL = std::max(glm::dot(n, l), 0.0f);

        const glm::vec3 f0 = glm::mix(glm::vec3(kDefaultDielectricF0), albedo, metallic);
        const f32 d = DistributionGGXSamplingDensity(n, h, r);
        const f32 vis = VisibilitySmithGGXCorrelated(nDotV, nDotL, r);
        const glm::vec3 f = FresnelSchlick(std::max(glm::dot(h, v), 0.0f), f0);

        // AUTHORED roughness into the energy lookup, per its contract — the
        // table rows bake the v2 clamp in (rows 0-3 all hold the r = 0.04 lobe,
        // and 0.04 lands on node 3, so the clamped r would read the same
        // value). GLSL twin agrees (closureV2Evaluate).
        const ClosureV2EnergyTerms energy = ClosureV2Energy(nDotV, nDotL, roughness, f0);
        const glm::vec3 specular = d * vis * f + energy.MultiScatter;

        const glm::vec3 kD = energy.DiffuseCoupling * (1.0f - metallic);
        return kD * albedo * kInvPi + specular;
    }

    // GLSL: sampleGGXVNDFTangent — Heitz 2018 §3.2 visible-normal sample in
    // TANGENT space (z = macrosurface normal), the reference listing
    // transcribed. Takes ALPHAS. The f64 brute-force twin used by the property
    // tests lives in StochasticSamplerTest.cpp's namespace Vndf; this is the
    // production-precision mirror of the GLSL.
    [[nodiscard]] inline glm::vec3 SampleGGXVNDFTangent(const glm::vec3& ve, f32 alphaX, f32 alphaY,
                                                        const glm::vec2& xi) noexcept
    {
        // 1. Stretch the view direction so the ellipsoid becomes a hemisphere.
        const glm::vec3 vh = glm::normalize(glm::vec3(alphaX * ve.x, alphaY * ve.y, ve.z));

        // 2. Orthonormal basis around Vh (degenerate when Vh is the pole).
        const f32 lenSq = vh.x * vh.x + vh.y * vh.y;
        const glm::vec3 t1 = (lenSq > 0.0f) ? (glm::vec3(-vh.y, vh.x, 0.0f) * (1.0f / std::sqrt(lenSq)))
                                            : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 t2 = glm::cross(vh, t1);

        // 3. Uniform point on the projected area: a disk, with the half below
        //    the horizon squashed into the visible hemisphere's silhouette.
        const f32 r = std::sqrt(xi.x);
        const f32 phi = kTwoPi * xi.y;
        const f32 p1 = r * std::cos(phi);
        f32 p2 = r * std::sin(phi);
        const f32 s = 0.5f * (1.0f + vh.z);
        p2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - p1 * p1)) + s * p2;

        // 4. Lift back onto the hemisphere, then unstretch to the ellipsoid.
        const glm::vec3 nh = p1 * t1 + p2 * t2 + std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * vh;
        return glm::normalize(glm::vec3(alphaX * nh.x, alphaY * nh.y, std::max(0.0f, nh.z)));
    }

    // GLSL: sampleGGXVNDF — world-space isotropic wrapper; pass roughness, not
    // alpha (it squares internally, matching the GLSL).
    [[nodiscard]] inline glm::vec3 SampleGGXVNDF(const glm::vec3& n, const glm::vec3& v, f32 roughness,
                                                 const glm::vec2& xi) noexcept
    {
        glm::vec3 tangent;
        glm::vec3 bitangent;
        OrthonormalBasis(n, tangent, bitangent);

        const glm::vec3 ve(glm::dot(v, tangent), glm::dot(v, bitangent), glm::dot(v, n));
        const f32 alpha = roughness * roughness;
        const glm::vec3 h = SampleGGXVNDFTangent(ve, alpha, alpha, xi);
        return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
    }

    // Solid-angle density of a direction produced by reflecting `v` about a
    // half-vector drawn from SampleGGXVNDF:
    //
    //   pdf(l) = D_vis(h) / (4 (v·h)) = G1(nDotV) * D(nDotH) / (4 * nDotV)
    //
    // (the v·h factors cancel). D is the UNCLAMPED NDF on the same alpha the
    // sampler stretched by — the "a PDF describes the SAMPLER" rule above, and
    // with the v2 alpha clamp this same D is also what ClosureV2Evaluate
    // evaluates, which is what makes Evaluate/Sample/Pdf agree. GLSL twin: the
    // specular term of closureV2Pdf.
    // Cosine-only form (the cos-form D cancels at low roughness); shading paths use
    // the vector overload below.
    [[nodiscard]] inline f32 PdfGGXVNDF(f32 nDotV, f32 nDotH, f32 roughness) noexcept
    {
        if (nDotV <= 0.0f)
            return 0.0f;
        const f32 alpha = roughness * roughness;
        const f32 g1V = 1.0f / (1.0f + GgxSmithLambda(nDotV, alpha));
        return g1V * DistributionGGXSamplingDensity(nDotH, roughness) / (4.0f * nDotV);
    }

    // PdfGGXVNDF from the vectors, with the cancellation-free D. BSDF::Pdf and
    // the GLSL closureV2Pdf use this form.
    [[nodiscard]] inline f32 PdfGGXVNDF(const glm::vec3& n, const glm::vec3& v, const glm::vec3& h, f32 roughness) noexcept
    {
        const f32 nDotV = glm::dot(n, v);
        if (nDotV <= 0.0f)
            return 0.0f;
        const f32 alpha = roughness * roughness;
        const f32 g1V = 1.0f / (1.0f + GgxSmithLambda(nDotV, alpha));
        return g1V * DistributionGGXSamplingDensity(n, h, roughness) / (4.0f * nDotV);
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
