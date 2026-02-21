#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <span>

namespace OloEngine
{
    class GPUParticleSystem
    {
      public:
        explicit GPUParticleSystem(u32 maxParticles = 100000);
        ~GPUParticleSystem();

        GPUParticleSystem(const GPUParticleSystem&) = delete;
        GPUParticleSystem& operator=(const GPUParticleSystem&) = delete;
        GPUParticleSystem(GPUParticleSystem&& other) noexcept;
        GPUParticleSystem& operator=(GPUParticleSystem&& other) noexcept;

        void Init(u32 maxParticles);
        void Shutdown();

        // Upload newly emitted particles from CPU into free GPU slots
        void EmitParticles(std::span<const GPUParticle> newParticles);

        // Dispatch simulation compute shader
        void Simulate(f32 dt, const GPUSimParams& params);

        // Dispatch compaction — builds alive index list and free list
        void Compact();

        // Dispatch indirect draw buffer construction
        void PrepareIndirectDraw();

        // Accessors for rendering
        [[nodiscard]] const Ref<StorageBuffer>& GetParticleSSBO() const
        {
            return m_ParticleSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetAliveIndexSSBO() const
        {
            return m_AliveIndexSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetIndirectDrawSSBO() const
        {
            return m_IndirectDrawSSBO;
        }

        // CPU readback (debug/UI only — involves GPU sync)
        [[nodiscard]] u32 GetAliveCount() const;

        [[nodiscard]] u32 GetMaxParticles() const
        {
            return m_MaxParticles;
        }
        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

      private:
        u32 m_MaxParticles = 0;
        bool m_Initialized = false;

        // SSBOs
        Ref<StorageBuffer> m_ParticleSSBO;     // binding 0: GPUParticle[maxParticles]
        Ref<StorageBuffer> m_AliveIndexSSBO;   // binding 1: u32[maxParticles]
        Ref<StorageBuffer> m_CounterSSBO;      // binding 2: GPUParticleCounters
        Ref<StorageBuffer> m_FreeListSSBO;     // binding 3: u32[maxParticles]
        Ref<StorageBuffer> m_IndirectDrawSSBO; // binding 4: DrawElementsIndirectCommand
        Ref<StorageBuffer> m_EmitStagingSSBO;  // binding 5: GPUParticle[emitBatchSize]

        // Compute shaders
        Ref<ComputeShader> m_EmitShader;
        Ref<ComputeShader> m_SimulateShader;
        Ref<ComputeShader> m_CompactShader;
        Ref<ComputeShader> m_BuildIndirectShader;

        static constexpr u32 EMIT_WORKGROUP_SIZE = 64;
        static constexpr u32 SIM_WORKGROUP_SIZE = 256;
        static constexpr u32 COMPACT_WORKGROUP_SIZE = 256;
        static constexpr u32 MAX_EMIT_BATCH = 4096;
    };

} // namespace OloEngine
