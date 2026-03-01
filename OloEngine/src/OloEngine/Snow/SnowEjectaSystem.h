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

    /**
     * @brief GPU-driven snow ejecta particle system.
     *
     * When SnowDeformerComponent entities move through accumulated snow,
     * this system emits short-lived snow puff particles that shoot outward
     * and drift down under low gravity. Uses the existing GPUParticleSystem
     * pipeline (emit → simulate → compact → build-indirect) and renders
     * via ParticleBatchRenderer::RenderGPUBillboards() during the
     * ParticleRenderPass.
     *
     * Follows the static-singleton pattern (WindSystem, SnowAccumulationSystem).
     */
    class SnowEjectaSystem
    {
      public:
        /// Initialize GPU particle system and load ejecta texture.
        static void Init(u32 maxParticles = 8192);

        /// Release all GPU resources.
        static void Shutdown();

        /// @return true after Init() succeeds.
        [[nodiscard]] static bool IsInitialized();

        /**
         * @brief Emit ejecta particles at a deformer location.
         *
         * Called once per SnowDeformerComponent per frame when the entity
         * is moving through snow. Particles are CPU-staged with randomized
         * directions, then uploaded to the GPU particle system.
         *
         * @param position       World position of the deformer.
         * @param deformerVelocity World-space velocity of the deformer entity.
         * @param deformRadius   Stamp radius (controls particle spread).
         * @param deformDepth    How deep the stamp goes (scales particle count).
         * @param settings       Scene-level ejecta configuration.
         */
        static void EmitAt(const glm::vec3& position,
                           const glm::vec3& deformerVelocity,
                           f32 deformRadius,
                           f32 deformDepth,
                           const SnowEjectaSettings& settings);

        /**
         * @brief Run GPU simulation pipeline for this frame.
         *
         * Dispatches simulate → compact → build-indirect compute shaders
         * for all live ejecta particles. Call once per frame after all
         * EmitAt() calls.
         *
         * @param settings   Scene-level ejecta configuration.
         * @param dt         Frame delta time.
         */
        static void Update(const SnowEjectaSettings& settings, Timestep dt);

        /**
         * @brief Render all live ejecta particles.
         *
         * Must be called inside the ParticleRenderPass callback, between
         * ParticleBatchRenderer::BeginBatch() and EndBatch().
         */
        static void Render();

        /// Reset particle pool (scene change / reload).
        static void Reset();

      private:
        struct SnowEjectaData
        {
            Scope<GPUParticleSystem> m_GPUSystem;
            Ref<Texture2D> m_EjectaTexture; // Soft white puff texture
            bool m_Initialized = false;
        };

        static SnowEjectaData s_Data;
    };
} // namespace OloEngine
