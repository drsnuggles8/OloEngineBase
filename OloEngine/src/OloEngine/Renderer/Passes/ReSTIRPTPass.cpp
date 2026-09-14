#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ReSTIRPTPass.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
#include "OloEngine/Renderer/PathTracing/MaterialTextureTable.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTGPU.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <limits>
#include <string>

namespace OloEngine
{
    ReSTIRPTPass::ReSTIRPTPass()
    {
        SetName("ReSTIRPTPass");
    }

    bool ReSTIRPTPass::IsReadyForExecution() const noexcept
    {
        return m_Shader && m_Shader->IsReady() && m_Parameters;
    }

    void ReSTIRPTPass::Init(const FramebufferSpecification& spec)
    {
        m_Width = spec.Width;
        m_Height = spec.Height;
        if (!RenderCommand::SupportsRayTracing())
            return;
        m_Shader = Shader::Create("assets/shaders/ReSTIR_PT.glsl");
        m_Parameters = UniformBuffer::Create(sizeof(ReSTIR::PT::Parameters), ShaderBindingLayout::UBO_RAY_TRACING);
    }

    void ReSTIRPTPass::SetSettings(const ReSTIRPTSettings& settings) noexcept
    {
        const auto sanitized = SanitizeReSTIRPTSettings(settings);
        // Output diagnostics and the optional resolve clamp do not change stored
        // suffixes. Inspecting history must not invalidate the history being shown.
        auto historySettings = sanitized;
        historySettings.DebugView = m_Settings.DebugView;
        historySettings.RadianceClamp = m_Settings.RadianceClamp;
        if (!(m_Settings == historySettings))
            m_HaveHistory = false;
        m_Settings = sanitized;
    }

    void ReSTIRPTPass::SetSceneEpoch(u64 epoch) noexcept
    {
        if (m_SceneEpoch != epoch)
            m_HaveHistory = false;
        m_SceneEpoch = epoch;
    }

    void ReSTIRPTPass::SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                                         const glm::vec3& origin) noexcept
    {
        if (!Math::BitwiseEqual(m_Origin, origin))
            m_HaveHistory = false;
        m_View = view;
        m_Projection = projection;
        m_Origin = origin;
    }

    void ReSTIRPTPass::SetEnvironment(const glm::vec3& rgb, f32 intensity, bool cubeBound) noexcept
    {
        if (!Math::BitwiseEqual(m_Environment, rgb) || !Math::BitwiseEqual(m_EnvironmentIntensity, intensity) ||
            m_CubeBound != cubeBound)
            m_HaveHistory = false;
        m_Environment = rgb;
        m_EnvironmentIntensity = intensity;
        m_CubeBound = cubeBound;
    }

    void ReSTIRPTPass::StandDown(std::string_view reason)
    {
        m_Stats.Active = false;
        m_Stats.HistoryValid = false;
        m_Stats.FallbackReason = reason;
        m_HaveHistory = false;
        if (m_LastFallback != reason)
        {
            OLO_CORE_INFO("ReSTIRPTPass: {}", reason);
            m_LastFallback = reason;
        }
    }

    bool ReSTIRPTPass::EnsureBuffers()
    {
        const u64 pixels = static_cast<u64>(m_Width) * m_Height;
        if (pixels == 0u || pixels > std::numeric_limits<u32>::max() / sizeof(ReSTIR::PT::PathRecord))
        {
            StandDown("suffix allocation exceeds 32-bit buffer capacity");
            return false;
        }
        const u32 bytes = static_cast<u32>(pixels * sizeof(ReSTIR::PT::PathRecord));
        if (m_Pools[0] && m_Pools[0]->GetSize() == bytes && m_Counters)
            return true;
        m_HaveHistory = false;
        m_HaveCounters = false;
        try
        {
            std::array<Ref<StorageBuffer>, 4> pools;
            for (auto& pool : pools)
            {
                pool = StorageBuffer::Create(bytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
                if (!pool || pool->GetDeviceAddress() == 0u)
                {
                    StandDown("suffix buffer allocation unavailable");
                    return false;
                }
            }
            auto counters = StorageBuffer::Create(sizeof(u32) * ReSTIR::PT::kCounterCount,
                                                  StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
            if (!counters || counters->GetDeviceAddress() == 0u)
            {
                StandDown("counter allocation unavailable");
                return false;
            }
            m_Pools = std::move(pools);
            m_Counters = std::move(counters);
            m_Stats.ReservoirBytes = static_cast<u64>(bytes) * 4u;
        }
        catch (const std::exception& error)
        {
            OLO_CORE_ERROR("ReSTIRPTPass allocation failed: {}", error.what());
            StandDown("suffix allocation failed");
            return false;
        }
        return true;
    }

    void ReSTIRPTPass::ResolveAvailabilityForFrame(bool deferredPathActive, bool participatingMedia)
    {
        m_DeferredPathActive = deferredPathActive;
        m_ParticipatingMedia = participatingMedia;
        m_Stats = {};
        m_Stats.Requested = m_Enabled && m_Settings.Enabled;
        m_Stats.SceneEpoch = m_SceneEpoch;
        m_Stats.ReservoirBytes = m_Pools[0] ? static_cast<u64>(m_Pools[0]->GetSize()) * 4u : 0u;
        m_Stats.BiasedClamp = m_Settings.RadianceClamp > 0.0f;
        if (!m_Stats.Requested)
        {
            StandDown("disabled");
            return;
        }
        if (participatingMedia)
        {
            StandDown("participating media unsupported");
            return;
        }
        if (!deferredPathActive)
        {
            StandDown("deferred path required");
            return;
        }
        if (!RenderCommand::SupportsRayTracing() || !m_RayTracingScene || !m_RayTracingScene->IsAvailable())
        {
            StandDown("hardware ray queries unavailable");
            return;
        }
        if (!IsReadyForExecution())
        {
            StandDown("shader unavailable");
            return;
        }
        if (m_RayTracingScene->GetTlasDeviceAddress() == 0u || m_RayTracingScene->GetStats().Resident.TlasInstances == 0u)
        {
            StandDown("acceleration structure empty");
            return;
        }
        if (!m_GPUScene || m_GPUScene->GetInstanceSlotCount() == 0u)
        {
            StandDown("GPU scene unavailable");
            return;
        }
        if (m_GPUScene->GetLastFrameUpdate().m_Stats.m_UnsupportedTotal != 0u)
        {
            StandDown("incomplete GPU scene geometry coverage");
            return;
        }
        const auto& resident = m_RayTracingScene->GetStats().Resident;
        if (resident.UnsupportedInstances != 0u ||
            resident.BlasByClass[static_cast<sizet>(RayTracing::GeometryClass::Masked)] != 0u ||
            resident.BlasByClass[static_cast<sizet>(RayTracing::GeometryClass::Deformed)] != 0u)
        {
            StandDown("unsupported or non-opaque geometry");
            return;
        }
        for (u32 i = 0; i < m_GPUScene->GetInstanceSlotCount(); ++i)
        {
            const auto* instance = m_GPUScene->GetLiveInstanceRecordBySlot(i);
            if (!instance)
                continue;
            const auto* material = m_GPUScene->GetLiveMaterialRecordBySlot(instance->MaterialIndex, instance->MaterialGeneration);
            if (!material || material->AlphaMode != 0u || material->ClosureVersion >= 2u ||
                (material->Flags & (GPUSceneMaterialFlagBlend | GPUSceneMaterialFlagTransmission)) != 0u)
            {
                StandDown("unsupported material");
                return;
            }
        }
        for (u32 i = 0; i < m_GPUScene->GetLightSlotCount(); ++i)
        {
            if (const auto* light = m_GPUScene->GetLiveLightRecordBySlot(i); light != nullptr)
            {
                if (i >= 256u || light->Type == static_cast<u32>(GPUSceneLightType::SphereArea))
                {
                    StandDown("unsupported finite-radius light or light slot bound");
                    return;
                }
            }
        }
        if (!EnsureBuffers())
            return;
        m_Stats.Active = true;
        m_Stats.FallbackReason = "none";
        m_LastFallback = "none";
    }

    void ReSTIRPTPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_Inputs = { board.Scene.SceneDepth, board.GBuffer.GBufferAlbedo, board.GBuffer.GBufferNormal,
                     board.GBuffer.GBufferEmissive, board.GBuffer.Velocity, board.IBL.PrefilterMap };
        m_Targets = { board.Scratch.ReSTIRPTInitial, board.Scratch.ReSTIRPTTemporal, board.Scratch.ReSTIRPTSpatial };
        if (!m_Stats.Active || !board.Lighting.ReSTIRPTRadiance.IsValid())
            return;
        // Resize can run between per-frame configuration and graph setup.
        // Never import a cleared pool based solely on the earlier verdict.
        if (!EnsureBuffers())
            return;
        for (sizet i = 0; i < 4u; ++i)
            if (!m_Inputs[i].IsValid())
            {
                StandDown("G-buffer unavailable");
                return;
            }
        builder.DependsOnPass("RayTracingScenePass");
        for (const auto input : m_Inputs)
            if (input.IsValid())
            {
                [[maybe_unused]] const auto read = builder.Read(input, RGReadUsage::ShaderSample);
            }
        for (const auto target : m_Targets)
        {
            if (!target.IsValid())
            {
                StandDown("scratch target unavailable");
                return;
            }
            builder.Write(target, RGWriteUsage::RenderTarget);
        }
        const auto importPool = [&builder](std::string_view name, const Ref<StorageBuffer>& pool)
        {
            RGResourceDesc desc{};
            desc.Kind = RGResourceHandle::Kind::StorageBuffer;
            desc.Width = pool->GetSize();
            const auto handle = builder.ImportBufferHandle(name, pool->GetRHIHandle(), desc);
            builder.AllowSamePassReadWrite(handle);
            [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderStorage);
            builder.Write(handle, RGWriteUsage::ShaderStorage);
        };
        const sizet poolCount = m_Pools.size();
        for (sizet i = 0; i < poolCount; ++i)
            importPool("ReSTIRPTPool" + std::to_string(i), m_Pools[i]);
        importPool("ReSTIRPTCounters", m_Counters);
        const auto output = builder.WriteNewVersion(board.Lighting.ReSTIRPTRadiance, RGWriteUsage::RenderTarget, "ReSTIRPTPass");
        SetPrimaryOutputFramebufferHandle(output);
        SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView("ReSTIRPTRadianceTexture@ReSTIRPTPass", output, 0u));
        board.Lighting.ReSTIRPTRadianceTexture = GetPrimaryOutputTextureHandle();
    }

    void ReSTIRPTPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        ResolveAvailabilityForFrame(m_DeferredPathActive, m_ParticipatingMedia);
        auto output = context.ResolveFramebuffer(GetPrimaryOutputFramebufferHandle());
        // Clear alpha before every possible early exit: a resolved graph handle
        // alone cannot certify that this frame produced an indirect estimate.
        if (output)
        {
            output->Bind();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetColorMask(true, true, true, true);
            constexpr std::array<u32, 4> attachments{ 0u, 1u, 2u, 3u };
            RenderCommand::SetDrawBuffers(attachments);
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
            context.Clear();
            output->Unbind();
        }
        if (!m_Stats.Active)
            return;
        std::array<Ref<Framebuffer>, 4> targets{
            context.ResolveFramebuffer(m_Targets[0]), context.ResolveFramebuffer(m_Targets[1]),
            context.ResolveFramebuffer(m_Targets[2]), output
        };
        for (const auto& target : targets)
            if (!target)
            {
                StandDown("render target unavailable");
                return;
            }
        std::array<RHI::ResourceHandle, 6> textures;
        const sizet textureCount = textures.size();
        for (sizet i = 0; i < textureCount; ++i)
            textures[i] = m_Inputs[i].IsValid() ? context.ResolveTextureHandle(m_Inputs[i]) : RHI::NullResource;
        for (sizet i = 0; i < 4u; ++i)
            if (!textures[i].IsValid())
            {
                StandDown("G-buffer unresolved");
                return;
            }

        // Sampler and transport edits change the meaning of retained path records.
        // A failed reload keeps the old shader and therefore keeps its history.
        const u64 shaderRevision = m_Shader->GetReloadRevision();
        if (m_ShaderReloadRevision != shaderRevision)
        {
            m_ShaderReloadRevision = shaderRevision;
            m_HaveHistory = false;
            m_HaveCounters = false;
            m_Stats.CountersValid = false;
        }

        // GetData synchronizes the previous submitted work on the active backend.
        // This prototype accepts the stall; the reported frame is the producer.
        if (m_HaveCounters)
        {
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
            m_Counters->GetData(m_Stats.Counters.data(), sizeof(m_Stats.Counters));
            m_Stats.CountersValid = true;
            m_Stats.CounterFrame = m_CounterFrame;
        }
        m_Counters->ClearData();
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        const u32 current = m_FrameIndex % 2u;
        const u32 previous = 1u - current;
        const bool consecutiveFrame = m_FrameIndex != 0u && m_LastFrame == m_FrameIndex - 1u;
        const bool historyIdentityMatches = m_HaveHistory && consecutiveFrame && m_LastEpoch == m_SceneEpoch;
        const bool history = historyIdentityMatches && textures[4].IsValid();
        m_Stats.HistoryValid = history && m_Settings.TemporalReuse;
        ReSTIR::PT::Parameters params{};
        params.View = MakeViewRelative(m_View, m_Origin);
        params.InvView = glm::inverse(params.View);
        params.InvProjection = RHI::AdjustedInverseForShaderReconstruction(m_Projection);
        const auto address = [](u64 value)
        { return glm::uvec2(static_cast<u32>(value), static_cast<u32>(value >> 32u)); };
        params.TlasAddressAndFrame = glm::uvec4(address(m_RayTracingScene->GetTlasDeviceAddress()),
                                                RayTracing::kInstanceMaskAll, m_FrameIndex);
        params.SlotCounts = { m_GPUScene->GetInstanceSlotCount(), m_GPUScene->GetGeometrySlotCount(),
                              m_GPUScene->GetMaterialSlotCount(), m_GPUScene->GetLightSlotCount() };
        const u64 emitterAddress = m_EmissiveTable ? m_EmissiveTable->GetDeviceAddress() : 0u;
        const u32 emitterCount = emitterAddress != 0u ? m_EmissiveTable->GetTriangleCount() : 0u;
        const bool materialTextures = m_MaterialTextures && m_MaterialTextures->GetDeviceAddress() != 0u &&
                                      m_MaterialTextures->GetSamplerHeapOffset() != RHI::HeapOffset::Invalid;
        const bool environmentCube = m_CubeBound && textures[5].IsValid();
        params.EmissiveTable = glm::uvec4(address(emitterAddress), emitterCount,
                                          (materialTextures ? ReSTIR::PT::kMaterialTexturesFlag : 0u) | (environmentCube ? ReSTIR::PT::kEnvironmentCubeFlag : 0u));
        if (materialTextures)
            params.MaterialTable = glm::uvec4(address(m_MaterialTextures->GetDeviceAddress()),
                                              m_MaterialTextures->GetRecordCount(), m_MaterialTextures->GetSamplerHeapOffset());
        params.HistoryAddresses = glm::uvec4(address(m_Pools[previous]->GetDeviceAddress()), address(m_Pools[current]->GetDeviceAddress()));
        params.Counts = { m_Settings.InitialCandidates, 0u, m_Settings.MappingMask, static_cast<u32>(m_SceneEpoch) };
        params.Params = { m_Settings.NormalBias, m_Settings.RayEpsilon, m_Settings.MaxRayDistance, 0.2f };
        params.EstimatorParams = { emitterCount != 0u ? m_EmissiveTable->GetPdfArea() : 0.0f,
                                   m_EnvironmentIntensity, m_Settings.RadianceClamp, m_Settings.SpatialRadius };
        params.Environment = glm::vec4(m_Environment, 0.0f);
        params.Screen = { static_cast<f32>(m_Width), static_cast<f32>(m_Height), 1.0f / m_Width, 1.0f / m_Height };
        params.Reuse = { m_Settings.TemporalReuse ? 1u : 0u, m_Settings.SpatialReuse ? 1u : 0u,
                         history ? 1u : 0u, static_cast<u32>(m_SceneEpoch >> 32u) };
        params.Debug = { static_cast<f32>(m_Settings.DebugView), static_cast<f32>(m_Settings.Seed), m_Settings.ConfidenceCap, static_cast<f32>(ReSTIR::PT::kLayoutVersion) };
        m_GPUScene->Bind();
        constexpr std::array<u32, 5> textureUnits{
            ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, ShaderBindingLayout::TEX_GBUFFER_ALBEDO,
            ShaderBindingLayout::TEX_GBUFFER_NORMAL, ShaderBindingLayout::TEX_GBUFFER_EMISSIVE,
            ShaderBindingLayout::TEX_GBUFFER_VELOCITY
        };
        constexpr std::array<std::string_view, 4> names{
            "ReSTIRPTInitial", "ReSTIRPTTemporal", "ReSTIRPTSpatial", "ReSTIRPTResolve"
        };
        const std::array<u32, 4> sourcePools{ current, current, 2u, m_Settings.SpatialReuse ? 3u : 2u };
        const std::array<u32, 4> neighbourPools{ current, previous, 2u, current };
        const std::array<u32, 4> destinationPools{ current, 2u, 3u, 3u };
        const sizet textureUnitCount = textureUnits.size();
        auto& timers = GPUPassTimerPool::GetInstance();
        for (u32 stage = 0; stage < 4u; ++stage)
        {
            if (stage == 2u && !m_Settings.SpatialReuse)
                continue;
            params.Counts.y = stage;
            params.SourceAddresses = glm::uvec4(address(m_Pools[sourcePools[stage]]->GetDeviceAddress()),
                                                address(m_Pools[neighbourPools[stage]]->GetDeviceAddress()));
            params.DestinationAddresses = glm::uvec4(address(m_Pools[destinationPools[stage]]->GetDeviceAddress()),
                                                     address(m_Counters->GetDeviceAddress()));
            m_Parameters->Bind();
            m_Parameters->SetData(&params, sizeof(params));
            timers.BeginSubPass(std::string(names[stage]));
            targets[stage]->Bind();
            context.SetViewport(0, 0, m_Width, m_Height);
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            constexpr std::array<u32, 4> attachments{ 0u, 1u, 2u, 3u };
            RenderCommand::SetDrawBuffers(attachments);
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
            context.Clear();
            m_Shader->Bind();
            for (sizet i = 0; i < textureUnitCount; ++i)
                context.BindTextureOrHeapOffset(textureUnits[i], textures[i].IsValid() ? textures[i] : textures[0],
                                                RHI::HeapSlotLifetime::FrameTransient);
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_USER_1, textures[5], RHI::HeapSlotLifetime::Persistent);
            const auto triangle = MeshPrimitives::GetFullscreenTriangle();
            triangle->Bind();
            context.FlushHeapOffsets();
            RenderCommand::DrawIndexed(triangle);
            targets[stage]->Unbind();
            timers.EndSubPass();
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        }
        RenderCommand::SetDepthMask(true);
        m_HaveHistory = true;
        m_LastFrame = m_FrameIndex;
        m_LastEpoch = m_SceneEpoch;
        m_HaveCounters = true;
        m_CounterFrame = m_FrameIndex;
    }

    void ReSTIRPTPass::SetupFramebuffer(u32 width, u32 height)
    {
        ResizeFramebuffer(width, height);
    }
    void ReSTIRPTPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (m_Width == width && m_Height == height)
            return;
        m_Width = width;
        m_Height = height;
        m_HaveHistory = false;
        m_HaveCounters = false;
        m_Pools = {};
        m_Counters.Reset();
        // Refresh allocation and ownership before PopulateBlackboard tests Active.
        ResolveAvailabilityForFrame(m_DeferredPathActive, m_ParticipatingMedia);
    }
    void ReSTIRPTPass::OnReset()
    {
        m_HaveHistory = false;
        m_HaveCounters = false;
        m_Inputs = {};
        m_Targets = {};
        m_Stats = {};
    }
} // namespace OloEngine
