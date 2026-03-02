#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    /// Identifies which precipitation layer particles are generated for.
    enum class PrecipitationLayer : u8
    {
        NearField = 0,
        FarField = 1
    };

    /**
     * @brief Camera-relative, frustum-aware precipitation particle emitter.
     *
     * Encapsulates the spawn logic for GPU precipitation particles, separated
     * from PrecipitationSystem for testability. Handles:
     *   - Camera-relative AABB spawning (particles always surround the camera)
     *   - Wind-biased volume offsets (shift upwind so particles drift into view)
     *   - Velocity-compensated spawning (no "wall of snow" when camera moves)
     *   - Height-stratified density (more particles at mid-height)
     *   - Per-layer configuration (near-field detail vs far-field atmosphere)
     */
    class PrecipitationEmitter
    {
      public:
        /**
         * @brief Generate precipitation particles for one layer.
         *
         * Particle velocity, jitter, and rotation are adjusted per
         * PrecipitationType (Snow, Rain, Hail, Sleet).
         *
         * @param cameraPos    Current camera world position.
         * @param lastCameraPos Previous frame camera position (for motion compensation).
         * @param settings     Precipitation configuration.
         * @param intensity    Current smoothed intensity (0–1).
         * @param layer        Which layer to generate for.
         * @param windDir      Current wind direction (normalized).
         * @param windSpeed    Current wind speed (m/s).
         * @param dt           Frame delta time.
         * @return             Vector of GPUParticle structs ready for upload.
         */
        static std::vector<GPUParticle> GenerateParticles(
            const glm::vec3& cameraPos,
            const glm::vec3& lastCameraPos,
            const PrecipitationSettings& settings,
            f32 intensity,
            PrecipitationLayer layer,
            const glm::vec3& windDir,
            f32 windSpeed,
            f32 dt);

        /**
         * @brief Calculate effective emission rate for a given intensity.
         *
         * Uses quadratic scaling for perceptual linearity:
         * effectiveRate = baseRate * intensity²
         *
         * @param baseRate   Base emission rate at intensity=1.
         * @param intensity  Current smoothed intensity (0–1).
         * @return           Effective particles/second.
         */
        [[nodiscard]] static f32 CalculateEmissionRate(u32 baseRate, f32 intensity);

        /**
         * @brief Seed the internal RNG for reproducible emission.
         *
         * Call before first emission if deterministic output is desired
         * (e.g. regression tests, replays).
         *
         * @param seed  32-bit seed value.
         */
        static void Seed(u32 seed);

        /**
         * @brief Compute the spawn volume AABB for a layer.
         *
         * @param cameraPos  Camera world position.
         * @param extent     Half-extents of the spawn volume.
         * @param windDir    Wind direction for upwind bias.
         * @param windSpeed  Wind speed for bias magnitude.
         * @return           {AABBMin, AABBMax} pair.
         */
        [[nodiscard]] static std::pair<glm::vec3, glm::vec3> ComputeSpawnVolume(
            const glm::vec3& cameraPos,
            const glm::vec3& extent,
            const glm::vec3& windDir,
            f32 windSpeed);

      private:
        static FastRandom<PCG32Algorithm> s_Rng;
    };
} // namespace OloEngine
