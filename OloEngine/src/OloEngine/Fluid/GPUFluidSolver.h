#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Fluid/CPUFluidSolver.h" // FluidKillBox
#include "OloEngine/Fluid/FluidSolverTypes.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    // =========================================================================
    // GPUFluidSolver — OpenGL compute backend of the Position-Based Fluids
    // step (issue #630). Mirrors CPUFluidSolver formula-for-formula; the
    // parity contract lives in
    // OloEngine/tests/Rendering/PropertyTests/GPUFluidSolverParityTest.cpp.
    //
    // Shape copied from GPUParticleSystem: move-only (NOT RefCounted), owns
    // its SSBOs at the ShaderBindingLayout::SSBO_FLUID_* bindings (21-32) plus
    // the FluidUBO at UBO_FLUID (47), re-Bind()s every buffer a pass touches
    // immediately before each dispatch (binding points are process-global GL
    // state), and issues a ShaderStorage barrier after every dispatch.
    //
    // Readback discipline (issue #551): Step() dispatches at a CPU-known
    // conservative upper bound and the shaders self-guard against the live
    // counter; the exact count is only read back every ~30 steps (or on
    // explicit RefreshExactCount()/ReadbackParticles() calls).
    //
    // Requires a live GL 4.6 context and cwd = OloEditor/ (shader paths are
    // cwd-relative). All methods must run on the thread owning the context.
    // =========================================================================
    class GPUFluidSolver
    {
      public:
        explicit GPUFluidSolver(u32 maxParticles);
        ~GPUFluidSolver();

        GPUFluidSolver(const GPUFluidSolver&) = delete;
        GPUFluidSolver& operator=(const GPUFluidSolver&) = delete;
        GPUFluidSolver(GPUFluidSolver&& other) noexcept;
        GPUFluidSolver& operator=(GPUFluidSolver&& other) noexcept;

        void Init(u32 maxParticles);
        void Shutdown();
        [[nodiscard]] bool IsValid() const
        {
            return m_Initialized;
        }

        /// Initial prefill: direct upload into the positions/velocities SSBOs
        /// and the counters (no shader dispatch). Replaces any existing
        /// particles and resets the CPU-known count. Entry Position/Velocity w
        /// components are ignored (w is the kill flag GPU-side, forced to 0).
        void SeedParticles(std::span<const GPUFluidEmitEntry> entries);

        /// Stage emissions for the NEXT Step() (uploaded to the staging SSBO
        /// immediately; clamped to the 4096-entry staging capacity per step).
        void Emit(std::span<const GPUFluidEmitEntry> entries);

        /// Advance one fixed step: emit -> (kill/compact) -> integrate -> grid
        /// -> Jacobi lambda/displace iterations -> velocity update -> vorticity
        /// -> XSPH apply -> finalize. Body proxies beyond kFluidMaxBodyProxies
        /// and kill boxes beyond FluidUBO::kMaxKillBoxes are ignored.
        void Step(const FluidSolverParams& params, f32 dt,
                  std::span<const FluidBodyProxy> bodyProxies,
                  std::span<const FluidKillBox> killBoxes);

        /// Read back the reaction impulses the LAST Step() accumulated for its
        /// body proxies (blocking GetData; the buffers are tiny). Fixed-point
        /// i32 values are converted via kFluidImpulseFixedScale. Entries
        /// beyond the last step's proxy count are zero-filled. Does NOT clear
        /// the accumulators — the next Step() with proxies does.
        void HarvestFeedback(std::span<FluidBodyFeedback> outFeedback);

        /// CPU-known conservative particle count (exact after SeedParticles /
        /// RefreshExactCount; grows optimistically on emit, never shrinks on
        /// kills until the next refresh).
        [[nodiscard]] u32 GetParticleUpperBound() const
        {
            return m_ParticleUpperBound;
        }

        /// Blocking readback of the live counter (GPU sync — issue #551).
        /// Callers use sparingly; Step() already refreshes every ~30 steps.
        u32 RefreshExactCount();

        [[nodiscard]] const Ref<StorageBuffer>& GetPositionsSSBO() const
        {
            return m_PositionsSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetVelocitiesSSBO() const
        {
            return m_VelocitiesSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetCountersSSBO() const
        {
            return m_CountersSSBO;
        }

        /// Test/debug: blocking readback of the live particles (positions and
        /// velocities as vec4, count from a counter refresh).
        void ReadbackParticles(std::vector<glm::vec4>& outPositions,
                               std::vector<glm::vec4>& outVelocities, u32& outCount);

        /// Re-read and recompile every Fluid_*.comp (HZBGenerator::Reload pattern).
        void Reload();

        [[nodiscard]] u32 GetMaxParticles() const
        {
            return m_MaxParticles;
        }

        /// Emit staging capacity per step (entries), mirrors
        /// GPUParticleSystem::MAX_EMIT_BATCH.
        static constexpr u32 kEmitStagingCapacity = 4096;

        /// Steps between automatic exact-count refreshes (issue #551).
        static constexpr u32 kCountRefreshInterval = 30;

      private:
        u32 m_MaxParticles = 0;
        bool m_Initialized = false;

        u32 m_ParticleUpperBound = 0;     // CPU-known conservative live count
        u32 m_PendingEmitCount = 0;       // staged entries awaiting the next Step
        u32 m_StepsSinceCountRefresh = 0; // cadence tracker for RefreshExactCount
        u32 m_LastProxyCount = 0;         // proxies uploaded by the last Step (for HarvestFeedback)
        u32 m_LastSolverIterations = 1;   // iteration count of the last Step (feedback averaging)
        u32 m_GridCellCount = 0;          // current grid-head buffer capacity, in cells

        // SSBOs (bindings = ShaderBindingLayout::SSBO_FLUID_*)
        Ref<StorageBuffer> m_PositionsSSBO;     // 21: vec4[max] — xyz pos, w kill flag
        Ref<StorageBuffer> m_VelocitiesSSBO;    // 22: vec4[max]
        Ref<StorageBuffer> m_PredictedASSBO;    // 23: vec4[max] — Jacobi ping / compact scratch
        Ref<StorageBuffer> m_PredictedBSSBO;    // 24: vec4[max] — Jacobi pong
        Ref<StorageBuffer> m_AuxSSBO;           // 25: vec4[max] — xyz omega, w lambda
        Ref<StorageBuffer> m_GridHeadSSBO;      // 26: u32[cells] — linked-list heads (index+1)
        Ref<StorageBuffer> m_GridNextSSBO;      // 27: u32[max] — linked-list next (index+1)
        Ref<StorageBuffer> m_CountersSSBO;      // 28: GPUFluidCounters
        Ref<StorageBuffer> m_EmitStagingSSBO;   // 29: GPUFluidEmitEntry[kEmitStagingCapacity]
        Ref<StorageBuffer> m_BodyProxiesSSBO;   // 30: FluidBodyProxy[kFluidMaxBodyProxies]
        Ref<StorageBuffer> m_BodyImpulsesSSBO;  // 31: GPUFluidBodyImpulse[kFluidMaxBodyProxies]
        Ref<StorageBuffer> m_VelocitiesAltSSBO; // 32: vec4[max] — XSPH pong / compact scratch

        // Solver parameters UBO (binding = ShaderBindingLayout::UBO_FLUID)
        Ref<UniformBuffer> m_FluidUBO;

        // Compute passes (assets/shaders/compute/Fluid_*.comp)
        Ref<ComputeShader> m_EmitShader;
        Ref<ComputeShader> m_KillMarkShader;
        Ref<ComputeShader> m_CompactShader;
        Ref<ComputeShader> m_CompactCommitShader;
        Ref<ComputeShader> m_IntegrateShader;
        Ref<ComputeShader> m_GridBuildShader;
        Ref<ComputeShader> m_LambdaShader;
        Ref<ComputeShader> m_DisplaceShader;
        Ref<ComputeShader> m_VelocityUpdateShader;
        Ref<ComputeShader> m_VorticityShader;
        Ref<ComputeShader> m_VelocityApplyShader;
        Ref<ComputeShader> m_FinalizeShader;
    };
} // namespace OloEngine
