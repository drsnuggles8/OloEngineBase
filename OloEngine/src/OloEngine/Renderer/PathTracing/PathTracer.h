#pragma once

// =============================================================================
// PathTracer.h — the offline reference path tracer (issue #709)
//
// WHAT THIS IS FOR
// ----------------
// The repo's rendering rule says contract tests prove the formula and
// screenshots prove "did it change?" — nothing answered "is it CORRECT?". This
// is that missing oracle: an unbiased Monte Carlo integrator that shades with
// the engine's OWN BRDF (ReferenceBRDF.h, pinned against PBRCommon.glsl by a
// GPU parity test) and computes transport by brute force. A golden image locks
// in whatever shipped, bugs included; a converged reference does not.
//
// It is NOT a shipping feature. It is CPU-side on purpose (see
// docs/agent-rules/reference-path-tracer.md §"Why CPU"): offline by definition,
// no GL context, so it runs headless in CI, and it stays entirely clear of the
// RHI/bindless work.
//
// THE INTEGRATOR
// --------------
// Unidirectional path tracing with:
//   * next-event estimation against punctual lights (delta -> no MIS needed)
//     and against emissive geometry (area sampled -> MIS against BSDF sampling
//     with the power heuristic),
//   * one-sample lobe MIS for the BSDF (pick diffuse or specular, evaluate the
//     FULL BRDF, divide by the mixture density),
//   * Russian roulette on throughput luminance after a configurable depth,
//   * a uniform environment collected on ray escape.
//
// DETERMINISM
// -----------
// Two renders with the same scene + camera + settings are BIT-IDENTICAL,
// whatever the thread count. That is a hard requirement, not a nicety: it is
// what lets a CI gate assert an exact hash instead of a fuzzy tolerance. It
// rests on two things — PathSampler is stateless (see its header), and a single
// pixel's samples are always summed by ONE thread in ascending sample order, so
// the floating-point accumulation order is fixed. Parallelism is over pixels
// only. Do not "optimise" this by splitting a pixel's samples across threads.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine::PathTracing
{
    // -------------------------------------------------------------------------
    // Camera — defined by the inverse view-projection matrix.
    //
    // Deliberately NOT a position + fov + up triple. A raster-vs-reference
    // comparison is only meaningful if both paths look through exactly the same
    // camera, and the engine's cameras (EditorCamera, CameraComponent) differ in
    // handedness, clip convention and aspect handling. Taking the matrix the
    // renderer itself used removes every chance of the two drifting: build one
    // with `FromViewProjection(camera.GetViewProjection(), camera.GetPosition())`
    // and the primary rays are the raster path's pixel centres by construction.
    // -------------------------------------------------------------------------
    struct ReferenceCamera
    {
        glm::mat4 InverseViewProjection{ 1.0f };
        glm::vec3 Position{ 0.0f };

        [[nodiscard]] static ReferenceCamera FromViewProjection(const glm::mat4& viewProjection, const glm::vec3& position);

        // Ray through a point in [0,1]^2 screen space, y DOWN (row 0 is the top
        // row of the image, matching the PNG the film writes).
        [[nodiscard]] Ray GenerateRay(const glm::vec2& screenUV) const;
    };

    // -------------------------------------------------------------------------
    // Settings
    // -------------------------------------------------------------------------
    struct PathTracerSettings
    {
        u32 SamplesPerPixel = 64;
        // Number of SURFACE interactions along a path. 1 == direct lighting
        // only (no GI); 2 == one bounce of indirect, and so on.
        u32 MaxBounces = 8;
        // Start Russian roulette after this many bounces. 0 disables RR (used
        // by the furnace test, where the whole point is to not throw away
        // energy stochastically at low depth).
        u32 RussianRouletteStartBounce = 4;
        // Global sampler seed. Changing it changes the image's noise but not
        // its converged value.
        u32 Seed = 0x9e3779b9u;
        // Next-event estimation. Off makes the integrator pure BSDF sampling —
        // still unbiased, far noisier, and the cross-check that proves the NEE
        // + MIS machinery did not introduce a bias (they must converge to the
        // same image).
        bool EnableNextEventEstimation = true;
        // Firefly clamp on a single path's contribution, in radiance units.
        // <= 0 disables it. NOTE: a clamp is a BIAS — it is off by default and
        // must stay off for anything that claims to be ground truth. It exists
        // for eyeballing a noisy preview, not for the oracle.
        f32 MaxRadianceClamp = 0.0f;
        // Ray offset along the shading normal to avoid self-intersection.
        f32 RayEpsilon = 1e-3f;
        // Force single-threaded evaluation (diagnostics: proving the parallel
        // and sequential renders agree bit-for-bit).
        bool ForceSingleThread = false;
    };

    // -------------------------------------------------------------------------
    // Film — a linear-radiance accumulation buffer plus encoders.
    // -------------------------------------------------------------------------
    class ReferenceFilm
    {
      public:
        ReferenceFilm() = default;
        ReferenceFilm(u32 width, u32 height);

        void Resize(u32 width, u32 height);
        void Clear();

        [[nodiscard]] u32 GetWidth() const
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const
        {
            return m_Height;
        }

        // Linear radiance, row 0 == top.
        [[nodiscard]] const std::vector<glm::vec3>& GetPixels() const
        {
            return m_Pixels;
        }
        [[nodiscard]] std::vector<glm::vec3>& GetPixels()
        {
            return m_Pixels;
        }
        [[nodiscard]] glm::vec3 GetPixel(u32 x, u32 y) const;

        // Mean linear radiance over an inclusive pixel rectangle — the
        // workhorse for region assertions (a per-pixel comparison against a
        // Monte Carlo image is dominated by noise; a region mean is not).
        [[nodiscard]] glm::vec3 MeanRadiance(u32 x0, u32 y0, u32 x1, u32 y1) const;

        // Encode to 8-bit RGBA using the engine's display transform, so the
        // result is directly comparable with a raster composite readback.
        // `tonemap` mirrors PBRCommon's postProcessColor operator selector
        // (0 none / 1 Reinhard / 2 ACES).
        void EncodeRgba8(std::vector<u8>& outRgba, i32 tonemap = 1, f32 exposure = 1.0f, bool applyGamma = true) const;

        // A stable content hash of the linear radiance buffer. This is the
        // determinism gate's assertion: same scene + settings => same hash,
        // any thread count. FNV-1a over the raw float bits — bitwise, not
        // approximate, because "approximately deterministic" is not a property
        // an oracle can have.
        [[nodiscard]] u64 ComputeHash() const;

      private:
        u32 m_Width = 0;
        u32 m_Height = 0;
        std::vector<glm::vec3> m_Pixels;
    };

    // -------------------------------------------------------------------------
    // PathTracer
    // -------------------------------------------------------------------------
    class PathTracer
    {
      public:
        // Render `scene` through `camera` into `film` (which is resized and
        // cleared). The scene must have been Build()t.
        static void Render(const ReferenceScene& scene, const ReferenceCamera& camera,
                           const PathTracerSettings& settings, ReferenceFilm& film);

        // Trace a single path and return its radiance estimate. Exposed so
        // tests can integrate a quantity that is not an image — e.g. the
        // ground-truth irradiance arriving at a DDGI probe.
        [[nodiscard]] static glm::vec3 TracePath(const ReferenceScene& scene, const Ray& primaryRay,
                                                 const PathTracerSettings& settings, PathSampler& sampler);

        // Ground-truth diffuse irradiance at a point/normal:
        //     E(p, n) = integral over the hemisphere of L_i(p, w) * max(0, n.w) dw
        // estimated with cosine-weighted sampling. This is the quantity a DDGI
        // probe stores, so it is the quantity a DDGI validation must compare —
        // comparing final PIXELS instead would fold in the raster path's
        // exposure, tonemap and ambient ladder and answer a different question.
        [[nodiscard]] static glm::vec3 EstimateIrradiance(const ReferenceScene& scene, const glm::vec3& position,
                                                          const glm::vec3& normal, const PathTracerSettings& settings,
                                                          u32 pixelSeed);
    };
} // namespace OloEngine::PathTracing
