#pragma once

// =============================================================================
// PBRClosureBSDF.h — the Evaluate / Sample / Pdf contract (issue #975)
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
// The GPU and CPU integrators sample the same VNDF and cosine mixture.
// Header-only, allocation-free, GL-free, sampler-agnostic: callers draw the
// random values (one lobe-select scalar + one 2D sample — the draw ORDER is
// part of the determinism contract) and pass them in.
// =============================================================================

#include "OloEngine/Core/Base.h"
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
        return ClosureV2Evaluate(n, v, l, material.BaseColor, material.Metallic, material.Roughness);
    }

    // Solid-angle density of the one-sample lobe mixture Sample() draws from.
    // Used by both the sampler (to divide by) and NEE (for the MIS weight).
    // Directions below the horizon report 0.
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

        const f32 pdfSpecular = PdfGGXVNDF(n, v, h, ClosureV2Roughness(material.Roughness));

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
            // Visible-normal sampling requires the view above the shading horizon.
            if (glm::dot(n, v) <= 0.0f)
                return false;
            const glm::vec3 h = SampleGGXVNDF(n, v, ClosureV2Roughness(material.Roughness), xi);
            l = glm::reflect(-v, h);
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
