#include "OloEnginePCH.h"
#include "GPUParticleSystem.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"

#include <algorithm>
#include <cstddef>
#include <numeric>

namespace OloEngine
{
    // Compact() hands m_CounterSSBO to GPUPrefixSum as the scan's `totalOut`,
    // and the scan writes the grand total to that buffer's first u32. That is
    // only the alive count if AliveCount is the first member — so reordering
    // GPUParticleCounters must be a compile error here rather than a particle
    // system that silently draws DeadCount instances (issue #713).
    static_assert(offsetof(GPUParticleCounters, AliveCount) == 0,
                  "GPUParticleCounters::AliveCount must be first — the prefix-sum total is written to offset 0");

    GPUParticleSystem::GPUParticleSystem(u32 maxParticles)
    {
        Init(maxParticles);
    }

    GPUParticleSystem::~GPUParticleSystem()
    {
        Shutdown();
    }

    GPUParticleSystem::GPUParticleSystem(GPUParticleSystem&& other) noexcept
        : m_MaxParticles(other.m_MaxParticles),
          m_Initialized(other.m_Initialized),
          m_ParticleSSBO(std::move(other.m_ParticleSSBO)),
          m_AliveIndexSSBO(std::move(other.m_AliveIndexSSBO)),
          m_CounterSSBO(std::move(other.m_CounterSSBO)),
          m_FreeListSSBO(std::move(other.m_FreeListSSBO)),
          m_IndirectDrawSSBO(std::move(other.m_IndirectDrawSSBO)),
          m_EmitStagingSSBO(std::move(other.m_EmitStagingSSBO)),
          m_PrevPositionSSBO(std::move(other.m_PrevPositionSSBO)),
          m_AliveScanSSBO(std::move(other.m_AliveScanSSBO)),
          m_EmitShader(std::move(other.m_EmitShader)),
          m_SimulateShader(std::move(other.m_SimulateShader)),
          m_CompactShader(std::move(other.m_CompactShader)),
          m_CompactScatterShader(std::move(other.m_CompactScatterShader)),
          m_BuildIndirectShader(std::move(other.m_BuildIndirectShader)),
          m_PrefixSum(std::move(other.m_PrefixSum)),
          m_ParamsUBO(std::move(other.m_ParamsUBO)),
          m_Params(other.m_Params)
    {
        other.m_Initialized = false;
        other.m_MaxParticles = 0;
    }

    GPUParticleSystem& GPUParticleSystem::operator=(GPUParticleSystem&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_MaxParticles = other.m_MaxParticles;
            m_Initialized = other.m_Initialized;
            m_ParticleSSBO = std::move(other.m_ParticleSSBO);
            m_AliveIndexSSBO = std::move(other.m_AliveIndexSSBO);
            m_CounterSSBO = std::move(other.m_CounterSSBO);
            m_FreeListSSBO = std::move(other.m_FreeListSSBO);
            m_IndirectDrawSSBO = std::move(other.m_IndirectDrawSSBO);
            m_EmitStagingSSBO = std::move(other.m_EmitStagingSSBO);
            m_PrevPositionSSBO = std::move(other.m_PrevPositionSSBO);
            m_AliveScanSSBO = std::move(other.m_AliveScanSSBO);
            m_EmitShader = std::move(other.m_EmitShader);
            m_SimulateShader = std::move(other.m_SimulateShader);
            m_CompactShader = std::move(other.m_CompactShader);
            m_CompactScatterShader = std::move(other.m_CompactScatterShader);
            m_BuildIndirectShader = std::move(other.m_BuildIndirectShader);
            m_PrefixSum = std::move(other.m_PrefixSum);
            m_ParamsUBO = std::move(other.m_ParamsUBO);
            m_Params = other.m_Params;
            other.m_Initialized = false;
            other.m_MaxParticles = 0;
        }
        return *this;
    }

    void GPUParticleSystem::Init(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Initialized)
        {
            Shutdown();
        }

        // Refuse a count the compaction cannot scan (issue #713). Past this
        // bound `ExclusiveScanInPlace` returns false every frame while the
        // system stays "initialized", so the alive list silently freezes at
        // whatever the last successful compaction left — a stale draw list is
        // far worse than a system that says up front it cannot run.
        if (maxParticles > GPUPrefixSum::kMaxElements)
        {
            OLO_CORE_ERROR("GPUParticleSystem: maxParticles {0} exceeds the GPU prefix-sum limit {1}",
                           maxParticles, GPUPrefixSum::kMaxElements);
            return;
        }

        m_MaxParticles = maxParticles;

        // Allocate SSBOs
        m_ParticleSSBO = StorageBuffer::Create(
            maxParticles * GPUParticle::GetSize(),
            ShaderBindingLayout::SSBO_GPU_PARTICLES,
            StorageBufferUsage::DynamicCopy);

        // Zero-initialize particle buffer so all particles start as dead (Misc.z == 0.0)
        m_ParticleSSBO->ClearData();

        m_AliveIndexSSBO = StorageBuffer::Create(
            maxParticles * sizeof(u32),
            ShaderBindingLayout::SSBO_ALIVE_INDICES,
            StorageBufferUsage::DynamicCopy);

        m_CounterSSBO = StorageBuffer::Create(
            sizeof(GPUParticleCounters),
            ShaderBindingLayout::SSBO_COUNTERS,
            StorageBufferUsage::DynamicCopy);

        m_FreeListSSBO = StorageBuffer::Create(
            maxParticles * sizeof(u32),
            ShaderBindingLayout::SSBO_FREE_LIST,
            StorageBufferUsage::DynamicCopy);

        m_IndirectDrawSSBO = StorageBuffer::Create(
            sizeof(DrawElementsIndirectCommand),
            ShaderBindingLayout::SSBO_INDIRECT_DRAW,
            StorageBufferUsage::DynamicCopy);

        m_EmitStagingSSBO = StorageBuffer::Create(
            MAX_EMIT_BATCH * GPUParticle::GetSize(),
            ShaderBindingLayout::SSBO_EMIT_STAGING,
            StorageBufferUsage::DynamicDraw);

        // Previous-frame particle snapshot. Each slot carries:
        //   - prev position (vec4 xyz; w unused)
        //   - prev rotation + size (vec4 x = rot, y = size; zw unused)
        // Written by Particle_Simulate.comp at the start of each frame before
        // integration, and by Particle_Emit.comp on spawn so newly-emitted
        // particles emit zero per-particle motion on their first render.
        // Matches PrevParticleData struct layout in Particle_{Emit,Simulate,Billboard_GPU}.glsl.
        m_PrevPositionSSBO = StorageBuffer::Create(
            maxParticles * 2 * sizeof(glm::vec4),
            ShaderBindingLayout::SSBO_GPU_PARTICLES_PREV,
            StorageBufferUsage::DynamicCopy);
        m_PrevPositionSSBO->ClearData();

        // Compaction scratch (issue #713): alive flags in, exclusive prefix sum
        // of those flags out, scanned in place by GPUPrefixSum between the two
        // compaction dispatches.
        m_AliveScanSSBO = StorageBuffer::Create(
            maxParticles * sizeof(u32),
            ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES,
            StorageBufferUsage::DynamicCopy);
        m_AliveScanSSBO->ClearData();

        // Initialize free list: all slots are free [0, 1, 2, ..., maxParticles-1]
        std::vector<u32> freeListInit(maxParticles);
        std::iota(freeListInit.begin(), freeListInit.end(), 0u);
        m_FreeListSSBO->SetData(freeListInit.data(), maxParticles * sizeof(u32));

        // Initialize counters: 0 alive, all dead
        GPUParticleCounters counters{};
        counters.AliveCount = 0;
        counters.DeadCount = maxParticles;
        counters.EmitCount = 0;
        counters.Pad = 0;
        m_CounterSSBO->SetData(&counters, sizeof(GPUParticleCounters));

        // Initialize indirect draw command (0 instances)
        DrawElementsIndirectCommand cmd{};
        cmd.Count = 6;         // Quad = 6 indices
        cmd.InstanceCount = 0; // No alive particles yet
        cmd.FirstIndex = 0;
        cmd.BaseVertex = 0;
        cmd.BaseInstance = 0;
        m_IndirectDrawSSBO->SetData(&cmd, sizeof(DrawElementsIndirectCommand));

        // Load compute shaders
        m_EmitShader = ComputeShader::Create("assets/shaders/compute/Particle_Emit.comp");
        m_SimulateShader = ComputeShader::Create("assets/shaders/compute/Particle_Simulate.comp");
        m_CompactShader = ComputeShader::Create("assets/shaders/compute/Particle_Compact.comp");
        m_CompactScatterShader = ComputeShader::Create("assets/shaders/compute/Particle_CompactScatter.comp");
        m_BuildIndirectShader = ComputeShader::Create("assets/shaders/compute/Particle_BuildIndirect.comp");
        m_PrefixSum = Ref<GPUPrefixSum>::Create();
        m_PrefixSum->EnsureInitialised();

        // Validate all shaders loaded successfully
        if (!m_EmitShader || !m_EmitShader->IsValid())
        {
            OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_EmitShader");
            Shutdown();
            return;
        }
        if (!m_SimulateShader || !m_SimulateShader->IsValid())
        {
            OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_SimulateShader");
            Shutdown();
            return;
        }
        if (!m_CompactShader || !m_CompactShader->IsValid())
        {
            OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_CompactShader");
            Shutdown();
            return;
        }
        if (!m_CompactScatterShader || !m_CompactScatterShader->IsValid())
        {
            OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_CompactScatterShader");
            Shutdown();
            return;
        }
        if (!m_PrefixSum || !m_PrefixSum->IsAvailable())
        {
            // Same shape as the shader checks above, and for the same reason:
            // without the scan there is no compaction at all (issue #713
            // replaced the atomicAdd path outright rather than keeping two).
            OLO_CORE_ERROR("GPUParticleSystem: GPU prefix-sum unavailable — compaction cannot run");
            Shutdown();
            return;
        }
        if (!m_BuildIndirectShader || !m_BuildIndirectShader->IsValid())
        {
            OLO_CORE_ERROR("GPUParticleSystem: Failed to load m_BuildIndirectShader");
            Shutdown();
            return;
        }

        m_Initialized = true;
    }

    void GPUParticleSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        m_ParticleSSBO = nullptr;
        m_AliveIndexSSBO = nullptr;
        m_CounterSSBO = nullptr;
        m_FreeListSSBO = nullptr;
        m_IndirectDrawSSBO = nullptr;
        m_EmitStagingSSBO = nullptr;
        m_PrevPositionSSBO = nullptr;
        m_AliveScanSSBO = nullptr;
        m_EmitShader = nullptr;
        m_SimulateShader = nullptr;
        m_CompactShader = nullptr;
        m_CompactScatterShader = nullptr;
        m_BuildIndirectShader = nullptr;
        m_PrefixSum = nullptr;
        m_ParamsUBO = nullptr;
        m_Initialized = false;
    }

    void GPUParticleSystem::UploadParams()
    {
        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::GPUParticleParamsUBO::GetSize(),
                                                ShaderBindingLayout::UBO_PARTICLE_SIM);
        }
        m_ParamsUBO->SetData(&m_Params, sizeof(m_Params));
        m_ParamsUBO->Bind();
    }

    void GPUParticleSystem::EmitParticles(std::span<const GPUParticle> newParticles)
    {
        OLO_PROFILE_FUNCTION();

        if (newParticles.empty() || !m_Initialized || !m_EmitShader || !m_EmitShader->IsValid())
        {
            return;
        }

        u32 emitCount = static_cast<u32>(std::min(newParticles.size(), static_cast<sizet>(MAX_EMIT_BATCH)));

        // Upload new particles to staging SSBO
        m_EmitStagingSSBO->Bind();
        m_EmitStagingSSBO->SetData(newParticles.data(), emitCount * GPUParticle::GetSize());

        // Bind all SSBOs
        m_ParticleSSBO->Bind();
        m_CounterSSBO->Bind();
        m_FreeListSSBO->Bind();
        m_PrevPositionSSBO->Bind();

        // Dispatch emission compute. One std140 refill per dispatch — the
        // former bare uniforms (issue #691 Phase 7). Legal on both backends:
        // GL re-uploads the bound buffer, the Vulkan arena mints a fresh
        // per-dispatch address on every SetData.
        m_EmitShader->Bind();
        m_Params = UBOStructures::GPUParticleParamsUBO{};
        m_Params.MaxParticles = m_MaxParticles;
        m_Params.EmitCount = static_cast<i32>(emitCount);
        UploadParams();

        u32 groups = (emitCount + EMIT_WORKGROUP_SIZE - 1) / EMIT_WORKGROUP_SIZE;
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void GPUParticleSystem::Simulate(const GPUSimParams& params)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || !m_SimulateShader || !m_SimulateShader->IsValid())
        {
            return;
        }

        // Bind SSBOs
        m_ParticleSSBO->Bind();
        m_PrevPositionSSBO->Bind();

        // Simulation parameters — one std140 refill per dispatch (the former
        // bare uniforms, issue #691 Phase 7).
        m_SimulateShader->Bind();
        m_Params = UBOStructures::GPUParticleParamsUBO{};
        m_Params.DeltaTime = params.DeltaTime;
        m_Params.Gravity = params.Gravity;
        m_Params.DragCoefficient = params.DragCoefficient;
        m_Params.MaxParticles = m_MaxParticles;
        m_Params.EnableGravity = params.EnableGravity;
        m_Params.EnableDrag = params.EnableDrag;
        m_Params.EnableWind = params.EnableWind;
        m_Params.WindInfluence = params.WindInfluence;
        m_Params.EnableNoise = params.EnableNoise;
        m_Params.NoiseStrength = params.NoiseStrength;
        m_Params.NoiseFrequency = params.NoiseFrequency;
        m_Params.EnableGroundCollision = params.EnableGroundCollision;
        m_Params.GroundY = params.GroundY;
        m_Params.CollisionBounce = params.CollisionBounce;
        m_Params.CollisionFriction = params.CollisionFriction;
        UploadParams();

        u32 groups = (m_MaxParticles + SIM_WORKGROUP_SIZE - 1) / SIM_WORKGROUP_SIZE;
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void GPUParticleSystem::Compact()
    {
        OLO_PROFILE_FUNCTION();

        const auto shaderReady = [](const Ref<ComputeShader>& shader)
        { return shader && shader->IsValid(); };

        if (!m_Initialized || !shaderReady(m_CompactShader) || !shaderReady(m_CompactScatterShader) || !m_PrefixSum)
        {
            return;
        }

        // ── Scan-based compaction (issue #713) ──
        // Three dispatches plus the scan's own, replacing one dispatch that
        // allocated every slot with `atomicAdd`. What that buys: `aliveIndices`
        // and `freeList` now come out in ascending particle index instead of in
        // whatever order invocations reached the counter, so an identical
        // particle buffer produces an identical draw order — which for blended
        // particles is a visible property, and for tests is the difference
        // between "same set" and "same result".

        // Reset counters. AliveCount is overwritten by the scan's total and
        // DeadCount by the scatter pass, but EmitCount must start at zero and a
        // defined struct beats three assignments that must stay in sync.
        GPUParticleCounters counters{};
        counters.AliveCount = 0;
        counters.DeadCount = 0;
        counters.EmitCount = 0;
        counters.Pad = 0;
        m_CounterSSBO->SetData(&counters, sizeof(GPUParticleCounters));

        m_Params = UBOStructures::GPUParticleParamsUBO{};
        m_Params.MaxParticles = m_MaxParticles;

        const u32 groups = (m_MaxParticles + COMPACT_WORKGROUP_SIZE - 1) / COMPACT_WORKGROUP_SIZE;

        // Pass 1 — alive flags into the scan scratch.
        m_ParticleSSBO->Bind();
        m_AliveScanSSBO->Bind();
        m_CompactShader->Bind();
        UploadParams();
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // Pass 2 — exclusive-scan the flags in place. The grand total (the
        // alive count) is written straight into the counter buffer's FIRST
        // u32, which the static_assert above pins to be AliveCount; that is why
        // the scatter pass only has to derive DeadCount.
        //
        // Bail rather than scatter if the scan did not run: pass 3 would read
        // the raw 0/1 flags as if they were prefixes and write every alive
        // particle to slot 0 or 1. A frame with no compaction is a visible
        // stall; a frame with a scattered-onto-itself alive list is corruption.
        if (!m_PrefixSum->ExclusiveScanInPlace(m_AliveScanSSBO, m_MaxParticles, m_CounterSSBO))
        {
            OLO_CORE_ERROR("GPUParticleSystem::Compact: prefix sum failed — skipping the scatter pass");
            return;
        }

        // Pass 3 — prefixes into slots. Re-binds everything the scan displaced:
        // GPUPrefixSum binds its own three SSBOs and its params UBO, so nothing
        // bound before pass 2 can be assumed to have survived it.
        m_ParticleSSBO->Bind();
        m_AliveIndexSSBO->Bind();
        m_CounterSSBO->Bind();
        m_FreeListSSBO->Bind();
        m_AliveScanSSBO->Bind();
        m_CompactScatterShader->Bind();
        UploadParams();
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void GPUParticleSystem::PrepareIndirectDraw()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || !m_BuildIndirectShader || !m_BuildIndirectShader->IsValid())
        {
            return;
        }

        // Bind SSBOs
        m_CounterSSBO->Bind();
        m_IndirectDrawSSBO->Bind();

        m_BuildIndirectShader->Bind();

        RenderCommand::DispatchCompute(1, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::Command | MemoryBarrierFlags::ShaderStorage);
    }

    u32 GPUParticleSystem::GetAliveCount() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return 0;
        }

        auto counters = m_CounterSSBO->GetData<GPUParticleCounters>();
        return counters.AliveCount;
    }

} // namespace OloEngine
