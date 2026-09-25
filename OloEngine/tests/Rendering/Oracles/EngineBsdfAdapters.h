#pragma once

// =============================================================================
// EngineBsdfAdapters.h — the engine's BSDF functions, adapted to the oracle
// checks' f64 callables (issue #1347).
// =============================================================================
//
// This is the ONE header in Rendering/Oracles/ that includes renderer code,
// and it is deliberately not an oracle: it is the implementation side of each
// check. Keeping the adapters here, rather than in each test file, means the
// identity, sampling and mutation tests all check the same engine calls — and
// a mutant in BsdfMutationDetectionTest is a copy of the adapter below with
// one thing changed, which is easy to review side by side.
//
// Everything is in the local frame n = +z, like the oracle; the adapters
// convert f64 <-> f32 at the boundary and nowhere else.
// =============================================================================

#include "Rendering/Oracles/BsdfOracleChecks.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirCore.h"

#include <glm/glm.hpp>

#include <optional>

namespace OloEngine::Tests::Oracle::Engine
{
    inline const glm::vec3 kNormal{ 0.0f, 0.0f, 1.0f };

    [[nodiscard]] inline glm::vec3 ToF32(const glm::dvec3& d)
    {
        return glm::vec3(d);
    }

    [[nodiscard]] inline glm::dvec3 ToF64(const glm::vec3& f)
    {
        return glm::dvec3(f);
    }

    [[nodiscard]] inline PathTracing::ReferenceMaterial ToReferenceMaterial(const MaterialCase& m, PBRModel model)
    {
        PathTracing::ReferenceMaterial material;
        material.BaseColor = ToF32(m.Albedo);
        material.Metallic = static_cast<f32>(m.Metallic);
        material.Roughness = static_cast<f32>(m.Roughness);
        material.Model = model;
        return material;
    }

    // ---- evaluation ---------------------------------------------------------

    // PathTracing::ClosureV2Evaluate — the C++ twin of PBRCommon.glsl
    // closureV2Evaluate (pinned to the shader by ReferenceBRDFGpuParityTest).
    [[nodiscard]] inline BrdfFn ClosureV2Brdf(const MaterialCase& m)
    {
        return [m](const glm::dvec3& v, const glm::dvec3& l)
        {
            return ToF64(PathTracing::ClosureV2Evaluate(kNormal, ToF32(v), ToF32(l), ToF32(m.Albedo),
                                                        static_cast<f32>(m.Metallic), static_cast<f32>(m.Roughness)));
        };
    }

    // PathTracing::CookTorranceBRDF — the Legacy lit-pass closure (twin of
    // PBRCommon.glsl cookTorranceBRDF).
    [[nodiscard]] inline BrdfFn LegacyBrdf(const MaterialCase& m)
    {
        return [m](const glm::dvec3& v, const glm::dvec3& l)
        {
            return ToF64(PathTracing::CookTorranceBRDF(kNormal, ToF32(v), ToF32(l), ToF32(m.Albedo),
                                                       static_cast<f32>(m.Metallic), static_cast<f32>(m.Roughness)));
        };
    }

    // ---- densities ----------------------------------------------------------

    // PathTracing::BSDF::Pdf — the mixture density the integrator divides by.
    [[nodiscard]] inline PdfFn BsdfPdf(const MaterialCase& m, PBRModel model)
    {
        const PathTracing::ReferenceMaterial material = ToReferenceMaterial(m, model);
        return [material](const glm::dvec3& v, const glm::dvec3& l)
        { return static_cast<f64>(PathTracing::BSDF::Pdf(material, kNormal, ToF32(v), ToF32(l))); };
    }

    // PathTracing::PdfGGXVNDF — the specular density of VNDF sampling, over l.
    [[nodiscard]] inline PdfFn VndfPdf(f64 roughness)
    {
        return [roughness](const glm::dvec3& v, const glm::dvec3& l)
        {
            const glm::vec3 vf = ToF32(v);
            const glm::vec3 lf = ToF32(l);
            if (lf.z <= 0.0f)
                return 0.0;
            const glm::vec3 h = glm::normalize(vf + lf);
            return static_cast<f64>(PathTracing::PdfGGXVNDF(vf.z, std::max(h.z, 0.0f), static_cast<f32>(roughness)));
        };
    }

    // PathTracing::PdfGGX — the density of reflecting v about an
    // ImportanceSampleGGX half-vector, over l.
    [[nodiscard]] inline PdfFn GgxReflectionPdf(f64 roughness)
    {
        return [roughness](const glm::dvec3& v, const glm::dvec3& l)
        {
            const glm::vec3 vf = ToF32(v);
            const glm::vec3 lf = ToF32(l);
            if (lf.z <= 0.0f)
                return 0.0;
            const glm::vec3 h = glm::normalize(vf + lf);
            return static_cast<f64>(PathTracing::PdfGGX(h.z, glm::dot(vf, h), static_cast<f32>(roughness)));
        };
    }

    // ---- samplers -----------------------------------------------------------

    // PathTracing::SampleGGXVNDF, reflected: the v2 specular lobe's draw.
    [[nodiscard]] inline SamplerFn VndfSampler(f64 roughness)
    {
        return [roughness](const glm::dvec3& v, IidStream& stream) -> std::optional<glm::dvec3>
        {
            const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
            const glm::vec3 vf = ToF32(v);
            const glm::vec3 h = PathTracing::SampleGGXVNDF(kNormal, vf, static_cast<f32>(roughness), xi);
            const glm::vec3 l = glm::reflect(-vf, h);
            if (l.z <= 0.0f)
                return std::nullopt;
            return ToF64(l);
        };
    }

    // PathTracing::ImportanceSampleGGX (a half-vector draw from D cos),
    // reflected about v: the Legacy specular lobe's draw.
    [[nodiscard]] inline SamplerFn GgxReflectionSampler(f64 roughness)
    {
        return [roughness](const glm::dvec3& v, IidStream& stream) -> std::optional<glm::dvec3>
        {
            const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
            const glm::vec3 vf = ToF32(v);
            const glm::vec3 h = PathTracing::ImportanceSampleGGX(xi, kNormal, static_cast<f32>(roughness));
            if (glm::dot(vf, h) <= 0.0f)
                return std::nullopt;
            const glm::vec3 l = glm::reflect(-vf, h);
            if (l.z <= 0.0f)
                return std::nullopt;
            return ToF64(l);
        };
    }

    // PathTracing::CosineSampleHemisphere.
    [[nodiscard]] inline SamplerFn CosineSampler()
    {
        return [](const glm::dvec3&, IidStream& stream) -> std::optional<glm::dvec3>
        {
            const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
            const glm::vec3 l = PathTracing::CosineSampleHemisphere(xi, kNormal);
            if (l.z <= 0.0f)
                return std::nullopt;
            return ToF64(l);
        };
    }

    // PathTracing::BSDF::Sample — the one-sample lobe mixture, with the draw
    // order the integrator uses (lobe select, then the 2D shape).
    [[nodiscard]] inline SamplerFn BsdfSampler(const MaterialCase& m, PBRModel model)
    {
        const PathTracing::ReferenceMaterial material = ToReferenceMaterial(m, model);
        return [material](const glm::dvec3& v, IidStream& stream) -> std::optional<glm::dvec3>
        {
            const auto lobeXi = static_cast<f32>(stream.Next());
            const glm::vec2 xi(static_cast<f32>(stream.Next()), static_cast<f32>(stream.Next()));
            PathTracing::BSDF::BSDFSample sample;
            if (!PathTracing::BSDF::Sample(material, kNormal, ToF32(v), lobeXi, xi, sample))
                return std::nullopt;
            return ToF64(sample.Direction);
        };
    }

    // The engine's lobe-selection policy. A POLICY, not a model property: the
    // mixture is unbiased for any value in (0, 1), so the oracle density of
    // BSDF::Sample takes it as an input rather than re-deriving it.
    [[nodiscard]] inline f64 SpecularProbability(const MaterialCase& m, PBRModel model)
    {
        return static_cast<f64>(PathTracing::BSDF::SpecularProbability(ToReferenceMaterial(m, model)));
    }

    // ---- path reuse ---------------------------------------------------------

    // ReSTIR::ReconnectionJacobian (ReservoirCore.h; GLSL twin
    // OloReconnectionJacobian in include/ReservoirCore.glsl).
    [[nodiscard]] inline JacobianFn ReconnectionJacobian()
    {
        return [](const glm::dvec3& vertex, const glm::dvec3& normal, const glm::dvec3& destination,
                  const glm::dvec3& source)
        {
            return static_cast<f64>(ReSTIR::ReconnectionJacobian(ToF32(vertex), ToF32(normal), ToF32(destination),
                                                                 ToF32(source)));
        };
    }
} // namespace OloEngine::Tests::Oracle::Engine
