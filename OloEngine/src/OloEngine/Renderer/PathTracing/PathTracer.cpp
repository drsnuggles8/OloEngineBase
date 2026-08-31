#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/PathTracer.h"

#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Task/ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OloEngine::PathTracing
{
    namespace
    {
        // The BSDF bodies that used to live here are now the versioned
        // Evaluate/Sample/Pdf contract in PBRClosureBSDF.h (issue #975) — the
        // Legacy branch is those bodies moved verbatim, so the bit-identical
        // render hashes are unchanged. These thin wrappers keep the
        // integrator's call sites and, in SampleBsdf, own the sampler
        // DIMENSION DRAW ORDER (Get1D then Get2D, unconditionally), which is
        // part of the determinism contract.
        [[nodiscard]] glm::vec3 EvaluateBRDF(const ReferenceMaterial& material, const glm::vec3& n,
                                             const glm::vec3& v, const glm::vec3& l)
        {
            return BSDF::Evaluate(material, n, v, l);
        }

        [[nodiscard]] f32 BsdfPdf(const glm::vec3& n, const glm::vec3& v, const glm::vec3& l,
                                  const ReferenceMaterial& material)
        {
            return BSDF::Pdf(material, n, v, l);
        }

        using BsdfSample = BSDF::BSDFSample;

        [[nodiscard]] bool SampleBsdf(const glm::vec3& n, const glm::vec3& v,
                                      const ReferenceMaterial& material, PathSampler& sampler,
                                      BsdfSample& outSample)
        {
            const f32 lobeSelect = sampler.Get1D();
            const glm::vec2 xi = sampler.Get2D();
            return BSDF::Sample(material, n, v, lobeSelect, xi, outSample);
        }

        // Offset a ray origin off the surface along the geometric normal, on
        // whichever side the outgoing direction leaves. Using the GEOMETRIC
        // normal (not the shading one) is what actually prevents self-hits: an
        // interpolated normal can point into the triangle plane.
        [[nodiscard]] glm::vec3 OffsetOrigin(const glm::vec3& position, const glm::vec3& geometricNormal,
                                             const glm::vec3& direction, f32 epsilon)
        {
            const f32 sign = glm::dot(geometricNormal, direction) >= 0.0f ? 1.0f : -1.0f;
            return position + geometricNormal * (sign * epsilon);
        }

        // Direct lighting at a shading point: every punctual light (each gets
        // its own shadow ray — deterministic and low-variance for the small
        // scenes this instrument targets) plus ONE MIS-weighted sample of the
        // emissive geometry.
        [[nodiscard]] glm::vec3 SampleDirectLighting(const ReferenceScene& scene, const glm::vec3& position,
                                                     const glm::vec3& geometricNormal, const glm::vec3& n,
                                                     const glm::vec3& v, const ReferenceMaterial& material,
                                                     const PathTracerSettings& settings, PathSampler& sampler)
        {
            glm::vec3 direct(0.0f);

            for (const ReferenceLight& light : scene.GetLights())
            {
                glm::vec3 l(0.0f);
                f32 attenuation = 1.0f;
                glm::vec3 shadowTarget(0.0f);

                switch (light.Type)
                {
                    case ReferenceLightType::Directional:
                    {
                        l = glm::normalize(-light.Direction);
                        // A directional light is infinitely far away; probe far
                        // enough to leave the scene.
                        const BoundingBox worldBounds = scene.GetWorldBounds();
                        const f32 reach = glm::length(worldBounds.GetSize()) + 1.0f;
                        shadowTarget = position + l * (2.0f * reach);
                        break;
                    }
                    case ReferenceLightType::Point:
                    {
                        const glm::vec3 toLight = light.Position - position;
                        const f32 distance = glm::length(toLight);
                        if (!(distance > 0.0f))
                            continue;
                        l = toLight / distance;
                        attenuation = CalculateAttenuation(light.Position, position, light.AttenuationParams);
                        shadowTarget = light.Position;
                        break;
                    }
                    case ReferenceLightType::Spot:
                    {
                        const glm::vec3 toLight = light.Position - position;
                        const f32 distance = glm::length(toLight);
                        if (!(distance > 0.0f))
                            continue;
                        l = toLight / distance;
                        attenuation = CalculateAttenuation(light.Position, position, light.AttenuationParams);
                        attenuation *= CalculateSpotIntensity(l, light.Direction, light.SpotParams);
                        shadowTarget = light.Position;
                        break;
                    }
                }

                // Mirrors calculateLightContribution's early-outs, epsilon and
                // all, so the reference and the raster path agree on which
                // lights contribute at all.
                if (attenuation <= kEpsilon)
                    continue;

                const f32 nDotL = std::max(glm::dot(n, l), 0.0f);
                if (nDotL <= kEpsilon)
                    continue;

                const glm::vec3 shadowOrigin = OffsetOrigin(position, geometricNormal, l, settings.RayEpsilon);
                if (scene.IsOccluded(shadowOrigin, shadowTarget, settings.RayEpsilon))
                    continue;

                const glm::vec3 radiance = light.Color * light.Intensity * attenuation;
                const glm::vec3 brdf = EvaluateBRDF(material, n, v, l);
                direct += brdf * radiance * nDotL;
            }

            // ---- emissive geometry, area-sampled, MIS-weighted --------------
            //
            // The dimensions below are consumed UNCONDITIONALLY when the scene
            // has emitters, even if the sample is then rejected, so the sampler
            // dimension index stays aligned across samples of the same pixel.
            if (!scene.GetEmissiveTriangles().empty())
            {
                const f32 xiSelect = sampler.Get1D();
                const glm::vec2 xiPoint = sampler.Get2D();

                ReferenceScene::EmissiveSample lightSample;
                if (scene.SampleEmissive(xiSelect, xiPoint, lightSample) && lightSample.PdfArea > 0.0f)
                {
                    const glm::vec3 toLight = lightSample.Position - position;
                    const f32 distanceSq = glm::dot(toLight, toLight);
                    if (distanceSq > 1e-12f)
                    {
                        const f32 distance = std::sqrt(distanceSq);
                        const glm::vec3 l = toLight / distance;
                        const f32 nDotL = glm::dot(n, l);
                        const f32 cosLight = glm::dot(lightSample.Normal, -l);
                        const f32 effectiveCosLight = lightSample.TwoSided ? std::abs(cosLight) : cosLight;

                        if (nDotL > 0.0f && effectiveCosLight > 0.0f)
                        {
                            // Area -> solid-angle change of variables.
                            const f32 pdfSolidAngle = lightSample.PdfArea * distanceSq / effectiveCosLight;
                            if (pdfSolidAngle > 0.0f)
                            {
                                const glm::vec3 shadowOrigin =
                                    OffsetOrigin(position, geometricNormal, l, settings.RayEpsilon);
                                if (!scene.IsOccluded(shadowOrigin, lightSample.Position, settings.RayEpsilon))
                                {
                                    const glm::vec3 brdf = EvaluateBRDF(material, n, v, l);
                                    const f32 pdfBsdf = BsdfPdf(n, v, l, material);
                                    const f32 misWeight = PowerHeuristic(pdfSolidAngle, pdfBsdf);
                                    direct += brdf * nDotL * lightSample.Radiance * (misWeight / pdfSolidAngle);
                                }
                            }
                        }
                    }
                }
            }

            return direct;
        }
    } // namespace

    // =========================================================================
    // ReferenceCamera
    // =========================================================================

    ReferenceCamera ReferenceCamera::FromViewProjection(const glm::mat4& viewProjection, const glm::vec3& position)
    {
        ReferenceCamera camera;
        camera.InverseViewProjection = glm::inverse(viewProjection);
        camera.Position = position;
        return camera;
    }

    Ray ReferenceCamera::GenerateRay(const glm::vec2& screenUV) const
    {
        // Screen UV has y DOWN (row 0 == top); clip space has y UP.
        const f32 ndcX = screenUV.x * 2.0f - 1.0f;
        const f32 ndcY = 1.0f - screenUV.y * 2.0f;

        // Unproject at the MIDDLE of the depth range rather than at a clip
        // plane, then aim from the eye through it. That is robust to the depth
        // convention (standard [-1,1] or reverse-Z): a near/far difference
        // vector would point backwards under one of them, and the resulting
        // image would be an unnoticed 180-degree flip.
        const glm::vec4 unprojected = InverseViewProjection * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
        if (!(std::abs(unprojected.w) > 0.0f))
            return Ray(Position, glm::vec3(0.0f, 0.0f, -1.0f));

        const glm::vec3 target = glm::vec3(unprojected) / unprojected.w;
        const glm::vec3 delta = target - Position;
        const f32 length = glm::length(delta);
        if (!(length > 0.0f))
            return Ray(Position, glm::vec3(0.0f, 0.0f, -1.0f));

        return Ray(Position, delta / length);
    }

    // =========================================================================
    // ReferenceFilm
    // =========================================================================

    ReferenceFilm::ReferenceFilm(u32 width, u32 height)
    {
        Resize(width, height);
    }

    void ReferenceFilm::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
        m_Pixels.assign(static_cast<sizet>(width) * height, glm::vec3(0.0f));
    }

    void ReferenceFilm::Clear()
    {
        std::fill(m_Pixels.begin(), m_Pixels.end(), glm::vec3(0.0f));
    }

    glm::vec3 ReferenceFilm::GetPixel(u32 x, u32 y) const
    {
        if (x >= m_Width || y >= m_Height)
            return glm::vec3(0.0f);
        return m_Pixels[static_cast<sizet>(y) * m_Width + x];
    }

    glm::vec3 ReferenceFilm::MeanRadiance(u32 x0, u32 y0, u32 x1, u32 y1) const
    {
        if (m_Width == 0 || m_Height == 0)
            return glm::vec3(0.0f);

        const u32 xEnd = std::min(x1, m_Width - 1);
        const u32 yEnd = std::min(y1, m_Height - 1);
        if (x0 > xEnd || y0 > yEnd)
            return glm::vec3(0.0f);

        glm::dvec3 sum(0.0);
        u64 count = 0;
        for (u32 y = y0; y <= yEnd; ++y)
        {
            for (u32 x = x0; x <= xEnd; ++x)
            {
                sum += glm::dvec3(m_Pixels[static_cast<sizet>(y) * m_Width + x]);
                ++count;
            }
        }
        if (count == 0)
            return glm::vec3(0.0f);
        return glm::vec3(sum / static_cast<f64>(count));
    }

    void ReferenceFilm::EncodeRgba8(std::vector<u8>& outRgba, i32 tonemap, f32 exposure, bool applyGamma) const
    {
        outRgba.assign(static_cast<sizet>(m_Width) * m_Height * 4, 0);
        for (sizet i = 0; i < m_Pixels.size(); ++i)
        {
            glm::vec3 color = m_Pixels[i] * exposure;

            switch (tonemap)
            {
                case 1:
                    color = ReinhardToneMapping(color);
                    break;
                case 2:
                    color = AcesToneMapping(color);
                    break;
                default:
                    color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
                    break;
            }

            if (applyGamma)
                color = LinearToSRGB(color);

            color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
            outRgba[i * 4 + 0] = static_cast<u8>(std::lround(color.r * 255.0f));
            outRgba[i * 4 + 1] = static_cast<u8>(std::lround(color.g * 255.0f));
            outRgba[i * 4 + 2] = static_cast<u8>(std::lround(color.b * 255.0f));
            outRgba[i * 4 + 3] = 255;
        }
    }

    u64 ReferenceFilm::ComputeHash() const
    {
        // FNV-1a over the raw float bits. Not a checksum of a rounded image:
        // the determinism contract is bitwise.
        u64 hash = 1469598103934665603ull;
        const auto mix = [&hash](u32 word)
        {
            for (u32 byte = 0; byte < 4; ++byte)
            {
                hash ^= static_cast<u64>((word >> (byte * 8)) & 0xFFu);
                hash *= 1099511628211ull;
            }
        };

        mix(m_Width);
        mix(m_Height);
        for (const glm::vec3& pixel : m_Pixels)
        {
            for (glm::length_t c = 0; c < 3; ++c)
            {
                u32 bits = 0;
                const f32 value = pixel[c];
                std::memcpy(&bits, &value, sizeof(bits));
                mix(bits);
            }
        }
        return hash;
    }

    // =========================================================================
    // PathTracer
    // =========================================================================

    glm::vec3 PathTracer::TracePath(const ReferenceScene& scene, const Ray& primaryRay,
                                    const PathTracerSettings& settings, PathSampler& sampler)
    {
        glm::vec3 radiance(0.0f);
        glm::vec3 throughput(1.0f);
        Ray ray = primaryRay;

        // The camera ray is treated as a "delta" scatter: an emitter seen
        // directly is added at full weight because NEE never had a chance to
        // sample it for this vertex.
        bool previousScatterWasDelta = true;
        f32 previousBsdfPdf = 0.0f;

        const bool hasEmissiveGeometry = !scene.GetEmissiveTriangles().empty();
        const bool neeSamplesEmitters = settings.EnableNextEventEstimation && hasEmissiveGeometry;

        for (u32 bounce = 0; bounce < settings.MaxBounces; ++bounce)
        {
            SurfaceInteraction hit;
            if (!scene.Intersect(ray, hit))
            {
                // The environment is uniform and is never NEE-sampled, so it
                // always arrives at full weight.
                radiance += throughput * scene.GetEnvironment().Radiance;
                break;
            }

            const ReferenceMaterial& material = scene.GetMaterial(hit.MaterialIndex);
            const glm::vec3 v = -ray.Direction;

            // ---- emitted radiance --------------------------------------------
            const f32 cosEmitter = glm::dot(hit.GeometricNormal, v);
            const bool emitterFaceVisible = material.TwoSidedEmission ? (std::abs(cosEmitter) > 0.0f)
                                                                      : (cosEmitter > 0.0f);
            if (emitterFaceVisible && std::max({ material.Emissive.x, material.Emissive.y, material.Emissive.z }) > 0.0f)
            {
                f32 misWeight = 1.0f;
                if (!previousScatterWasDelta && neeSamplesEmitters)
                {
                    // This vertex could also have been reached by the NEE
                    // sample taken at the PREVIOUS vertex; weight the two
                    // strategies with the same densities NEE used.
                    const f32 effectiveCos = material.TwoSidedEmission ? std::abs(cosEmitter) : cosEmitter;
                    if (effectiveCos > 0.0f)
                    {
                        const f32 pdfLightSolidAngle =
                            scene.EmissivePdfArea() * (hit.Distance * hit.Distance) / effectiveCos;
                        misWeight = PowerHeuristic(previousBsdfPdf, pdfLightSolidAngle);
                    }
                }
                radiance += throughput * material.Emissive * misWeight;
            }

            // ---- shading frame ------------------------------------------------
            // Two-sided shading: flip the normal to the side the viewer is on,
            // exactly like a two-sided raster material. Both normals flip
            // together so the ray-offset side stays consistent with shading.
            glm::vec3 shadingNormal = hit.ShadingNormal;
            glm::vec3 geometricNormal = hit.GeometricNormal;
            if (glm::dot(geometricNormal, v) < 0.0f)
            {
                shadingNormal = -shadingNormal;
                geometricNormal = -geometricNormal;
            }

            // ---- next-event estimation ---------------------------------------
            if (settings.EnableNextEventEstimation)
            {
                radiance += throughput * SampleDirectLighting(scene, hit.Position, geometricNormal, shadingNormal,
                                                              v, material, settings, sampler);
            }

            // Last allowed vertex: stop before scattering. (MaxBounces == 1 is
            // therefore "direct lighting only".)
            if (bounce + 1 >= settings.MaxBounces)
                break;

            // ---- BSDF sample --------------------------------------------------
            BsdfSample bsdf;
            if (!SampleBsdf(shadingNormal, v, material, sampler, bsdf))
                break;

            const f32 nDotL = glm::dot(shadingNormal, bsdf.Direction);
            throughput *= bsdf.Value * nDotL / bsdf.Pdf;
            previousBsdfPdf = bsdf.Pdf;
            previousScatterWasDelta = false;

            if (!(std::max({ throughput.x, throughput.y, throughput.z }) > 0.0f))
                break;

            // ---- Russian roulette ---------------------------------------------
            if (settings.RussianRouletteStartBounce > 0 && bounce + 1 >= settings.RussianRouletteStartBounce)
            {
                const f32 survival = std::clamp(std::max({ throughput.x, throughput.y, throughput.z }), 0.05f, 0.95f);
                if (sampler.Get1D() >= survival)
                    break;
                throughput /= survival;
            }

            ray = Ray(OffsetOrigin(hit.Position, geometricNormal, bsdf.Direction, settings.RayEpsilon),
                      bsdf.Direction, 0.0f, std::numeric_limits<f32>::max());
        }

        if (settings.MaxRadianceClamp > 0.0f)
            radiance = glm::min(radiance, glm::vec3(settings.MaxRadianceClamp));

        // A NaN here would propagate through the whole accumulation buffer and
        // silently poison every region mean computed from it. Drop the sample
        // instead — and note it is a *drop*, so a scene that produces them will
        // read as too dark rather than as garbage.
        if (!std::isfinite(radiance.x) || !std::isfinite(radiance.y) || !std::isfinite(radiance.z))
            return glm::vec3(0.0f);

        return radiance;
    }

    void PathTracer::Render(const ReferenceScene& scene, const ReferenceCamera& camera,
                            const PathTracerSettings& settings, ReferenceFilm& film)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene.IsBuilt())
        {
            OLO_CORE_ERROR("PathTracer::Render: scene was never Build()t — nothing to trace");
            return;
        }

        // The film's extent IS the render resolution (settings carries no
        // width/height), so a default-constructed film renders nothing. Say so:
        // a silent no-op return here looks exactly like "the scene is black".
        const u32 width = film.GetWidth();
        const u32 height = film.GetHeight();
        if (width == 0 || height == 0 || settings.SamplesPerPixel == 0)
        {
            OLO_CORE_ERROR("PathTracer::Render: nothing to render — film is {}x{} and SamplesPerPixel is {}. "
                           "The CALLER sizes the film (ReferenceFilm(w, h) or Resize).",
                           width, height, settings.SamplesPerPixel);
            return;
        }

        film.Clear();
        std::vector<glm::vec3>& pixels = film.GetPixels();

        const f32 invWidth = 1.0f / static_cast<f32>(width);
        const f32 invHeight = 1.0f / static_cast<f32>(height);
        const f32 invSamples = 1.0f / static_cast<f32>(settings.SamplesPerPixel);

        // Parallel over ROWS only. Every pixel's samples are summed by one
        // thread in ascending sample order — that fixed accumulation order is
        // half of the bit-identity guarantee (PathSampler is the other half).
        const EParallelForFlags flags = settings.ForceSingleThread ? EParallelForFlags::ForceSingleThread
                                                                   : EParallelForFlags::None;

        ParallelFor(
            "PathTracer::Render",
            static_cast<i32>(height),
            1, // MinBatchSize: rows are individually expensive
            [&](i32 rowIndex)
            {
                const auto y = static_cast<u32>(rowIndex);
                for (u32 x = 0; x < width; ++x)
                {
                    const u32 pixelSeed = MakePixelSeed(x, y, settings.Seed);
                    glm::vec3 accumulated(0.0f);

                    for (u32 sample = 0; sample < settings.SamplesPerPixel; ++sample)
                    {
                        PathSampler sampler(pixelSeed, sample);

                        // Jitter within the pixel footprint (box filter).
                        const glm::vec2 jitter = sampler.Get2D();
                        const glm::vec2 screenUV((static_cast<f32>(x) + jitter.x) * invWidth,
                                                 (static_cast<f32>(y) + jitter.y) * invHeight);

                        accumulated += TracePath(scene, camera.GenerateRay(screenUV), settings, sampler);
                    }

                    pixels[static_cast<sizet>(y) * width + x] = accumulated * invSamples;
                }
            },
            flags);
    }

    glm::vec3 PathTracer::EstimateIrradiance(const ReferenceScene& scene, const glm::vec3& position,
                                             const glm::vec3& normal, const PathTracerSettings& settings,
                                             u32 pixelSeed)
    {
        if (!scene.IsBuilt() || settings.SamplesPerPixel == 0)
            return glm::vec3(0.0f);

        const f32 normalLengthSq = glm::dot(normal, normal);
        if (!(normalLengthSq > 0.0f))
            return glm::vec3(0.0f);
        const glm::vec3 n = normal * glm::inversesqrt(normalLengthSq);

        glm::dvec3 sum(0.0);
        for (u32 sample = 0; sample < settings.SamplesPerPixel; ++sample)
        {
            PathSampler sampler(pixelSeed, sample);
            const glm::vec3 direction = CosineSampleHemisphere(sampler.Get2D(), n);
            if (glm::dot(direction, n) <= 0.0f)
                continue;

            const Ray ray(position + n * settings.RayEpsilon, direction, 0.0f, std::numeric_limits<f32>::max());
            // Cosine-weighted estimator: L * cos / pdf, and pdf == cos / pi, so
            // the cosine cancels and the estimator is simply L * pi.
            sum += glm::dvec3(TracePath(scene, ray, settings, sampler)) * static_cast<f64>(kPi);
        }

        return glm::vec3(sum / static_cast<f64>(settings.SamplesPerPixel));
    }
} // namespace OloEngine::PathTracing
