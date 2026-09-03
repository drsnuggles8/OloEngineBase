#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SphereProxyAORenderPass.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        // Pass-local heap slots, exactly like GTAORenderPass's: indices into the
        // shared heap-offset table this dispatch refills, NOT engine-wide TEX_*
        // reservations. Slot 0 is the storage image (a separate, rebased
        // namespace), so the samplers start at 1.
        constexpr u32 kSceneDepthSlot = 1u;
        constexpr u32 kSceneNormalsSlot = 2u;

        constexpr u32 kTileSize = 16u;
    } // namespace

    SphereProxyAORenderPass::SphereProxyAORenderPass()
    {
        SetName("SphereProxyAOPass");
        SetPassWorkType(PassWorkType::Compute);
        SetAsyncComputeCandidate(false);
    }

    void SphereProxyAORenderPass::SetProxySourceBounds(std::span<const BoundingBox> bounds)
    {
        m_SourceBounds.assign(bounds.begin(), bounds.end());
    }

    void SphereProxyAORenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SceneNormalsAreViewSpace = false;
        m_SelectedAOTexture = {};

        if (!IsEnabled())
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto depthRead =
                builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = blackboard.Scene.SceneNormals;
            m_SceneNormalsAreViewSpace = blackboard.Scene.SceneNormalsAreViewSpace;
            [[maybe_unused]] const auto normalsRead =
                builder.Read(blackboard.Scene.SceneNormals, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedAOTexture = blackboard.AO.AOBuffer;
            // Read-modify-write inside one dispatch: every invocation loads and
            // stores its OWN texel and no other, so the same-pass read/write is
            // a per-texel dependency the graph must be told about rather than a
            // hazard it has to schedule around.
            builder.AllowSamePassReadWrite(blackboard.AO.AOBuffer);
            builder.Write(blackboard.AO.AOBuffer, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto aoRead =
                builder.Read(blackboard.AO.AOBuffer, RGReadUsage::ShaderImage);
        }
    }

    void SphereProxyAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Width = spec.Width;
        m_Height = spec.Height;

        m_Shader = ComputeShader::Create("assets/shaders/compute/SphereProxyAO.comp");
        m_UBO = UniformBuffer::Create(UBOStructures::SphereProxyAOUBO::GetSize(),
                                      ShaderBindingLayout::UBO_SPHERE_PROXY_AO);

        OLO_CORE_INFO("SphereProxyAORenderPass: Initialized at {}x{}", m_Width, m_Height);
    }

    void SphereProxyAORenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
    }

    void SphereProxyAORenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
    }

    void SphereProxyAORenderPass::OnReset()
    {
        m_SourceBounds.clear();
        m_Proxies.clear();
        m_LastUploadedProxyCount = 0u;
    }

    void SphereProxyAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_LastUploadedProxyCount = 0u;

        if (!IsEnabled() || !IsReadyForExecution())
            return;

        RHI::ResourceHandle aoTexID{};
        RHI::ResourceHandle depthID{};
        RHI::ResourceHandle normalsID{};
        if (m_SelectedAOTexture.IsValid())
            aoTexID = context.ResolveTextureHandle(m_SelectedAOTexture);
        if (m_SelectedSceneDepthTexture.IsValid())
            depthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsID = context.ResolveTextureHandle(m_SelectedSceneNormalsTexture);

        // Every early return from here on leaves AOBuffer exactly as the AO
        // producer wrote it, which is the correct neutral: this pass only ever
        // MULTIPLIES into an already-valid buffer, so "did not run" and
        // "contributed nothing" are the same state. That is why there is no
        // PublishNoOcclusion equivalent here — unlike a producer, this pass
        // never owns the buffer's initial contents (issue #771's failure mode
        // does not reach it).
        if (!aoTexID.IsValid() || !depthID.IsValid() || !normalsID.IsValid())
            return;

        // Proxies, in view space. The world bounds can be kilometres out, so
        // both sides are shifted by the render origin before the f32 multiply.
        const glm::mat4 viewRelative = MakeViewRelative(m_ViewMatrix, m_RenderOrigin);
        // std::max BEFORE the cast: the setting is reachable through the
        // panel's CTRL+Click type-in, which does not clamp, and a negative i32
        // cast to u32 is a huge number that would select the FULL budget where
        // the user asked for none.
        const u32 maxProxies = std::min(static_cast<u32>(std::max(m_Settings.SphereProxyAOMaxProxies, 0)),
                                        SphereProxyAO::kMaxProxies);
        m_Proxies = SphereProxyAO::SelectProxies(m_SourceBounds, m_ViewPosition, maxProxies,
                                                 m_Settings.SphereProxyAOMaxRadius);
        if (m_Proxies.empty())
            return;

        const u32 proxyCount = std::min(static_cast<u32>(m_Proxies.size()), SphereProxyAO::kMaxProxies);
        for (u32 i = 0u; i < proxyCount; ++i)
        {
            const glm::vec3 relativeCentre = m_Proxies[i].Center - m_RenderOrigin;
            const glm::vec3 viewCentre = glm::vec3(viewRelative * glm::vec4(relativeCentre, 1.0f));
            m_GPUData.Proxies[i] = glm::vec4(viewCentre, m_Proxies[i].Radius);
        }

        UploadUniforms(proxyCount);
        m_LastUploadedProxyCount = proxyCount;

        auto& gpuSubTimers = GPUPassTimerPool::GetInstance();
        gpuSubTimers.BeginSubPass("SphereProxyAO");

        m_Shader->Bind();

        // FrameTransient for all three: every one comes from the graph's
        // transient pool, so a Persistent view would memoise an offset onto an
        // object the pool may reassign next frame (issue #691).
        HeapBinding::BindImageOrOffset(0, aoTexID, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(kSceneDepthSlot, depthID, RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(kSceneNormalsSlot, normalsID, RHI::HeapSlotLifetime::FrameTransient);

        const u32 groupsX = (m_Width + kTileSize - 1u) / kTileSize;
        const u32 groupsY = (m_Height + kTileSize - 1u) / kTileSize;
        HeapBinding::FlushOffsets();
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);

        // The AO buffer's consumers sample it (AOApplyPass, DeferredLightingPass),
        // so the image writes above need a texture-fetch barrier, not just an
        // image-access one.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess |
                                     MemoryBarrierFlags::TextureFetch |
                                     MemoryBarrierFlags::TextureUpdate);

        m_Shader->Unbind();
        gpuSubTimers.EndSubPass();
    }

    void SphereProxyAORenderPass::UploadUniforms(u32 proxyCount)
    {
        if (!m_UBO)
            return;

        const f32 projScale00 = m_Projection[0][0];
        const f32 projScale11 = m_Projection[1][1];

        // GL convention on both axes, matching GTAO.comp's unpack: the depth and
        // normals this pass fetches are the same GL-convention textures, and
        // negating the Y pair (the XeGTAO/D3D form) would mirror every
        // reconstructed view position about the horizontal plane.
        m_GPUData.NDCToViewMul = glm::vec2(2.0f / projScale00, 2.0f / projScale11);
        m_GPUData.NDCToViewAdd = glm::vec2(-1.0f / projScale00, -1.0f / projScale11);

        m_GPUData.ScreenWidth = static_cast<i32>(m_Width);
        m_GPUData.ScreenHeight = static_cast<i32>(m_Height);
        m_GPUData.ProxyCount = static_cast<i32>(proxyCount);
        m_GPUData.DebugView = m_Settings.SphereProxyAODebugView ? 1 : 0;

        m_GPUData.Strength = m_Settings.SphereProxyAOStrength;
        m_GPUData.DepthLinearizeA = m_Projection[2][2];
        m_GPUData.DepthLinearizeB = m_Projection[3][2];
        m_GPUData.InfluenceRadiusScale = m_Settings.SphereProxyAOInfluenceScale;

        // Identity when the bound normals are already view space; applying the
        // view rotation a second time would take every normal out of the
        // hemisphere the integral is written against (the forward-path bug GTAO
        // hit — see GTAORenderPass::UploadGTAOUniforms).
        m_GPUData.ViewMatrix = m_SceneNormalsAreViewSpace ? glm::mat4(1.0f) : m_ViewMatrix;

        m_UBO->SetData(&m_GPUData, UBOStructures::SphereProxyAOUBO::GetSize());
        m_UBO->Bind();
    }
} // namespace OloEngine
