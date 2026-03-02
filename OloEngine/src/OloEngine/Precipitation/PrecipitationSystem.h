#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    class GPUParticleSystem;
    class Texture2D;
    class UniformBuffer;
    class ComputeShader;
    class EditorCamera;
    class Camera;

    /**
     * @brief Statistics snapshot for precipitation system performance monitoring.
     */
    struct PrecipitationStats
    {
        u32 NearFieldAlive = 0;          ///< Number of live near-field particles
        u32 FarFieldAlive = 0;           ///< Number of live far-field particles
        f32 EffectiveEmissionRate = 0.0f; ///< Current effective particles/sec
        f32 GPUTimeMs = 0.0f;            ///< GPU time for sim+render (ms)
        u32 AccumulationFeedCount = 0;   ///< Particles that fed accumulation this frame
    };

    /**
     * @brief Scene-global, camera-relative, multi-layer precipitation system.
     *
     * Manages two GPU particle layers:
     *   - Near-field: Detailed snowflakes close to camera (30m radius)
     *   - Far-field: Atmospheric fill in the distance (30–120m)
     *
     * Features:
     *   - Camera-relative emission (particles always surround the viewer)
     *   - Wind field integration via existing WindSampling.glsl
     *   - Intensity-driven LOD with frame budget control
     *   - Precipitation-to-accumulation bridge (feeding snow depth clipmap)
     *   - Screen-space effects (directional streaks + lens impacts)
     *
     * Uses the existing GPUParticleSystem compute pipeline (emit → simulate →
     * compact → build-indirect) and renders via ParticleBatchRenderer.
     *
     * Follows the static-singleton pattern (WindSystem, SnowEjectaSystem).
     */
    class PrecipitationSystem
    {
      public:
        /// Initialize GPU particle systems, textures, UBO, and compute shaders.
        static void Init();

        /// Release all GPU resources.
        static void Shutdown();

        /// @return true after Init() succeeds.
        [[nodiscard]] static bool IsInitialized();

        /**
         * @brief Main per-frame update.
         *
         * Interpolates intensity, emits particles into both layers,
         * runs GPU simulation pipeline, dispatches accumulation feed,
         * and updates the precipitation UBO for screen-space effects.
         *
         * @param settings   Scene-level precipitation configuration.
         * @param cameraPos  Camera world position.
         * @param windDir    Current wind direction (normalized).
         * @param windSpeed  Current wind speed (m/s).
         * @param dt         Frame delta time.
         */
        static void Update(const PrecipitationSettings& settings,
                           const glm::vec3& cameraPos,
                           const glm::vec3& windDir,
                           f32 windSpeed,
                           Timestep dt);

        /**
         * @brief Render all live precipitation particles.
         *
         * Must be called inside the ParticleRenderPass callback, between
         * ParticleBatchRenderer::BeginBatch() and EndBatch().
         */
        static void Render();

        /**
         * @brief Update screen-space precipitation UBO for post-process effects.
         *
         * Call before the post-process pass. Uploads streak/lens impact
         * parameters to UBO_PRECIPITATION for the PostProcess_Precipitation shader.
         *
         * @param settings       Scene-level precipitation configuration.
         * @param windDirScreen  Wind direction projected onto screen XY.
         * @param time           Accumulated time for animation.
         */
        static void UpdateScreenEffectsUBO(const PrecipitationSettings& settings,
                                           const glm::vec2& windDirScreen,
                                           f32 time);

        /// Reset particle pools (scene change / reload).
        static void Reset();

        /// Set target intensity with smooth interpolation.
        static void SetIntensity(f32 intensity);

        /// Set intensity immediately (no interpolation — for editor scrubbing).
        static void SetIntensityImmediate(f32 intensity);

        /// @return Current smoothed intensity value (0–1).
        [[nodiscard]] static f32 GetCurrentIntensity();

        /// @return Performance and state statistics.
        [[nodiscard]] static PrecipitationStats GetStatistics();

      private:
        struct PrecipitationData
        {
            Scope<GPUParticleSystem> m_NearFieldSystem;
            Scope<GPUParticleSystem> m_FarFieldSystem;
            Ref<Texture2D> m_SnowflakeTexture;
            Ref<UniformBuffer> m_PrecipitationUBO;
            Ref<ComputeShader> m_FeedShader;         // Precipitation_Feed.comp

            f32 m_CurrentIntensity = 0.0f;
            f32 m_TargetIntensity = 0.0f;
            glm::vec3 m_LastCameraPos = glm::vec3(0.0f);
            f32 m_AccumulatedTime = 0.0f;

            // Frame budget tracking
            f32 m_EmissionReductionFactor = 1.0f;
            f32 m_LastFrameTimeMs = 0.0f;

            // GPU timer query objects
            u32 m_TimerQueries[2] = { 0, 0 };
            u32 m_CurrentTimerQuery = 0;
            bool m_TimerQueryActive = false;

            PrecipitationUBOData m_GPUData;

            bool m_Initialized = false;
        };

        static PrecipitationData s_Data;
    };
} // namespace OloEngine
