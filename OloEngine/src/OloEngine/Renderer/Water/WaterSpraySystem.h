#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/Water/WaterSpray.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class GPUParticleSystem;
    class Texture2D;

    namespace Ocean
    {
        class OceanFFTField;
    }

    // =========================================================================
    // WaterSpraySystem — bubble / spray particles off folding crests
    // (issue #1034, §2.3).
    //
    // The engine already has everything this needs: GPUParticleSystem runs the
    // emit/simulate/compact/indirect chain, Particle_Simulate.comp already does
    // ballistic motion with gravity, drag and wind-field sampling, and
    // ParticleBatchRenderer already draws soft-depth-blended billboards. So
    // this class is an EMITTER and an emission criterion, which is exactly what
    // the issue says it should be — not a new particle subsystem.
    //
    // WHY THE EMISSION IS CPU-SIDE. The crest test needs the ocean's folding
    // Jacobian, and OceanFFTField already retains a CPU copy of every band so
    // buoyancy can float a boat without a GPU readback. Emitting from the
    // compute pass that writes the foam field instead would need an append
    // buffer from that pass into the particle pool, and would put the
    // "does not emit on a calm sea" acceptance criterion somewhere no CI test
    // can reach — the whole emission path here runs headlessly, which is why
    // WaterSprayTest can assert it against a real calm spectrum.
    //
    // Static singleton, dispatched from RenderPipeline, mirroring
    // PrecipitationSystem — which is the same shape (a camera-followed GPU
    // particle layer fed by a CPU emitter). It carries the same obligation to
    // Reset() on scene entry: the pool is world-positioned, so a runtime scene
    // switch would otherwise leave the previous sea's spray hanging in the air.
    // =========================================================================
    class WaterSpraySystem
    {
      public:
        /// Create the particle pool and load the droplet texture.
        /// @param maxParticles Pool capacity. Spray is a garnish — the default
        ///        is two orders of magnitude below the precipitation layers'.
        static void Init(u32 maxParticles = 8000u);

        /// Release GPU resources.
        static void Shutdown();

        [[nodiscard]] static bool IsInitialized();

        /// Publish this frame's spray tunables and the ocean field to sample.
        ///
        /// The FIELD, not a texture handle: the criterion is evaluated on the
        /// CPU proxy (see the class comment). A null field is the disabled
        /// state — a Gerstner sea has no folding Jacobian to test, exactly as
        /// it has none for the foam deposit to read.
        ///
        /// `foamThreshold` is the foam field's deposit threshold, forwarded so
        /// the spray criterion can never fire on gentler crests than the foam
        /// one does — see WaterSpray::EffectiveThreshold.
        static void SetSource(const WaterSpray::WaterSpraySettings& settings,
                              const Ref<Ocean::OceanFFTField>& field, f32 foamThreshold);

        /// Emit from this frame's folding crests and step the pool.
        ///
        /// Runs even when spray is disabled, so particles already in the air
        /// finish their arcs instead of vanishing the instant the sea calms —
        /// the same drain the precipitation system does, and for the same
        /// reason.
        static void Update(const glm::vec3& cameraPos, Timestep dt);

        /// Draw the live spray. Must be called inside the ParticleRenderPass
        /// callback, between ParticleBatchRenderer::BeginBatch() and EndBatch(),
        /// alongside PrecipitationSystem::Render.
        static void Render();

        /// Drop the pool and the published source. Called on scene load /
        /// runtime start alongside WaterDisturbanceSystem::Reset.
        static void Reset();

        /// Particles emitted by the last Update. Diagnostics and tests; 0 on a
        /// calm sea is the §2.3 acceptance criterion.
        [[nodiscard]] static u32 GetLastEmitCount();

        /// Particles currently ALIVE in the pool. Diagnostics and tests.
        ///
        /// A GPU->CPU readback, so it stalls — never call it per frame from
        /// render code. It exists because the emit count alone proves only that
        /// the CPU emitter ran: this is what shows the particles survived the
        /// upload, the simulate and the compaction and are there to be drawn.
        [[nodiscard]] static u32 GetAliveParticleCount();

        [[nodiscard]] static const WaterSpray::WaterSpraySettings& GetSettings();

      private:
        struct WaterSprayData
        {
            Scope<GPUParticleSystem> m_System;
            Ref<Texture2D> m_DropletTexture;
            WaterSpray::WaterSpraySettings m_Settings{};
            Ref<Ocean::OceanFFTField> m_Field;
            f32 m_FoamThreshold = 0.10f;
            f32 m_AccumulatedTime = 0.0f;
            /// Keeps simulating for a while after spray is switched off, so
            /// airborne droplets land rather than disappearing.
            f32 m_DrainTimeRemaining = 0.0f;
            u32 m_LastEmitCount = 0;
            u32 m_LogThrottle = 0;
            bool m_Initialized = false;
        };

        static WaterSprayData s_Data;
    };
} // namespace OloEngine
