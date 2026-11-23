#include "OloEnginePCH.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Fluid/FluidKernels.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <cmath>
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

    GPUFluidSolver::GPUFluidSolver(u32 maxParticles, FeedbackMode feedbackMode)
        : m_FeedbackMode(feedbackMode)
    {
        Init(maxParticles);
    }

    GPUFluidSolver::~GPUFluidSolver()
    {
        Shutdown();
    }

    GPUFluidSolver::GPUFluidSolver(GPUFluidSolver&& other) noexcept
        : m_FeedbackMode(other.m_FeedbackMode),
          m_FeedbackBatches(std::move(other.m_FeedbackBatches)),
          m_FeedbackHead(std::exchange(other.m_FeedbackHead, 0u)),
          m_MaxParticles(other.m_MaxParticles),
          m_Initialized(other.m_Initialized),
          m_HasDispatched(other.m_HasDispatched),
          m_ParticleUpperBound(other.m_ParticleUpperBound),
          m_PendingEmitCount(other.m_PendingEmitCount),
          m_PendingEmitEntries(std::move(other.m_PendingEmitEntries)),
          m_StepsSinceCountRefresh(other.m_StepsSinceCountRefresh),
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
          m_ImpulseReadback(std::move(other.m_ImpulseReadback)),
          m_PositionsReadback(std::move(other.m_PositionsReadback)),
          m_VelocitiesReadback(std::move(other.m_VelocitiesReadback)),
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
        other.m_HasDispatched = false;
        other.m_MaxParticles = 0;
        other.m_ParticleUpperBound = 0;
        other.m_PendingEmitCount = 0;
        other.m_PendingEmitEntries.Reset();
        other.m_StepsSinceCountRefresh = 0;
        other.m_GridCellCount = 0;
    }

    GPUFluidSolver& GPUFluidSolver::operator=(GPUFluidSolver&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_FeedbackMode = other.m_FeedbackMode;
            m_FeedbackBatches = std::move(other.m_FeedbackBatches);
            m_FeedbackHead = std::exchange(other.m_FeedbackHead, 0u);
            m_MaxParticles = other.m_MaxParticles;
            m_Initialized = other.m_Initialized;
            m_HasDispatched = other.m_HasDispatched;
            m_ParticleUpperBound = other.m_ParticleUpperBound;
            m_PendingEmitCount = other.m_PendingEmitCount;
            m_PendingEmitEntries = std::move(other.m_PendingEmitEntries);
            m_StepsSinceCountRefresh = other.m_StepsSinceCountRefresh;
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
            m_ImpulseReadback = std::move(other.m_ImpulseReadback);
            m_PositionsReadback = std::move(other.m_PositionsReadback);
            m_VelocitiesReadback = std::move(other.m_VelocitiesReadback);
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
            other.m_HasDispatched = false;
            other.m_MaxParticles = 0;
            other.m_ParticleUpperBound = 0;
            other.m_PendingEmitCount = 0;
            other.m_PendingEmitEntries.Reset();
            other.m_StepsSinceCountRefresh = 0;
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
        m_HasDispatched = false;
        m_ParticleUpperBound = 0;
        m_PendingEmitCount = 0;
        m_PendingEmitEntries.Reset();
        m_StepsSinceCountRefresh = 0;
        m_FeedbackBatches.front().ProxyCount = 0;

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
        // Same reason as VirtualShadowMap::DestroyResources: free the raw staging
        // handles while the renderer API is still alive.
        m_ImpulseReadback.Release();
        m_FeedbackHead = 0;
        m_PositionsReadback.Release();
        m_VelocitiesReadback.Release();

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
        m_HasDispatched = false;
        m_ParticleUpperBound = 0;
        m_PendingEmitCount = 0;
        m_PendingEmitEntries.Reset();
        m_StepsSinceCountRefresh = 0;
        m_FeedbackBatches.front().ProxyCount = 0;
        m_GridCellCount = 0;
    }

    void GPUFluidSolver::SeedParticles(std::span<const GPUFluidEmitEntry> entries)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        // These three buffers are GPU-produced after the first Step. A host
        // rewrite could overtake an earlier submitted dispatch, so require a
        // fresh solver for a Vulkan reset until an ordered reset exists.
        if (m_HasDispatched && RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
        {
            OLO_CORE_ERROR("GPUFluidSolver::SeedParticles: Vulkan reset after dispatch refused; create a new solver");
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
        m_PendingEmitEntries.Reset();
        m_StepsSinceCountRefresh = 0;
    }

    bool GPUFluidSolver::UsesTransientStaging()
    {
        // Vulkan's per-batch frame-arena ranges (#1543); the fault lever puts
        // Vulkan on GL's one-buffer reuse path, the negative control.
        return RendererAPI::GetAPI() == RendererAPI::API::Vulkan && !Levers::FaultVulkanStagingReuse();
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

        // Vulkan compute reads the batch when the emit dispatch EXECUTES, so
        // the batch is kept on the CPU and Step binds it as one frame-arena
        // range (#1543). A partial write into a mapped staging buffer inside a
        // recording took a whole-buffer draw snapshot, filled by reading the
        // other 4,000-odd entries back out of write-combined memory: the 3 ms
        // the staging baseline charged a 32-entry emit. GL uploads are ordered
        // against dispatches, so the batch goes straight into its one buffer.
        if (UsesTransientStaging())
        {
            m_PendingEmitEntries.Append(entries.data(), static_cast<i32>(count));
        }
        else
        {
            m_EmitStagingSSBO->SetData(entries.data(), count * GPUFluidEmitEntry::GetSize(),
                                       m_PendingEmitCount * GPUFluidEmitEntry::GetSize());
        }
        m_PendingEmitCount += count;
    }

    void GPUFluidSolver::Step(const FluidSolverParams& params, f32 dt,
                              std::span<const FluidBodyProxy> bodyProxies,
                              std::span<const FluidKillBox> killBoxes,
                              std::span<const UUID> bodyEntities)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || dt <= 0.0f || !std::isfinite(dt))
        {
            return;
        }

        if (m_FeedbackMode == FeedbackMode::Immediate)
        {
            m_FeedbackBatches.front().ProxyCount = 0;
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
        const bool transient = UsesTransientStaging();

        // ---- Body proxies, staged first ----------------------------------------
        // On Vulkan the proxies are a frame-arena range bound once here and
        // read by every displace iteration below (#1543): a later step's
        // proxies take a range of their own, so a displace dispatch still
        // queued from an earlier step keeps the proxies it was recorded with.
        // Bound BEFORE the emit batch takes its range, so a refused step
        // leaves the pending batch intact for the next one.
        const bool transientProxies = transient && proxyCount > 0;
        if (transientProxies)
        {
            const u32 proxyBytes = proxyCount * FluidBodyProxy::GetSize();
            if (!RenderCommand::BindTransientStorage(ShaderBindingLayout::SSBO_FLUID_BODY_PROXIES, bodyProxies.data(),
                                                     proxyBytes))
            {
                OLO_CORE_ERROR("GPUFluidSolver::Step: the frame arena refused {} B of body proxies — refusing step",
                               proxyBytes);
                return;
            }
        }

        u32 emitCount = m_PendingEmitCount;
        if (transient && emitCount > 0)
        {
            const u32 emitBytes = emitCount * GPUFluidEmitEntry::GetSize();
            const bool bound = RenderCommand::BindTransientStorage(ShaderBindingLayout::SSBO_FLUID_EMIT_STAGING,
                                                                   m_PendingEmitEntries.GetData(), emitBytes);
            m_PendingEmitEntries.Reset();
            if (!bound)
            {
                // Dropped, not carried to the next step: an arena full now is
                // full next frame too. The rest of the step still runs.
                OLO_CORE_ERROR("GPUFluidSolver::Step: the frame arena refused {} B of emit staging — dropping {} "
                               "emitted particles",
                               emitBytes, emitCount);
                emitCount = 0;
                m_PendingEmitCount = 0;
            }
        }

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
            if (!transientProxies) // the Vulkan range was bound above
                m_BodyProxiesSSBO->SetData(bodyProxies.data(), proxyCount * FluidBodyProxy::GetSize());
        }
        if (emitCount > 0 || m_ParticleUpperBound > 0)
            m_HasDispatched = true;

        // ---- Emit staged particles ------------------------------------------
        if (emitCount > 0)
        {
            if (!transient) // the batch was bound above, before the UBO took its count
                m_EmitStagingSSBO->Bind();
            m_PositionsSSBO->Bind();
            m_VelocitiesSSBO->Bind();
            m_CountersSSBO->Bind();
            m_EmitShader->Bind();
            RenderCommand::DispatchCompute(GroupCount(emitCount), 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            // Conservative: the shader may reject entries past capacity.
            m_ParticleUpperBound = std::min(m_ParticleUpperBound + emitCount, m_MaxParticles);
            m_PendingEmitCount = 0;
            m_PendingEmitEntries.Reset();
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
            if (!transientProxies) // the arena range bound above stays the occupant
                m_BodyProxiesSSBO->Bind();
            m_BodyImpulsesSSBO->Bind();
            m_DisplaceShader->Bind();
            RenderCommand::DispatchCompute(particleGroups, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
        }

        // Body impulses are complete now that the last displace iteration has
        // run, and the next Step() clears them. The staged copy keeps the
        // batch and its body IDs together across subsequent steps.
        if (proxyCount > 0)
        {
            StageFeedback(proxyCount, params.SolverIterations, bodyEntities);
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

    void GPUFluidSolver::StageFeedback(u32 proxyCount, u32 solverIterations, std::span<const UUID> bodyEntities)
    {
        const RHI::ResourceHandle source = m_BodyImpulsesSSBO->GetRHIHandle();
        const u64 bytes = static_cast<u64>(proxyCount) * sizeof(GPUFluidBodyImpulse);
        u32 batchIndex = 0;
        if (m_FeedbackMode == FeedbackMode::Deferred)
        {
            const u32 outstanding = m_ImpulseReadback.GetDeferredCopiesOutstanding();
            if (!m_ImpulseReadback.StageDeferred(source, 0, bytes))
            {
                return;
            }
            batchIndex = (m_FeedbackHead + outstanding) % StagedBufferReadback::kRingSlots;
        }
        else
        {
            m_ImpulseReadback.Stage(source, 0, bytes);
        }

        FeedbackBatch& batch = m_FeedbackBatches[batchIndex];
        batch.ProxyCount = proxyCount;
        batch.SolverIterations = std::max(1u, solverIterations);
        batch.Entities.Reset();
        if (!bodyEntities.empty())
        {
            batch.Entities.Append(bodyEntities.data(), static_cast<i32>(std::min<sizet>(proxyCount, bodyEntities.size())));
        }
    }

    std::span<const UUID> GPUFluidSolver::HarvestFeedback(std::span<FluidBodyFeedback> outFeedback)
    {
        OLO_PROFILE_FUNCTION();

        for (FluidBodyFeedback& feedback : outFeedback)
        {
            feedback = FluidBodyFeedback{};
        }

        const bool deferred = m_FeedbackMode == FeedbackMode::Deferred;
        const FeedbackBatch& batch = m_FeedbackBatches[m_FeedbackHead];
        if (!m_Initialized || outFeedback.empty() || batch.ProxyCount == 0 ||
            (deferred && m_ImpulseReadback.GetDeferredCopiesOutstanding() == 0))
        {
            return {};
        }

        const auto count = static_cast<u32>(std::min<sizet>(outFeedback.size(), batch.ProxyCount));
        std::array<GPUFluidBodyImpulse, kFluidMaxBodyProxies> raw;
        const u64 rawBytes = static_cast<u64>(count) * sizeof(GPUFluidBodyImpulse);
        // Simulation only polls. A late GPU means no impulse this step.
        // Frameless parity tests deliberately read their one-shot copy.
        const bool copied = deferred ? m_ImpulseReadback.ReadCompleted(raw.data(), rawBytes)
                                     : m_ImpulseReadback.Read(raw.data(), rawBytes);
        if (!copied)
        {
            return {};
        }
        if (deferred)
        {
            m_FeedbackHead = (m_FeedbackHead + 1u) % StagedBufferReadback::kRingSlots;
        }

        // Average over the last step's constraint iterations — the displace
        // pass accumulates per iteration and the lambda solve re-penetrates
        // bodies between iterations (CPU mirror: CPUFluidSolver.cpp feedback
        // averaging; the closed-loop float contract pins the magnitude).
        const f32 invScale = 1.0f / (kFluidImpulseFixedScale * static_cast<f32>(batch.SolverIterations));
        for (u32 i = 0; i < count; ++i)
        {
            outFeedback[i].Impulse = glm::vec3(
                                         static_cast<f32>(raw[i].ImpulseX),
                                         static_cast<f32>(raw[i].ImpulseY),
                                         static_cast<f32>(raw[i].ImpulseZ)) *
                                     invScale;
            outFeedback[i].AngularImpulse = glm::vec3(
                                                static_cast<f32>(raw[i].AngularX),
                                                static_cast<f32>(raw[i].AngularY),
                                                static_cast<f32>(raw[i].AngularZ)) *
                                            invScale;
        }
        if (batch.Entities.IsEmpty())
        {
            return {};
        }
        return { batch.Entities.GetData(), std::min<sizet>(count, batch.Entities.Num()) };
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
        // Stage-then-read-now: still blocking, because a parity test asking for
        // the particles wants THIS step's values, not last step's. Staging only
        // keeps the CPU off the position/velocity SSBOs, which every solver
        // kernel reads and writes.
        const u64 bytes = static_cast<u64>(outCount) * sizeof(glm::vec4);
        m_PositionsReadback.Stage(m_PositionsSSBO->GetRHIHandle(), 0, bytes);
        m_VelocitiesReadback.Stage(m_VelocitiesSSBO->GetRHIHandle(), 0, bytes);
        m_PositionsReadback.Read(outPositions.data(), bytes);
        m_VelocitiesReadback.Read(outVelocities.data(), bytes);
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
