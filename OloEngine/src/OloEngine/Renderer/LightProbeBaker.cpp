#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace OloEngine
{
    // Cubemap face directions: +X, -X, +Y, -Y, +Z, -Z
    static const glm::vec3 s_CubemapTargets[6] = {
        { 1.0f, 0.0f, 0.0f },
        { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -1.0f }
    };

    static const glm::vec3 s_CubemapUps[6] = {
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }
    };

    bool LightProbeBaker::RenderCubemapAtPosition(
        Ref<Scene>& scene,
        const glm::vec3& position,
        u32 resolution,
        std::vector<glm::vec3>& outPixels)
    {
        OLO_PROFILE_FUNCTION();

        auto const totalPixels = static_cast<size_t>(resolution) * resolution * 6;
        outPixels.resize(totalPixels);

        FramebufferSpecification spec;
        spec.Width = resolution;
        spec.Height = resolution;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::Depth };
        auto fbo = Framebuffer::Create(spec);

        Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

        // Temporary buffer for RGBA16F readback (4 floats per pixel)
        std::vector<f32> rgbaBuffer(static_cast<size_t>(resolution) * resolution * 4);

        for (u32 face = 0; face < 6; ++face)
        {
            glm::mat4 const view = glm::lookAt(position, position + s_CubemapTargets[face], s_CubemapUps[face]);
            glm::mat4 const transform = glm::inverse(view);

            fbo->Bind();
            RenderCommand::SetViewport(0, 0, resolution, resolution);
            fbo->ClearAllAttachments(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

            // Render the full scene from this cubemap face's perspective
            scene->RenderScene3D(captureCamera, transform);

            // Read back RGBA16F pixel data from the color attachment, by
            // identity rather than driver name (issue #691 step 3).
            RHI::ResourceHandle const colorAttachment = fbo->GetColorAttachmentHandle(0);
            const bool readOk = RenderCommand::ReadTextureImage(
                colorAttachment, 0, RHI::Format::RGBA32Float,
                rgbaBuffer.size() * sizeof(f32), rgbaBuffer.data());

            fbo->Unbind();

            if (!readOk)
            {
                // Fail the whole bake rather than folding an unspecified buffer
                // into the SH projection: these coefficients are PERSISTED into
                // LightProbeVolumeAsset, so a rare readback failure would write
                // bad lighting to disk that no later run would recompute.
                OLO_CORE_ERROR("LightProbeBaker: cubemap face {} readback failed; abandoning this probe", face);
                return false;
            }

            // Convert RGBA to RGB and store
            auto const faceOffset = static_cast<size_t>(face) * resolution * resolution;
            for (size_t i = 0; i < static_cast<size_t>(resolution) * resolution; ++i)
            {
                outPixels[faceOffset + i] = glm::vec3(
                    rgbaBuffer[i * 4 + 0],
                    rgbaBuffer[i * 4 + 1],
                    rgbaBuffer[i * 4 + 2]);
            }
        }

        return true;
    }

    SHCoefficients LightProbeBaker::ProjectToSH(
        const std::vector<glm::vec3>& cubemapPixels,
        u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        SHCoefficients result;
        result.Zero();

        if (resolution == 0)
        {
            OLO_CORE_ERROR("LightProbeBaker::ProjectToSH: resolution must be > 0");
            return result;
        }

        if (auto const expectedPixels = static_cast<size_t>(6) * resolution * resolution; cubemapPixels.size() < expectedPixels)
        {
            OLO_CORE_ERROR("LightProbeBaker::ProjectToSH: cubemapPixels size {} < expected {}", cubemapPixels.size(), expectedPixels);
            return result;
        }

        f32 totalWeight = 0.0f;
        f32 const texelSize = 2.0f / static_cast<f32>(resolution);

        for (u32 face = 0; face < 6; ++face)
        {
            for (u32 y = 0; y < resolution; ++y)
            {
                for (u32 x = 0; x < resolution; ++x)
                {
                    // Map texel to [-1, 1] range
                    f32 const u = (static_cast<f32>(x) + 0.5f) * texelSize - 1.0f;
                    f32 const v = (static_cast<f32>(y) + 0.5f) * texelSize - 1.0f;

                    // Convert to world direction based on face
                    glm::vec3 dir(0.0f);
                    switch (face)
                    {
                        case 0:
                            dir = glm::vec3(1.0f, -v, -u);
                            break; // +X
                        case 1:
                            dir = glm::vec3(-1.0f, -v, u);
                            break; // -X
                        case 2:
                            dir = glm::vec3(u, 1.0f, v);
                            break; // +Y
                        case 3:
                            dir = glm::vec3(u, -1.0f, -v);
                            break; // -Y
                        case 4:
                            dir = glm::vec3(u, -v, 1.0f);
                            break; // +Z
                        case 5:
                            dir = glm::vec3(-u, -v, -1.0f);
                            break; // -Z
                    }
                    dir = glm::normalize(dir);

                    // Solid angle approximation for cubemap texel
                    f32 const distSq = u * u + v * v + 1.0f;
                    f32 const weight = 4.0f / (std::sqrt(distSq) * distSq);

                    auto const idx = static_cast<size_t>(face) * resolution * resolution + static_cast<size_t>(y) * resolution + x;
                    glm::vec3 const color = cubemapPixels[idx];

                    // Evaluate SH basis and accumulate
                    auto basis = SHBasis::Evaluate(dir);
                    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
                    {
                        result.Coefficients[i] += color * (basis[i] * weight);
                    }
                    totalWeight += weight;
                }
            }
        }

        // Normalize
        if (totalWeight > 0.0f)
        {
            f32 const norm = (4.0f * glm::pi<f32>()) / totalWeight;
            result.Scale(norm);
        }

        return result;
    }

    SHCoefficients LightProbeBaker::BakeProbeAtPosition(
        Ref<Scene>& scene,
        const glm::vec3& position,
        u32 cubemapResolution,
        bool* outValid)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<glm::vec3> pixels;
        if (!RenderCubemapAtPosition(scene, position, cubemapResolution, pixels))
        {
            // Readback failed — report the probe as invalid so the caller stores
            // nothing rather than persisting SH derived from undefined pixels.
            if (outValid)
            {
                *outValid = false;
            }
            return {};
        }

        SHCoefficients sh = ProjectToSH(pixels, cubemapResolution);

        if (outValid)
        {
            // Simple heuristic: if the probe captures mostly black (inside geometry),
            // mark as invalid
            f32 energy = glm::dot(sh.Coefficients[0], glm::vec3(1.0f));
            *outValid = energy > 0.001f;
        }

        return sh;
    }

    void LightProbeBaker::BakeVolume(
        Ref<Scene>& scene,
        LightProbeVolumeComponent& volume,
        Ref<LightProbeVolumeAsset>& asset,
        u32 cubemapResolution,
        const ProbeBakeProgressCallback& progress)
    {
        OLO_PROFILE_FUNCTION();

        if (!asset)
        {
            return;
        }

        // Sync asset parameters from component
        asset->BoundsMin = volume.m_BoundsMin;
        asset->BoundsMax = volume.m_BoundsMax;
        asset->Resolution = volume.m_Resolution;
        asset->Spacing = volume.m_Spacing;
        asset->AllocateCoefficients();

        i32 const totalProbes = volume.GetTotalProbeCount();
        glm::vec3 const extent = volume.m_BoundsMax - volume.m_BoundsMin;

        for (i32 z = 0; z < volume.m_Resolution.z; ++z)
        {
            for (i32 y = 0; y < volume.m_Resolution.y; ++y)
            {
                for (i32 x = 0; x < volume.m_Resolution.x; ++x)
                {
                    // Calculate probe world position
                    glm::vec3 t(0.0f);
                    if (volume.m_Resolution.x > 1)
                        t.x = static_cast<f32>(x) / static_cast<f32>(volume.m_Resolution.x - 1);
                    if (volume.m_Resolution.y > 1)
                        t.y = static_cast<f32>(y) / static_cast<f32>(volume.m_Resolution.y - 1);
                    if (volume.m_Resolution.z > 1)
                        t.z = static_cast<f32>(z) / static_cast<f32>(volume.m_Resolution.z - 1);

                    glm::vec3 const probePos = volume.m_BoundsMin + extent * t;

                    bool valid = true;
                    SHCoefficients sh = BakeProbeAtPosition(scene, probePos, cubemapResolution, &valid);

                    i32 const linearIdx = volume.GridIndex(x, y, z);
                    asset->SetProbeData(linearIdx, sh, valid ? 1.0f : 0.0f);

                    if (progress)
                    {
                        progress(linearIdx + 1, totalProbes);
                    }
                }
            }
        }

        volume.m_Dirty = true;
    }

    // =========================================================================
    // Path-traced probe bake (issue #439)
    //
    // SH CONVENTION — measured from the existing pipeline and matched exactly.
    // ProjectToSH() above stores the RAW RADIANCE PROJECTION of the incident
    // field:
    //     c_i = ∫ L(ω) · Y_i(ω) dω        (no cosine convolution, no 1/π)
    // and the shader side (SphericalHarmonics.glsl::evaluateSH, consumed by
    // LightProbeSampling.glsl::sampleLightProbeGrid) reconstructs
    //     Σ c_i · Y_i(n)
    // — the band-limited RADIANCE arriving from direction n, NOT the
    // irradiance E(n). For a uniform field of radiance L the shader returns L
    // where true irradiance is π·L; band-wise, E(n) would need the cosine-lobe
    // convolution factors Â_0 = π, Â_1 = 2π/3, Â_2 = π/4 applied per
    // coefficient (Ramamoorthi & Hanrahan 2001), which neither side of this
    // pipeline applies. This bake REPLICATES that convention so its output is
    // photometrically interchangeable with the cubemap route's for the same
    // incident light field (light-path-photometric-parity rule 1: matching the
    // shipped raster path beats textbook correctness — the two bake buttons
    // must not light the same scene differently). The divergence from the
    // DDGI atlas and the scene lightmap (both store FULL irradiance E — see
    // DDGI_BlendIrradiance.glsl / LightmapSampling.glsl headers and
    // docs/agent-rules/reference-path-tracer.md §4) is therefore PRE-EXISTING
    // in the baked-SH path and is deliberately not fixed here: rescaling
    // means touching BOTH bake routes and every already-baked scene at once.
    //
    // THE ESTIMATOR: N uniform-sphere directions (pdf = 1/4π) from the probe
    // position, full TracePath radiance along each, Monte Carlo projection
    //     c_i ≈ (4π/N) · Σ_s L(d_s) · Y_i(d_s).
    // Deterministic: each sample s draws from the stateless Owen-scrambled
    // Sobol' PathSampler(probeSeed, s) — direction from the first 2D
    // dimension, the path's scatter decisions from the following dimensions,
    // mirroring EstimateIrradiance's layout — and samples are accumulated in
    // ascending order WITHIN each probe, so two bakes are bit-identical
    // regardless of how probes are scheduled across threads.
    //
    // The sphere directions come from the shared
    // PathTracing::UniformSampleSphere (ReferenceBRDF.h) — bit-identical to
    // the local copy this file used to carry (kTwoPi rounds to the same f32 as
    // 2.0f * glm::pi<f32>()). ToUnitFloat keeps xi < 1, so z ∈ (-1, 1] and
    // the result is always unit length (SHBasis::Evaluate asserts that).
    // =========================================================================

    SHCoefficients LightProbeBaker::BakeProbeAtPositionPathTraced(
        const PathTracing::ReferenceScene& world,
        const glm::vec3& position,
        const LightProbePathTracedBakeSettings& settings,
        u32 probeSeed,
        bool* outValid)
    {
        OLO_PROFILE_FUNCTION();

        SHCoefficients result;
        result.Zero();
        if (outValid)
        {
            *outValid = false;
        }

        if (!world.IsBuilt() || settings.SamplesPerProbe == 0)
        {
            OLO_CORE_ERROR("LightProbeBaker::BakeProbeAtPositionPathTraced: world must be Build()t and SamplesPerProbe > 0");
            return result;
        }

        PathTracing::PathTracerSettings tracer;
        tracer.MaxBounces = settings.MaxBounces;
        // 0 disables Russian roulette: deterministic cost, lower variance at
        // these short depths (mirrors LightmapBaker::BakeTexels).
        tracer.RussianRouletteStartBounce = 0;
        tracer.Seed = settings.Seed; // TracePath itself reads no seed (the sampler carries it); recorded for completeness

        // f64 accumulation in fixed ascending sample order: better-conditioned
        // sums at high sample counts, still bit-reproducible.
        std::array<glm::dvec3, SH_COEFFICIENT_COUNT> sums{};

        for (u32 sample = 0; sample < settings.SamplesPerProbe; ++sample)
        {
            PathTracing::PathSampler sampler(probeSeed, sample);
            glm::vec3 const direction = PathTracing::UniformSampleSphere(sampler.Get2D());
            // The probe floats in air — no surface, so no normal-offset
            // epsilon at the origin (unlike EstimateIrradiance's shading
            // point).
            Ray const ray(position, direction, 0.0f, std::numeric_limits<f32>::max());
            glm::vec3 const radiance = PathTracing::PathTracer::TracePath(world, ray, tracer, sampler);

            auto const basis = SHBasis::Evaluate(direction);
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                sums[i] += glm::dvec3(radiance) * static_cast<f64>(basis[i]);
            }
        }

        f64 const scale = (4.0 * glm::pi<f64>()) / static_cast<f64>(settings.SamplesPerProbe);
        for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
        {
            result.Coefficients[i] = glm::vec3(sums[i] * scale);
        }

        if (outValid)
        {
            // Same heuristic and threshold as BakeProbeAtPosition: a probe
            // that captured (nearly) no energy is treated as buried inside
            // geometry and flagged invalid so the sampler skips it.
            f32 const energy = glm::dot(result.Coefficients[0], glm::vec3(1.0f));
            *outValid = energy > 0.001f;
        }

        return result;
    }

    bool LightProbeBaker::BakeVolumePathTraced(
        const PathTracing::ReferenceScene& world,
        LightProbeVolumeComponent& volume,
        Ref<LightProbeVolumeAsset>& asset,
        const LightProbePathTracedBakeSettings& settings,
        const ProbeBakeProgressCallback& progress)
    {
        OLO_PROFILE_FUNCTION();

        if (!asset)
        {
            OLO_CORE_ERROR("LightProbeBaker::BakeVolumePathTraced: asset is null");
            return false;
        }
        if (!world.IsBuilt())
        {
            OLO_CORE_ERROR("LightProbeBaker::BakeVolumePathTraced: ReferenceScene must be Build()t before baking");
            return false;
        }
        if (settings.SamplesPerProbe == 0)
        {
            OLO_CORE_ERROR("LightProbeBaker::BakeVolumePathTraced: SamplesPerProbe must be > 0");
            return false;
        }

        // Sync asset parameters from component — identical to BakeVolume.
        asset->BoundsMin = volume.m_BoundsMin;
        asset->BoundsMax = volume.m_BoundsMax;
        asset->Resolution = volume.m_Resolution;
        asset->Spacing = volume.m_Spacing;
        asset->AllocateCoefficients();

        i32 const totalProbes = volume.GetTotalProbeCount();
        if (totalProbes <= 0 ||
            asset->CoefficientData.size() != static_cast<size_t>(totalProbes) * SH_COEFFICIENT_COUNT)
        {
            OLO_CORE_ERROR("LightProbeBaker::BakeVolumePathTraced: empty or over-budget probe grid ({} probes)", totalProbes);
            return false;
        }

        glm::vec3 const extent = volume.m_BoundsMax - volume.m_BoundsMin;
        glm::ivec3 const res = volume.m_Resolution;

        // Probes are independent (per-probe seeds, per-probe asset slots, no
        // shared sampler state — BakeProbeAtPositionPathTraced builds a fresh
        // stateless PathSampler per sample), so the outer loop parallelizes
        // over engine tasks. Determinism is untouched: each probe's value
        // depends only on its seed and the built world, never on scheduling.
        // MinBatchSize 1 because a single probe is SamplesPerProbe full paths.
        std::atomic<i32> completedProbes{ 0 };
        std::mutex progressMutex;
        constexpr i32 kProbeLogInterval = 64;

        ParallelFor(
            "LightProbeBaker::BakeVolumePathTraced",
            totalProbes,
            1,
            [&](i32 probeIndex)
            {
                // Decompose the z-major linear index (the GridIndex
                // convention: idx = z*ny*nx + y*nx + x), so probeIndex IS the
                // linear grid index the seed contract keys on.
                i32 const x = probeIndex % res.x;
                i32 const y = (probeIndex / res.x) % res.y;
                i32 const z = probeIndex / (res.x * res.y);

                // Probe world position — the SAME derivation as BakeVolume
                // (bounds are authored in world space; single-slice axes
                // sit at BoundsMin), so the two bake modes place every
                // probe identically.
                glm::vec3 t(0.0f);
                if (res.x > 1)
                    t.x = static_cast<f32>(x) / static_cast<f32>(res.x - 1);
                if (res.y > 1)
                    t.y = static_cast<f32>(y) / static_cast<f32>(res.y - 1);
                if (res.z > 1)
                    t.z = static_cast<f32>(z) / static_cast<f32>(res.z - 1);

                glm::vec3 const probePos = volume.m_BoundsMin + extent * t;

                // Per-probe sampler seed from the linear grid index — the
                // bake's determinism contract.
                u32 const probeSeed = PathTracing::MakePixelSeed(static_cast<u32>(probeIndex), 0u, settings.Seed);

                bool valid = true;
                SHCoefficients const sh = BakeProbeAtPositionPathTraced(world, probePos, settings, probeSeed, &valid);
                asset->SetProbeData(probeIndex, sh, valid ? 1.0f : 0.0f);

                if (progress)
                {
                    // Increment under the lock so the callback sees a strictly
                    // increasing completed count with the final call carrying
                    // (totalProbes, totalProbes), whatever the thread timing.
                    std::scoped_lock lock(progressMutex);
                    i32 const done = completedProbes.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done % kProbeLogInterval == 0 || done == totalProbes)
                        OLO_CORE_INFO("LightProbeBaker: path-traced bake completed {}/{} probes", done, totalProbes);
                    progress(done, totalProbes);
                }
                else
                {
                    i32 const done = completedProbes.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done % kProbeLogInterval == 0 || done == totalProbes)
                        OLO_CORE_INFO("LightProbeBaker: path-traced bake completed {}/{} probes", done, totalProbes);
                }
            });

        volume.m_Dirty = true;
        return true;
    }
} // namespace OloEngine
