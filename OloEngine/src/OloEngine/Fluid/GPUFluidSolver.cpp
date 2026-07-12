#include "OloEnginePCH.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"

#include "OloEngine/Fluid/FluidKernels.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] constexpr u32 GroupCount(u32 threadCount)
        {
            return (threadCount + kFluidWorkgroupSize - 1u) / kFluidWorkgroupSize;
        }
    } // namespace

    GPUFluidSolver::GPUFluidSolver(u32 maxParticles)
    {
        Init(maxParticles);
    }

    GPUFluidSolver::~GPUFluidSolver()
    {
        Shutdown();
    }

    GPUFluidSolver::GPUFluidSolver(GPUFluidSolver&& other) noexcept
        : m_MaxParticles(other.m_MaxParticles),
          m_Initialized(other.m_Initialized),
          m_ParticleUpperBound(other.m_ParticleUpperBound),
          m_PendingEmitCount(other.m_PendingEmitCount),
          m_StepsSinceCountRefresh(other.m_StepsSinceCountRefresh),
          m_LastProxyCount(other.m_LastProxyCount),
          m_GridCellCount(other.m_GridCellCount),
          m_PositionsSSBO(std::move(other.m_PositionsSSBO)),
          m_VelocitiesSSBO(std::move(other.m_VelocitiesSSBO)),
          m_PredictedASSBO(std::move(other.m_PredictedASSBO)),
          m_PredictedBSSBO(std::move(other.m_PredictedBSSBO)),
          m_AuxSSBO(std::move(other.m_AuxSSBO)),
          m_GridHeadSSBO(std::move(other.m_GridHeadSSBO)),
          m_GridNextSSBO(std::move(other.m_GridNextSSBO)),
          m_CountersSSBO(std::move(other.m_CountersSSBO)),
          m_EmitStagingSSBO(std::move(other.m_EmitStagingSSBO)),
          m_BodyProxiesSSBO(std::move(other.m_BodyProxiesSSBO)),
          m_BodyImpulsesSSBO(std::move(other.m_BodyImpulsesSSBO)),
          m_VelocitiesAltSSBO(std::move(other.m_VelocitiesAltSSBO)),
          m_FluidUBO(std::move(other.m_FluidUBO)),
          m_EmitShader(std::move(other.m_EmitShader)),
          m_KillMarkShader(std::move(other.m_KillMarkShader)),
          m_CompactShader(std::move(other.m_CompactShader)),
          m_CompactCommitShader(std::move(other.m_CompactCommitShader)),
          m_IntegrateShader(std::move(other.m_IntegrateShader)),
          m_GridBuildShader(std::move(other.m_GridBuildShader)),
          m_LambdaShader(std::move(other.m_LambdaShader)),
          m_DisplaceShader(std::move(other.m_DisplaceShader)),
          m_VelocityUpdateShader(std::move(other.m_VelocityUpdateShader)),
          m_VorticityShader(std::move(other.m_VorticityShader)),
          m_VelocityApplyShader(std::move(other.m_VelocityApplyShader)),
          m_FinalizeShader(std::move(other.m_FinalizeShader))
    {
        other.m_Initialized = false;
        other.m_MaxParticles = 0;
        other.m_ParticleUpperBound = 0;
        other.m_PendingEmitCount = 0;
        other.m_StepsSinceCountRefresh = 0;
        other.m_LastProxyCount = 0;
        other.m_GridCellCount = 0;
    }

    GPUFluidSolver& GPUFluidSolver::operator=(GPUFluidSolver&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_MaxParticles = other.m_MaxParticles;
            m_Initialized = other.m_Initialized;
            m_ParticleUpperBound = other.m_ParticleUpperBound;
            m_PendingEmitCount = other.m_PendingEmitCount;
            m_StepsSinceCountRefresh = other.m_StepsSinceCountRefresh;
            m_LastProxyCount = other.m_LastProxyCount;
            m_GridCellCount = other.m_GridCellCount;
            m_PositionsSSBO = std::move(other.m_PositionsSSBO);
            m_VelocitiesSSBO = std::move(other.m_VelocitiesSSBO);
            m_PredictedASSBO = std::move(other.m_PredictedASSBO);
            m_PredictedBSSBO = std::move(other.m_PredictedBSSBO);
            m_AuxSSBO = std::move(other.m_AuxSSBO);
            m_GridHeadSSBO = std::move(other.m_GridHeadSSBO);
            m_GridNextSSBO = std::move(other.m_GridNextSSBO);
            m_CountersSSBO = std::move(other.m_CountersSSBO);
            m_EmitStagingSSBO = std::move(other.m_EmitStagingSSBO);
            m_BodyProxiesSSBO = std::move(other.m_BodyProxiesSSBO);
            m_BodyImpulsesSSBO = std::move(other.m_BodyImpulsesSSBO);
            m_VelocitiesAltSSBO = std::move(other.m_VelocitiesAltSSBO);
            m_FluidUBO = std::move(other.m_FluidUBO);
            m_EmitShader = std::move(other.m_EmitShader);
            m_KillMarkShader = std::move(other.m_KillMarkShader);
            m_CompactShader = std::move(other.m_CompactShader);
            m_CompactCommitShader = std::move(other.m_CompactCommitShader);
            m_IntegrateShader = std::move(other.m_IntegrateShader);
            m_GridBuildShader = std::move(other.m_GridBuildShader);
            m_LambdaShader = std::move(other.m_LambdaShader);
            m_DisplaceShader = std::move(other.m_DisplaceShader);
            m_VelocityUpdateShader = std::move(other.m_VelocityUpdateShader);
            m_VorticityShader = std::move(other.m_VorticityShader);
            m_VelocityApplyShader = std::move(other.m_VelocityApplyShader);
            m_FinalizeShader = std::move(other.m_FinalizeShader);
            other.m_Initialized = false;
            other.m_MaxParticles = 0;
            other.m_ParticleUpperBound = 0;
            other.m_PendingEmitCount = 0;
            other.m_StepsSinceCountRefresh = 0;
            other.m_LastProxyCount = 0;
            other.m_GridCellCount = 0;
        }
        return *this;
    }

    void GPUFluidSolver::Init(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Initialized)
        {
            Shutdown();
        }

        if (maxParticles == 0)
        {
            OLO_CORE_ERROR("GPUFluidSolver: maxParticles must be > 0");
            return;
        }

        m_MaxParticles = maxParticles;
        m_ParticleUpperBound = 0;
        m_PendingEmitCount = 0;
        m_StepsSinceCountRefresh = 0;
        m_LastProxyCount = 0;

        const u32 vec4Bytes = static_cast<u32>(sizeof(glm::vec4));

        m_PositionsSSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_POSITIONS,
            StorageBufferUsage::DynamicCopy);
        // Zero-fill so every slot starts with kill flag w == 0 (never read past
        // the live count anyway — the shaders self-guard, issue #551).
        m_PositionsSSBO->ClearData();

        m_VelocitiesSSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_VELOCITIES,
            StorageBufferUsage::DynamicCopy);
        m_VelocitiesSSBO->ClearData();

        m_PredictedASSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_PREDICTED_A,
            StorageBufferUsage::DynamicCopy);

        m_PredictedBSSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_PREDICTED_B,
            StorageBufferUsage::DynamicCopy);

        m_AuxSSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_AUX,
            StorageBufferUsage::DynamicCopy);

        // Grid heads start at a single cell; Step() resizes to the real cell
        // count on first use (and on every domain change).
        m_GridHeadSSBO = StorageBuffer::Create(
            static_cast<u32>(sizeof(u32)),
            ShaderBindingLayout::SSBO_FLUID_GRID_HEAD,
            StorageBufferUsage::DynamicCopy);
        m_GridHeadSSBO->ClearData();
        m_GridCellCount = 1;

        m_GridNextSSBO = StorageBuffer::Create(
            maxParticles * static_cast<u32>(sizeof(u32)),
            ShaderBindingLayout::SSBO_FLUID_GRID_NEXT,
            StorageBufferUsage::DynamicCopy);

        m_CountersSSBO = StorageBuffer::Create(
            static_cast<u32>(sizeof(GPUFluidCounters)),
            ShaderBindingLayout::SSBO_FLUID_COUNTERS,
            StorageBufferUsage::DynamicCopy);
        const GPUFluidCounters zeroCounters{};
        m_CountersSSBO->SetData(&zeroCounters, sizeof(GPUFluidCounters));

        m_EmitStagingSSBO = StorageBuffer::Create(
            kEmitStagingCapacity * GPUFluidEmitEntry::GetSize(),
            ShaderBindingLayout::SSBO_FLUID_EMIT_STAGING,
            StorageBufferUsage::DynamicDraw);

        m_BodyProxiesSSBO = StorageBuffer::Create(
            kFluidMaxBodyProxies * FluidBodyProxy::GetSize(),
            ShaderBindingLayout::SSBO_FLUID_BODY_PROXIES,
            StorageBufferUsage::DynamicDraw);

        m_BodyImpulsesSSBO = StorageBuffer::Create(
            kFluidMaxBodyProxies * static_cast<u32>(sizeof(GPUFluidBodyImpulse)),
            ShaderBindingLayout::SSBO_FLUID_BODY_IMPULSES,
            StorageBufferUsage::DynamicCopy);
        m_BodyImpulsesSSBO->ClearData();

        m_VelocitiesAltSSBO = StorageBuffer::Create(
            maxParticles * vec4Bytes,
            ShaderBindingLayout::SSBO_FLUID_VELOCITIES_ALT,
            StorageBufferUsage::DynamicCopy);

        m_FluidUBO = UniformBuffer::Create(UBOStructures::FluidUBO::GetSize(),
                                           ShaderBindingLayout::UBO_FLUID);

        // Load and validate every compute pass; bail out whole on any failure
        // (GPUParticleSystem::Init pattern).
        struct ShaderSlot
        {
            Ref<ComputeShader>* Target;
            const char* Path;
        };
        const ShaderSlot shaderSlots[] = {
            { &m_EmitShader, "assets/shaders/compute/Fluid_Emit.comp" },
            { &m_KillMarkShader, "assets/shaders/compute/Fluid_KillMark.comp" },
            { &m_CompactShader, "assets/shaders/compute/Fluid_Compact.comp" },
            { &m_CompactCommitShader, "assets/shaders/compute/Fluid_CompactCommit.comp" },
            { &m_IntegrateShader, "assets/shaders/compute/Fluid_Integrate.comp" },
            { &m_GridBuildShader, "assets/shaders/compute/Fluid_GridBuild.comp" },
            { &m_LambdaShader, "assets/shaders/compute/Fluid_Lambda.comp" },
            { &m_DisplaceShader, "assets/shaders/compute/Fluid_Displace.comp" },
            { &m_VelocityUpdateShader, "assets/shaders/compute/Fluid_VelocityUpdate.comp" },
            { &m_VorticityShader, "assets/shaders/compute/Fluid_Vorticity.comp" },
            { &m_VelocityApplyShader, "assets/shaders/compute/Fluid_VelocityApply.comp" },
            { &m_FinalizeShader, "assets/shaders/compute/Fluid_Finalize.comp" },
        };
        for (const ShaderSlot& slot : shaderSlots)
        {
            *slot.Target = ComputeShader::Create(slot.Path);
            if (!(*slot.Target) || !(*slot.Target)->IsValid())
            {
                OLO_CORE_ERROR("GPUFluidSolver: failed to load compute shader '{}'", slot.Path);
                Shutdown();
                return;
            }
        }

        m_Initialized = true;
    }

    void GPUFluidSolver::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        m_PositionsSSBO = nullptr;
        m_VelocitiesSSBO = nullptr;
        m_PredictedASSBO = nullptr;
        m_PredictedBSSBO = nullptr;
        m_AuxSSBO = nullptr;
        m_GridHeadSSBO = nullptr;
        m_GridNextSSBO = nullptr;
        m_CountersSSBO = nullptr;
        m_EmitStagingSSBO = nullptr;
        m_BodyProxiesSSBO = nullptr;
        m_BodyImpulsesSSBO = nullptr;
        m_VelocitiesAltSSBO = nullptr;
        m_FluidUBO = nullptr;
        m_EmitShader = nullptr;
        m_KillMarkShader = nullptr;
        m_CompactShader = nullptr;
        m_CompactCommitShader = nullptr;
        m_IntegrateShader = nullptr;
        m_GridBuildShader = nullptr;
        m_LambdaShader = nullptr;
        m_DisplaceShader = nullptr;
        m_VelocityUpdateShader = nullptr;
        m_VorticityShader = nullptr;
        m_VelocityApplyShader = nullptr;
        m_FinalizeShader = nullptr;
        m_Initialized = false;
        m_ParticleUpperBound = 0;
        m_PendingEmitCount = 0;
        m_StepsSinceCountRefresh = 0;
        m_LastProxyCount = 0;
        m_GridCellCount = 0;
    }

    void GPUFluidSolver::SeedParticles(std::span<const GPUFluidEmitEntry> entries)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        const u32 count = static_cast<u32>(std::min<sizet>(entries.size(), m_MaxParticles));

        if (count > 0)
        {
            // Sanitize w: GPU-side w is the kill flag (must start at 0/alive).
            std::vector<glm::vec4> positions(count);
            std::vector<glm::vec4> velocities(count);
            for (u32 i = 0; i < count; ++i)
            {
                positions[i] = glm::vec4(glm::vec3(entries[i].Position), 0.0f);
                velocities[i] = glm::vec4(glm::vec3(entries[i].Velocity), 0.0f);
            }
            m_PositionsSSBO->SetData(positions.data(), count * static_cast<u32>(sizeof(glm::vec4)));
            m_VelocitiesSSBO->SetData(velocities.data(), count * static_cast<u32>(sizeof(glm::vec4)));
        }

        GPUFluidCounters counters{};
        counters.Count = count;
        m_CountersSSBO->SetData(&counters, sizeof(GPUFluidCounters));

        m_ParticleUpperBound = count;
        m_PendingEmitCount = 0;
        m_StepsSinceCountRefresh = 0;
    }

    void GPUFluidSolver::Emit(std::span<const GPUFluidEmitEntry> entries)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || entries.empty())
        {
            return;
        }

        const u32 remainingSpace = kEmitStagingCapacity - m_PendingEmitCount;
        const u32 count = static_cast<u32>(std::min<sizet>(entries.size(), remainingSpace));
        if (count == 0)
        {
            return;
        }

        m_EmitStagingSSBO->SetData(entries.data(), count * GPUFluidEmitEntry::GetSize(),
                                   m_PendingEmitCount * GPUFluidEmitEntry::GetSize());
        m_PendingEmitCount += count;
    }

    void GPUFluidSolver::Step(const FluidSolverParams& params, f32 dt,
                              std::span<const FluidBodyProxy> bodyProxies,
                              std::span<const FluidKillBox> killBoxes)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || dt <= 0.0f || !std::isfinite(dt))
        {
            return;
        }

        // ---- Grid sizing — CPU mirror: CPUFluidSolver::BuildGrid ------------
        const f32 h = params.SmoothingRadius();
        const glm::vec3 extent = params.BoundsMax - params.BoundsMin;
        const f32 maxExtent = std::max({ extent.x, extent.y, extent.z, h });
        const f32 cellSize = std::max(h, maxExtent / static_cast<f32>(kFluidMaxGridCellsPerAxis));
        const glm::uvec3 gridDims(
            std::max(1u, static_cast<u32>(std::ceil(extent.x / cellSize))),
            std::max(1u, static_cast<u32>(std::ceil(extent.y / cellSize))),
            std::max(1u, static_cast<u32>(std::ceil(extent.z / cellSize))));
        const u32 cellCount = gridDims.x * gridDims.y * gridDims.z;

        if (cellCount != m_GridCellCount)
        {
            m_GridHeadSSBO->Resize(cellCount * static_cast<u32>(sizeof(u32)));
            m_GridHeadSSBO->ClearData(); // Resize invalidates contents
            m_GridCellCount = cellCount;
        }

        const u32 proxyCount = static_cast<u32>(std::min<sizet>(bodyProxies.size(), kFluidMaxBodyProxies));
        const u32 killBoxCount = static_cast<u32>(
            std::min<sizet>(killBoxes.size(), UBOStructures::FluidUBO::kMaxKillBoxes));
        const u32 emitCount = m_PendingEmitCount;

        // ---- FluidUBO upload (StepFlags.x = 0: iteration 0 reads parity A) --
        const f32 poly6ScaleValue = FluidKernels::Poly6Scale(h);
        UBOStructures::FluidUBO ubo{};
        ubo.BoundsMinCellSize = glm::vec4(params.BoundsMin, cellSize);
        ubo.BoundsMaxDt = glm::vec4(params.BoundsMax, dt);
        ubo.GravityH = glm::vec4(params.Gravity, h);
        ubo.KernelScales = glm::vec4(
            poly6ScaleValue,
            FluidKernels::SpikyGradScale(h),
            FluidKernels::Poly6(params.SCorrDeltaQ * h, h, poly6ScaleValue),
            params.ParticleMass());
        ubo.PbfParams = glm::vec4(1.0f / params.RestDensity, params.CfmEpsilon,
                                  params.SCorrK, params.SCorrN);
        ubo.ViscosityParams = glm::vec4(params.XsphViscosity, params.VorticityEpsilon,
                                        params.MaxSpeed, params.ParticleRadius);
        ubo.CouplingParams = glm::vec4(params.CouplingStiffness, kFluidImpulseFixedScale,
                                       kFluidMaxDeltaPFraction * h, kFluidJacobiRelaxation);
        ubo.GridDims = glm::uvec4(gridDims, cellCount);
        ubo.Counts = glm::uvec4(m_MaxParticles, emitCount, proxyCount, killBoxCount);
        ubo.StepFlags = glm::uvec4(0u);
        // Fill every slot (glm default ctors don't zero-init — unused entries
        // must not upload uninitialized stack bytes).
        for (u32 b = 0; b < UBOStructures::FluidUBO::kMaxKillBoxes; ++b)
        {
            ubo.KillBoxMin[b] = b < killBoxCount ? glm::vec4(killBoxes[b].Min, 0.0f) : glm::vec4(0.0f);
            ubo.KillBoxMax[b] = b < killBoxCount ? glm::vec4(killBoxes[b].Max, 0.0f) : glm::vec4(0.0f);
        }
        m_FluidUBO->SetData(&ubo, sizeof(ubo));
        m_FluidUBO->Bind();

        // ---- Body proxies: clear last step's impulses + upload snapshots ----
        if (proxyCount > 0)
        {
            m_BodyImpulsesSSBO->ClearData();
            m_BodyProxiesSSBO->SetData(bodyProxies.data(), proxyCount * FluidBodyProxy::GetSize());
        }
        m_LastProxyCount = proxyCount;
        m_LastSolverIterations = std::max(1u, params.SolverIterations);

        // ---- Emit staged particles ------------------------------------------
        if (emitCount > 0)
        {
            m_CountersSSBO->SetData(&emitCount, static_cast<u32>(sizeof(u32)),
                                    static_cast<u32>(offsetof(GPUFluidCounters, EmitCount)));

            m_PositionsSSBO->Bind();
            m_VelocitiesSSBO->Bind();
            m_CountersSSBO->Bind();
            m_EmitStagingSSBO->Bind();
            m_EmitShader->Bind();
            RenderCommand::DispatchCompute(GroupCount(emitCount), 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            // Conservative: the shader may reject entries past capacity.
            m_ParticleUpperBound = std::min(m_ParticleUpperBound + emitCount, m_MaxParticles);
            m_PendingEmitCount = 0;
        }

        const u32 upperBound = m_ParticleUpperBound;
        if (upperBound == 0)
        {
            return;
        }
        const u32 particleGroups = GroupCount(upperBound);

        // ---- Kill volumes: mark -> compact -> commit -------------------------
        if (killBoxCount > 0)
        {
            m_PositionsSSBO->Bind();
            m_CountersSSBO->Bind();
            m_KillMarkShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            m_PositionsSSBO->Bind();
            m_VelocitiesSSBO->Bind();
            m_PredictedASSBO->Bind();
            m_CountersSSBO->Bind();
            m_VelocitiesAltSSBO->Bind();
            m_CompactShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            m_PositionsSSBO->Bind();
            m_VelocitiesSSBO->Bind();
            m_PredictedASSBO->Bind();
            m_CountersSSBO->Bind();
            m_VelocitiesAltSSBO->Bind();
            m_CompactCommitShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
        }

        // ---- Integrate: gravity + prediction into parity A -------------------
        m_PositionsSSBO->Bind();
        m_VelocitiesSSBO->Bind();
        m_PredictedASSBO->Bind();
        m_CountersSSBO->Bind();
        m_IntegrateShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Neighbour grid (heads zero-filled CPU-side, no clear shader) ----
        m_GridHeadSSBO->ClearData();
        m_PredictedASSBO->Bind();
        m_GridHeadSSBO->Bind();
        m_GridNextSSBO->Bind();
        m_CountersSSBO->Bind();
        m_GridBuildShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Jacobi constraint iterations (lambda -> displace, ping-pong) ----
        u32 uploadedParity = 0;
        for (u32 iteration = 0; iteration < params.SolverIterations; ++iteration)
        {
            const u32 parity = iteration & 1u;
            if (parity != uploadedParity)
            {
                ubo.StepFlags.x = parity;
                m_FluidUBO->SetData(&ubo, sizeof(ubo));
                m_FluidUBO->Bind();
                uploadedParity = parity;
            }

            m_PredictedASSBO->Bind();
            m_PredictedBSSBO->Bind();
            m_AuxSSBO->Bind();
            m_GridHeadSSBO->Bind();
            m_GridNextSSBO->Bind();
            m_CountersSSBO->Bind();
            m_LambdaShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            m_PredictedASSBO->Bind();
            m_PredictedBSSBO->Bind();
            m_AuxSSBO->Bind();
            m_GridHeadSSBO->Bind();
            m_GridNextSSBO->Bind();
            m_CountersSSBO->Bind();
            m_BodyProxiesSSBO->Bind();
            m_BodyImpulsesSSBO->Bind();
            m_DisplaceShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
        }

        // ---- Final parity: iteration 0 reads A and writes B, so N iterations
        // leave the result in B when N is odd, A when N is even. --------------
        const u32 finalParity = (params.SolverIterations % 2u == 1u) ? 1u : 0u;
        if (finalParity != uploadedParity)
        {
            ubo.StepFlags.x = finalParity;
            m_FluidUBO->SetData(&ubo, sizeof(ubo));
            m_FluidUBO->Bind();
        }

        // ---- Velocity from displacement ---------------------------------------
        m_PositionsSSBO->Bind();
        m_VelocitiesSSBO->Bind();
        m_PredictedASSBO->Bind();
        m_PredictedBSSBO->Bind();
        m_CountersSSBO->Bind();
        m_VelocityUpdateShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Vorticity omega ---------------------------------------------------
        m_VelocitiesSSBO->Bind();
        m_PredictedASSBO->Bind();
        m_PredictedBSSBO->Bind();
        m_AuxSSBO->Bind();
        m_GridHeadSSBO->Bind();
        m_GridNextSSBO->Bind();
        m_CountersSSBO->Bind();
        m_VorticityShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Vorticity confinement + XSPH into the velocity pong buffer -------
        m_VelocitiesSSBO->Bind();
        m_PredictedASSBO->Bind();
        m_PredictedBSSBO->Bind();
        m_AuxSSBO->Bind();
        m_GridHeadSSBO->Bind();
        m_GridNextSSBO->Bind();
        m_CountersSSBO->Bind();
        m_VelocitiesAltSSBO->Bind();
        m_VelocityApplyShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Commit ------------------------------------------------------------
        m_PositionsSSBO->Bind();
        m_VelocitiesSSBO->Bind();
        m_PredictedASSBO->Bind();
        m_PredictedBSSBO->Bind();
        m_CountersSSBO->Bind();
        m_VelocitiesAltSSBO->Bind();
        m_FinalizeShader->Bind();
        RenderCommand::DispatchCompute(particleGroups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // ---- Periodic exact-count refresh (issue #551: never per step) --------
        ++m_StepsSinceCountRefresh;
        if (m_StepsSinceCountRefresh >= kCountRefreshInterval)
        {
            RefreshExactCount();
        }
    }

    void GPUFluidSolver::HarvestFeedback(std::span<FluidBodyFeedback> outFeedback)
    {
        OLO_PROFILE_FUNCTION();

        for (FluidBodyFeedback& feedback : outFeedback)
        {
            feedback = FluidBodyFeedback{};
        }

        if (!m_Initialized || m_LastProxyCount == 0 || outFeedback.empty())
        {
            return;
        }

        const u32 count = static_cast<u32>(std::min<sizet>(outFeedback.size(), m_LastProxyCount));

        // Shader writes -> CPU read via GetSubData needs a BufferUpdate barrier
        // per the GL memory model (cheap; the buffer is <= 2 KB).
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);

        std::vector<GPUFluidBodyImpulse> raw(count);
        m_BodyImpulsesSSBO->GetData(raw.data(), count * static_cast<u32>(sizeof(GPUFluidBodyImpulse)));

        // Average over the last step's constraint iterations — the displace
        // pass accumulates per iteration and the lambda solve re-penetrates
        // bodies between iterations (CPU mirror: CPUFluidSolver.cpp feedback
        // averaging; the closed-loop float contract pins the magnitude).
        const f32 kInvScale = 1.0f / (kFluidImpulseFixedScale * static_cast<f32>(m_LastSolverIterations));
        for (u32 i = 0; i < count; ++i)
        {
            outFeedback[i].Impulse = glm::vec3(
                                         static_cast<f32>(raw[i].ImpulseX),
                                         static_cast<f32>(raw[i].ImpulseY),
                                         static_cast<f32>(raw[i].ImpulseZ)) *
                                     kInvScale;
            outFeedback[i].AngularImpulse = glm::vec3(
                                                static_cast<f32>(raw[i].AngularX),
                                                static_cast<f32>(raw[i].AngularY),
                                                static_cast<f32>(raw[i].AngularZ)) *
                                            kInvScale;
        }
    }

    u32 GPUFluidSolver::RefreshExactCount()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return 0;
        }

        RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);
        const auto counters = m_CountersSSBO->GetData<GPUFluidCounters>();
        m_ParticleUpperBound = std::min(counters.Count, m_MaxParticles);
        m_StepsSinceCountRefresh = 0;
        return m_ParticleUpperBound;
    }

    void GPUFluidSolver::ReadbackParticles(std::vector<glm::vec4>& outPositions,
                                           std::vector<glm::vec4>& outVelocities, u32& outCount)
    {
        OLO_PROFILE_FUNCTION();

        outCount = 0;
        outPositions.clear();
        outVelocities.clear();

        if (!m_Initialized)
        {
            return;
        }

        outCount = RefreshExactCount();
        if (outCount == 0)
        {
            return;
        }

        outPositions.resize(outCount);
        outVelocities.resize(outCount);
        const u32 bytes = outCount * static_cast<u32>(sizeof(glm::vec4));
        m_PositionsSSBO->GetData(outPositions.data(), bytes);
        m_VelocitiesSSBO->GetData(outVelocities.data(), bytes);
    }

    void GPUFluidSolver::Reload()
    {
        OLO_PROFILE_FUNCTION();

        Ref<ComputeShader>* const shaders[] = {
            &m_EmitShader,
            &m_KillMarkShader,
            &m_CompactShader,
            &m_CompactCommitShader,
            &m_IntegrateShader,
            &m_GridBuildShader,
            &m_LambdaShader,
            &m_DisplaceShader,
            &m_VelocityUpdateShader,
            &m_VorticityShader,
            &m_VelocityApplyShader,
            &m_FinalizeShader,
        };
        for (Ref<ComputeShader>* shader : shaders)
        {
            if (*shader)
            {
                (*shader)->Reload();
            }
        }
    }
} // namespace OloEngine
