#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"

#include <functional>
#include <vector>
#include <glm/glm.hpp>

namespace OloEngine
{
    namespace PathTracing
    {
        class ReferenceScene;
    }

    class Scene;
    struct LightProbeVolumeComponent;

    // Progress callback: (currentProbe, totalProbes)
    using ProbeBakeProgressCallback = std::function<void(i32, i32)>;

    // Settings for the CPU path-traced probe bake (issue #439).
    struct LightProbePathTracedBakeSettings
    {
        // Radiance paths per probe, distributed over the full sphere.
        u32 SamplesPerProbe = 512;
        // Surface interactions per path (PathTracerSettings::MaxBounces
        // semantics: 1 == direct-on-surfaces only, no GI).
        u32 MaxBounces = 4;
        // Global bake seed; per-probe sampler seeds derive from this and the
        // probe's linear grid index, so the bake is deterministic.
        u32 Seed = 0x439u;
    };

    class LightProbeBaker
    {
      public:
        // Capture cubemap at a world position and project to SH coefficients.
        // Resolution is the per-face cubemap resolution (e.g. 64).
        // Returns the baked SH and a validity flag (false if probe is inside geometry).
        static SHCoefficients BakeProbeAtPosition(
            Ref<Scene>& scene,
            const glm::vec3& position,
            u32 cubemapResolution = 64,
            bool* outValid = nullptr);

        // Bake all probes in a volume. Populates the asset's coefficient data.
        // The progress callback is invoked after each probe is baked.
        static void BakeVolume(
            Ref<Scene>& scene,
            LightProbeVolumeComponent& volume,
            Ref<LightProbeVolumeAsset>& asset,
            u32 cubemapResolution = 64,
            const ProbeBakeProgressCallback& progress = nullptr);

        // Project cubemap pixel data (faces laid out +X, -X, +Y, -Y, +Z, -Z;
        // each face stored row-major at the given per-face resolution) onto
        // the L2 SH basis. Reusable by IBL irradiance generation, scene-baked
        // light probes, and any future SH-based ambient path.
        static SHCoefficients ProjectToSH(
            const std::vector<glm::vec3>& cubemapPixels,
            u32 resolution);

        // ---- path-traced bake (issue #439) ----------------------------------
        // CPU-only alternative to the cubemap route above: the incident
        // radiance at each probe comes from PathTracer::TracePath against a
        // built ReferenceScene instead of a rasterized capture — no GL
        // context, full multi-bounce GI. The SH storage convention matches
        // ProjectToSH EXACTLY (raw radiance projection, no cosine
        // convolution — see the convention note in LightProbeBaker.cpp), so
        // the two bake modes are interchangeable for the same incident field.

        // Estimate one probe's SH from `world` at a world position.
        // `probeSeed` keys the deterministic sampler (derive it from the
        // probe's grid index via PathTracing::MakePixelSeed). `outValid`
        // mirrors BakeProbeAtPosition's mostly-black heuristic (a probe
        // buried inside geometry captures no energy).
        [[nodiscard]] static SHCoefficients BakeProbeAtPositionPathTraced(
            const PathTracing::ReferenceScene& world,
            const glm::vec3& position,
            const LightProbePathTracedBakeSettings& settings,
            u32 probeSeed,
            bool* outValid = nullptr);

        // Path-traced twin of BakeVolume: same probe-grid derivation from the
        // component's bounds/resolution (the grid is authored in WORLD space,
        // exactly as BakeVolume reads it — no volume transform participates),
        // same asset population and validity-flag convention. Probes bake in
        // parallel over engine tasks (each probe writes only its own asset
        // slot); still deterministic, because per-probe seeds derive from the
        // probe's linear grid index and settings.Seed, and samples are summed
        // in ascending order WITHIN each probe. `progress` may be invoked from
        // worker threads, but calls are serialized under an internal lock and
        // report a strictly increasing completed-probe count ending at
        // (total, total). Returns false when `asset` is null, `world` was not
        // Build()t, or the grid is empty.
        [[nodiscard]] static bool BakeVolumePathTraced(
            const PathTracing::ReferenceScene& world,
            LightProbeVolumeComponent& volume,
            Ref<LightProbeVolumeAsset>& asset,
            const LightProbePathTracedBakeSettings& settings = {},
            const ProbeBakeProgressCallback& progress = nullptr);

      private:
        // Render the scene into a cubemap FBO at the given position
        // Returns false when the GPU readback of any face fails; `outPixels` is
        // then not safe to project. Callers must not persist SH built from it.
        [[nodiscard]] static bool RenderCubemapAtPosition(
            Ref<Scene>& scene,
            const glm::vec3& position,
            u32 resolution,
            std::vector<glm::vec3>& outPixels);
    };
} // namespace OloEngine
