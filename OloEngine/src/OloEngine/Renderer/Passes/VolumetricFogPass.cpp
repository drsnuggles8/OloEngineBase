#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine
{
    VolumetricFogPass::VolumetricFogPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("VolumetricFogPass");
        // The integrated volume is consumed OUTSIDE the graph's resource
        // tracking (FogRenderPass binds it as a plain sampler3D), so the
        // graph's reachability cull must never drop this pass while enabled.
        SetSideEffects(SideEffect::NeverCull);
    }

    void VolumetricFogPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_ScatterShader = ComputeShader::Create("assets/shaders/compute/FroxelFogScatter.comp");
        m_IntegrateShader = ComputeShader::Create("assets/shaders/compute/FroxelFogIntegrate.comp");
        if (!m_ScatterShader || !m_ScatterShader->IsValid() ||
            !m_IntegrateShader || !m_IntegrateShader->IsValid())
        {
            OLO_CORE_ERROR("VolumetricFogPass: Failed to load froxel fog compute shaders");
        }

        Texture3DSpecification volumeSpec;
        volumeSpec.Width = kVolumeWidth;
        volumeSpec.Height = kVolumeHeight;
        volumeSpec.Depth = kVolumeDepth;
        volumeSpec.Format = Texture3DFormat::RGBA16F;
        m_ScatterVolume[0] = Texture3D::Create(volumeSpec);
        m_ScatterVolume[1] = Texture3D::Create(volumeSpec);
        m_IntegratedVolume = Texture3D::Create(volumeSpec);

        m_FroxelUBO = UniformBuffer::Create(
            UBOStructures::FroxelFogUBO::GetSize(),
            ShaderBindingLayout::UBO_FROXEL_FOG);

        m_HistoryValid = false;
        m_PrevViewProjectionValid = false;

        OLO_CORE_INFO("VolumetricFogPass: Initialized {}x{}x{} froxel fog volume",
                      kVolumeWidth, kVolumeHeight, kVolumeDepth);
    }

    void VolumetricFogPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        if (!m_Enabled)
            return;

        // The clustered light lists are dispatched inline inside
        // SceneRenderPass::Execute — order after it explicitly (the SSBOs are
        // not graph resources).
        builder.DependsOnPass("ScenePass");

        // Shadow inputs: sun visibility (CSM) + local-light visibility (atlas)
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            [[maybe_unused]] const auto csmRead = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            [[maybe_unused]] const auto atlasRead = builder.Read(blackboard.Shadows.ShadowMapAtlas, RGReadUsage::ShaderSample);
        }
    }

    void VolumetricFogPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        (void)context;

        m_RanThisFrame = false;

        if (!m_Enabled || !IsReadyForExecution() ||
            !m_ScatterVolume[0] || !m_ScatterVolume[1] || !m_IntegratedVolume)
        {
            return;
        }

        const auto& fog = Renderer3D::GetFogSettings();

        // Camera basis. Renderer3D::GetViewMatrix() is the WORLD view; every
        // GPU-side position this pass touches (cluster lights, shadow
        // matrices) is render-RELATIVE (issue #429), so build the relative
        // view for the froxel unproject and keep the absolute VP for the
        // temporal reprojection of absolute-world positions.
        const glm::mat4 viewWorld = Renderer3D::GetViewMatrix();
        const glm::mat4 projection = Renderer3D::GetProjectionMatrix();
        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();
        const glm::mat4 viewRelative = MakeViewRelative(viewWorld, renderOrigin);
        const glm::mat4 viewProjectionAbsolute = projection * viewWorld;

        f32 cameraNear = 0.1f;
        f32 cameraFar = 1000.0f;
        ClusteredLighting::ExtractClipPlanes(projection, cameraNear, cameraFar);

        // The fog volume spans [cameraNear, fogFar]: deep enough to carry the
        // atmosphere, shallow enough that 64 exponential slices stay dense.
        const f32 fogFar = std::min(std::clamp(fog.End, 20.0f, 500.0f), cameraFar);
        const f32 fogNear = std::max(cameraNear, ClusteredLighting::kMinNearPlane);

        // Upload the froxel UBO
        UBOStructures::FroxelFogUBO ubo{};
        ubo.InverseView = glm::inverse(viewRelative);
        ubo.InverseProjection = glm::inverse(projection);
        ubo.PrevViewProjection = m_PrevViewProjectionValid ? m_PrevViewProjection : viewProjectionAbsolute;
        ubo.Dims = glm::vec4(static_cast<f32>(kVolumeWidth), static_cast<f32>(kVolumeHeight),
                             static_cast<f32>(kVolumeDepth),
                             m_HistoryValid ? 0.9f : 0.0f);
        ubo.DepthParams = glm::vec4(fogNear, fogFar, std::log2(fogFar / fogNear),
                                    static_cast<f32>(m_FrameIndex));
        ubo.RenderOrigin = glm::vec4(renderOrigin, 1.0f);
        m_FroxelUBO->SetData(&ubo, sizeof(ubo));
        m_FroxelUBO->Bind();

        // The clustered light lists were unbound after the scene color pass —
        // re-bind them (and the enabled Forward+ UBO) for the scatter compute.
        // When clustering is inactive the last-uploaded Forward+ UBO is the
        // disabled baseline, so the shader's cluster loop self-gates.
        auto& forwardPlus = Renderer3D::GetForwardPlus();
        const bool clusteredActive = forwardPlus.ShouldUseForwardPlus();
        if (clusteredActive)
            forwardPlus.BindForShading();

        // Shadow maps (compute-local sampler units 0/1; placeholders keep the
        // declared samplers valid when no real map exists this frame)
        auto& shadowMap = Renderer3D::GetShadowMap();
        const u32 csmID = shadowMap.GetCSMRendererID() != 0
                              ? shadowMap.GetCSMRendererID()
                              : ShadowMap::GetCSMPlaceholderRendererID();
        const u32 atlasID = shadowMap.GetAtlasRendererID() != 0
                                ? shadowMap.GetAtlasRendererID()
                                : ShadowMap::GetAtlasPlaceholderRendererID();
        RenderCommand::BindTexture(0, csmID);
        RenderCommand::BindTexture(1, atlasID);

        const u32 historyIndex = 1u - m_CurrentScatter;
        RenderCommand::BindTexture(2, m_ScatterVolume[historyIndex]->GetRendererID());

        // --- Scatter (inject + light scattering + temporal) ---
        m_ScatterShader->Bind();
        RenderCommand::BindImageTexture(0, m_ScatterVolume[m_CurrentScatter]->GetRendererID(),
                                        0, true, 0, GL_WRITE_ONLY, GL_RGBA16F);
        RenderCommand::DispatchCompute((kVolumeWidth + 3) / 4, (kVolumeHeight + 3) / 4, (kVolumeDepth + 3) / 4);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        // --- Integrate (front-to-back accumulation per column) ---
        m_IntegrateShader->Bind();
        RenderCommand::BindTexture(0, m_ScatterVolume[m_CurrentScatter]->GetRendererID());
        RenderCommand::BindImageTexture(0, m_IntegratedVolume->GetRendererID(),
                                        0, true, 0, GL_WRITE_ONLY, GL_RGBA16F);
        RenderCommand::DispatchCompute((kVolumeWidth + 7) / 8, (kVolumeHeight + 7) / 8, 1);
        // The composite pass samples the integrated volume as a texture.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        m_IntegrateShader->Unbind();

        if (clusteredActive)
            forwardPlus.UnbindAfterShading();

        // Bookkeeping for the next frame
        m_CurrentScatter = historyIndex;
        m_HistoryValid = true;
        m_PrevViewProjection = viewProjectionAbsolute;
        m_PrevViewProjectionValid = true;
        ++m_FrameIndex;
        m_RanThisFrame = true;
    }

    void VolumetricFogPass::UploadDisabledUBO()
    {
        if (!m_FroxelUBO)
            return;

        UBOStructures::FroxelFogUBO ubo{};
        ubo.RenderOrigin = glm::vec4(0.0f); // w = 0 -> froxel path disabled
        m_FroxelUBO->SetData(&ubo, sizeof(ubo));
        m_FroxelUBO->Bind();

        // A disabled frame also invalidates the temporal history: the next
        // enabled frame must not reproject against a stale volume.
        m_HistoryValid = false;
        m_PrevViewProjectionValid = false;
    }

    void VolumetricFogPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void VolumetricFogPass::ResizeFramebuffer(u32 width, u32 height)
    {
        // The froxel volume is fixed-resolution (screen-decoupled); only the
        // spec bookkeeping follows the viewport.
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }
} // namespace OloEngine
