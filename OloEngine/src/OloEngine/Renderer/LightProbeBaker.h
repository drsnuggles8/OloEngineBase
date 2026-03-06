#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"

#include <functional>
#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;
    struct LightProbeVolumeComponent;

    // Progress callback: (currentProbe, totalProbes)
    using ProbeBakeProgressCallback = std::function<void(i32, i32)>;

    class LightProbeBaker
    {
      public:
        // Capture cubemap at a world position and project to SH coefficients.
        // Resolution is the per-face cubemap resolution (e.g. 64).
        // Returns the baked SH and a validity flag (false if probe is inside geometry).
        static SHCoefficients BakeProbeAtPosition(
            const Ref<Scene>& scene,
            const glm::vec3& position,
            u32 cubemapResolution = 64,
            bool* outValid = nullptr);

        // Bake all probes in a volume. Populates the asset's coefficient data.
        // The progress callback is invoked after each probe is baked.
        static void BakeVolume(
            const Ref<Scene>& scene,
            LightProbeVolumeComponent& volume,
            Ref<LightProbeVolumeAsset>& asset,
            u32 cubemapResolution = 64,
            const ProbeBakeProgressCallback& progress = nullptr);

      private:
        // Render the scene into a cubemap FBO at the given position
        static void RenderCubemapAtPosition(
            const Ref<Scene>& scene,
            const glm::vec3& position,
            u32 resolution,
            std::vector<glm::vec3>& outPixels);

        // Project cubemap pixel data onto L2 SH basis
        static SHCoefficients ProjectToSH(
            const std::vector<glm::vec3>& cubemapPixels,
            u32 resolution);
    };
} // namespace OloEngine
