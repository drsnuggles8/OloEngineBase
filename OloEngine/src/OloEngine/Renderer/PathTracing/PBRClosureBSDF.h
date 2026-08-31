#pragma once

// =============================================================================
// PBRClosureBSDF.h — the versioned Evaluate / Sample / Pdf contract (issue #975)
// =============================================================================
//
// One material, three views of the same lobe, ONE agreeing density. Path
// tracing needs Evaluate, Sample and Pdf to be mathematically consistent, and
// MIS needs the density they agree on — a superficially plausible BSDF whose
// three parts disagree converges beautifully to the wrong image, and ReSTIR
// then reuses those samples across pixels and frames. This header is that
// contract's single home on the CPU side:
//
//   Evaluate(material, n, v, l)          -> f(v, l), cosine NOT included
//   Sample  (material, n, v, lobeXi, xi) -> direction + f + the MIXTURE pdf
//   Pdf     (material, n, v, l)          -> the density Sample drew from
//
// For every sample, Sample()'s reported Pdf and Pdf() return the SAME number,
// and it is the density the integrator's MIS weights use — deliberately one
// function per model, because a MIS weight built from an independently-written
// density is the classic silently-biased integrator.
//
// Versioning: dispatch is on ReferenceMaterial::Model (PBRModel), mirroring
// the u_PBRModel lane the raster path branches on:
//
//   * Legacy    — the bodies formerly private to PathTracer.cpp, moved here
//                 VERBATIM (PathTracerCornellBoxTest.RendersAreBitIdentical
//                 pins the hashes, so the float sequence must not change):
//                 full-NDF GGX sampling with the clamped-D evaluation and the
//                 unclamped sampling density, exactly as the Legacy raster
//                 closure requires (see DistributionGGXSamplingDensity's notes).
//   * ClosureV2 — VNDF specular sampling + cosine diffuse over the v2 closure
//                 (ReferenceBRDF.h's ClosureV2* twins of PBRCommon.glsl's
//                 PBR CLOSURE V2 section). Because v2 clamps ALPHA rather than
//                 the denominator, one D serves all three functions and the
//                 Evaluate/Sample/Pdf agreement is by construction rather than
//                 by tolerance.
//
// The GLSL twins of the v2 half are closureV2Evaluate / closureV2SampleBRDF /
// closureV2Pdf in PBRCommon.glsl, pinned by ReferenceBRDFGpuParityTest.
// The specification strategy (issue #975 §3) is the #706/#904 one: explicit
// C++ and GLSL implementations pinned by cross-language parity tests plus this
// header + THE ALPHA LEDGER as the formula ledger — extending the mechanism
// ReferenceBRDFGpuParityTest already is, not replacing it.
//
// Header-only, allocation-free, GL-free, sampler-agnostic: callers draw the
// random values (one lobe-select scalar + one 2D sample — the draw ORDER is
// part of the determinism contract) and pass them in.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <glm/glm.hpp>

#include <algorithm>

namespace OloEngine::PathTracing::BSDF
{
    // The result of one Sample() draw. Matches the issue-#975 contract sketch;
    // `Value` is f(l, v) WITHOUT the cosine, like every BRDF in ReferenceBRDF.h.
    struct BSDFSample
    {
        glm::vec3 Direction{ 0.0f };
        glm::vec3 Value{ 0.0f };
        f32 Pdf = 0.0f;
    };

    // Roughness used for Legacy IMPORTANCE SAMPLING and its density, floored at
    // PBRCommon's MIN_ROUGHNESS. The Legacy BRDF is still EVALUATED at the
    // material's raw roughness — the reference must reproduce what the renderer
    // computes, including the EPSILON-clamp lobe collapse below roughness
    // ~0.27. Widening only the sampling distribution leaves the estimator
    // unbiased (the sampled lobe strictly contains the evaluated one) while
    // keeping the density away from the numerical cliff at alpha -> 0.
    // (The v2 closure has no such split: ClosureV2Roughness is applied to
    // evaluation, sampling and density alike.)
    [[nodiscard]] inline f32 SamplingRoughness(f32 materialRoughness) noexcept
    {
        return std::clamp(materialRoughness, kMinRoughness, 1.0f);
    }

    // Lobe-selection probability for the one-sample mixture, shared by both
    // models (the v2 GLSL twin closureV2SpecularProbability is the same
    // formula). Zero for the Lambertian diagnostic so the diffuse lobe is the
    // only one ever drawn.
    [[nodiscard]] inline f32 SpecularProbability(const ReferenceMaterial& material) noexcept
    {
        if (material.LambertianDiffuseOnly)
            return 0.0f;
        return SpecularLobeProbability(material.BaseColor, material.Metallic);
    }

    // The single BRDF evaluation point for the whole integrator. The
    // Lambertian branch exists solely for the DDGI parity diagnostic (see
    // ReferenceMaterial::LambertianDiffuseOnly for why that is not a loophole)
    // and overrides the model dispatch.
    [[nodiscard]] inline glm::vec3 Evaluate(const ReferenceMaterial& material, const glm::vec3& n,
                                            const glm::vec3& v, const glm::vec3& l) noexcept
    {
        if (material.LambertianDiffuseOnly)
            return material.BaseColor * kInvPi;
        if (material.Model == PBRModel::ClosureV2)
            return ClosureV2Evaluate(n, v, l, material.BaseColor, material.Metallic, material.Roughness);
        return CookTorranceBRDF(n, v, l, material.BaseColor, material.Metallic, material.Roughness);
    }

    // Solid-angle density of the one-sample lobe mixture Sample() draws from.
    // Used by both the sampler (to divide by) and NEE (for the MIS weight).
    // Directions below the horizon report 0 in both models.
    [[nodiscard]] inline f32 Pdf(const ReferenceMaterial& material, const glm::vec3& n,
                                 const glm::vec3& v, const glm::vec3& l) noexcept
    {
        const f32 nDotL = glm::dot(n, l);
        if (nDotL <= 0.0f)
            return 0.0f;

        const f32 pdfDiffuse = PdfCosineHemisphere(nDotL);
        const f32 pSpecular = SpecularProbability(material);
        if (!(pSpecular > 0.0f))
            return pdfDiffuse;

        const glm::vec3 h = glm::normalize(v + l);
        const f32 nDotH = glm::dot(n, h);

        f32 pdfSpecular = 0.0f;
        if (material.Model == PBRModel::ClosureV2)
        {
            // GLSL twin: closureV2Pdf's specular term. Same clamped roughness
            // as the v2 sampler AND the v2 evaluation — one D for all three.
            pdfSpecular = PdfGGXVNDF(glm::dot(n, v), std::max(nDotH, 0.0f),
                                     ClosureV2Roughness(material.Roughness));
        }
        else
        {
            const f32 vDotH = glm::dot(v, h);
            pdfSpecular = PdfGGX(nDotH, vDotH, SamplingRoughness(material.Roughness));
        }

        return pSpecular * pdfSpecular + (1.0f - pSpecular) * pdfDiffuse;
    }

    // One-sample mixture draw. `lobeXi` selects the lobe, `xi` shapes it — the
    // caller owns the sampler and the dimension order (Get1D then Get2D; that
    // order is pinned by the bit-identical-render tests). Returns false for a
    // below-horizon or zero-density direction, in which case the path carries
    // no energy and the caller terminates it — identical semantics to the
    // former PathTracer-internal SampleBsdf.
    [[nodiscard]] inline bool Sample(const ReferenceMaterial& material, const glm::vec3& n,
                                     const glm::vec3& v, f32 lobeXi, const glm::vec2& xi,
                                     BSDFSample& outSample) noexcept
    {
        const f32 pSpecular = SpecularProbability(material);

        glm::vec3 l;
        if (lobeXi < pSpecular)
        {
            if (material.Model == PBRModel::ClosureV2)
            {
                // GLSL twin: closureV2SampleBRDF's specular branch — VNDF, on
                // the same clamped roughness the pdf and evaluation use.
                const glm::vec3 h = SampleGGXVNDF(n, v, ClosureV2Roughness(material.Roughness), xi);
                l = glm::reflect(-v, h);
            }
            else
            {
                const glm::vec3 h = ImportanceSampleGGX(xi, n, SamplingRoughness(material.Roughness));
                l = glm::reflect(-v, h);
            }
        }
        else
        {
            l = CosineSampleHemisphere(xi, n);
        }

        const f32 nDotL = glm::dot(n, l);
        if (nDotL <= 0.0f)
            return false;

        const f32 pdf = Pdf(material, n, v, l);
        if (!(pdf > 0.0f))
            return false;

        outSample.Direction = l;
        outSample.Value = Evaluate(material, n, v, l);
        outSample.Pdf = pdf;
        return true;
    }

} // namespace OloEngine::PathTracing::BSDF
