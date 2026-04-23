#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        // Must match the `DeferredLightingControls` block layout in
        // DeferredLighting.glsl / DeferredLighting_MSAA.glsl (two vec4s, 32
        // bytes std140).
        struct DeferredControlsData
        {
            glm::vec4 Controls;   // x=EnableIBL, y=EnableProbes, z=IBLIntensity, w=CascadeDebug
            glm::vec4 MSAAParams; // x=SampleCount (float), yzw reserved
        };
    } // namespace

    DeferredLightingPass::DeferredLightingPass()
    {
        SetName("DeferredLightingPass");
    }

    void DeferredLightingPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Shader = Shader::Create("assets/shaders/DeferredLighting.glsl");
        m_ShaderMSAA = Shader::Create("assets/shaders/DeferredLighting_MSAA.glsl");
        m_ControlsUBO = UniformBuffer::Create(sizeof(DeferredControlsData),
                                              ShaderBindingLayout::UBO_DEFERRED_LIGHTING);

        OLO_CORE_INFO("DeferredLightingPass: Initialized ({}x{}); writes into scene FB color[0]",
                      spec.Width, spec.Height);
    }

    void DeferredLightingPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Only runs when registered in the graph, which `Renderer3D::
        // ConfigureRenderGraph` does solely for RenderingPath::Deferred.
        // The guards here are for genuine invalid states (shader load
        // failure, G-Buffer not yet lazily created, explicit debug-channel
        // override bypassing lighting) — not for path selection.
        if (!m_Shader || !m_GBuffer || !m_SceneFramebuffer || m_DebugChannel != 0)
            return;

        m_SceneFramebuffer->Bind();

        const u32 w = m_GBuffer->GetWidth();
        const u32 h = m_GBuffer->GetHeight();
        RenderCommand::SetViewport(0, 0, w, h);

        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();
        const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(sceneFBID, 1, &drawBuf);

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetCullFace(GL_BACK);

        const u32 sampleCount = m_GBuffer ? m_GBuffer->GetSampleCount() : 1u;
        const bool useMSAAShading = m_PerSampleLighting && sampleCount > 1u && m_ShaderMSAA;
        Ref<Shader>& shader = useMSAAShading ? m_ShaderMSAA : m_Shader;
        shader->Bind();

        Renderer3D::BindSceneUBOs();

        // Upload per-frame controls — IBL enable + intensity + cascade-debug
        // flag. Light-probe toggle is driven from RendererSettings once the
        // deferred panel exposes it; default it on if the probe grid is valid.
        DeferredControlsData controls{};
        const bool iblAvailable = Renderer3D::GetGlobalIrradianceMapID() != 0 && Renderer3D::GetGlobalPrefilterMapID() != 0 && Renderer3D::GetGlobalBRDFLutMapID() != 0;
        controls.Controls.x = iblAvailable ? 1.0f : 0.0f;
        controls.Controls.y = 0.0f; // light probes — disabled until grid bind is in place
        controls.Controls.z = 1.0f;
        controls.Controls.w = 0.0f;
        controls.MSAAParams.x = static_cast<f32>(useMSAAShading ? sampleCount : 1u);
        controls.MSAAParams.y = 0.0f;
        controls.MSAAParams.z = 0.0f;
        controls.MSAAParams.w = 0.0f;
        m_ControlsUBO->SetData(&controls, sizeof(controls));
        m_ControlsUBO->Bind();

        // G-Buffer samplers (slots 43-47). In per-sample shading mode we
        // bind the raw multisample attachments; otherwise bind the resolved
        // single-sample copies produced by GBuffer::Resolve().
        if (useMSAAShading)
        {
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_ALBEDO,
                                       m_GBuffer->GetMSColorAttachmentID(GBuffer::Albedo));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_NORMAL,
                                       m_GBuffer->GetMSColorAttachmentID(GBuffer::Normal));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_EMISSIVE,
                                       m_GBuffer->GetMSColorAttachmentID(GBuffer::Emissive));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                       m_GBuffer->GetMSColorAttachmentID(GBuffer::Velocity));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_DEPTH,
                                       m_GBuffer->GetMSDepthAttachmentID());
        }
        else
        {
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_ALBEDO,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Albedo));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_NORMAL,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Normal));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_EMISSIVE,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Emissive));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_VELOCITY,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Velocity));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_DEPTH,
                                       m_GBuffer->GetDepthAttachmentID());
        }

        // IBL (safe to rebind regardless — shader branches on DeferredControls).
        if (iblAvailable)
        {
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_USER_0,
                                       Renderer3D::GetGlobalIrradianceMapID());
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_USER_1,
                                       Renderer3D::GetGlobalPrefilterMapID());
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_USER_2,
                                       Renderer3D::GetGlobalBRDFLutMapID());
        }

        // Shadow maps — reuse the same slots the Forward shader expects so
        // shadow UBO binding 6 carries compatible matrices.
        auto& shadow = Renderer3D::GetShadowMap();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW, shadow.GetCSMRendererID());
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_SPOT, shadow.GetSpotRendererID());
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_0, shadow.GetPointRendererID(0));
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_1, shadow.GetPointRendererID(1));
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_2, shadow.GetPointRendererID(2));
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_3, shadow.GetPointRendererID(3));

        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        const GLenum fullDrawBufs[] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2
        };
        glNamedFramebufferDrawBuffers(sceneFBID, 3, fullDrawBufs);

        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);

        // Copy G-Buffer depth into the scene framebuffer so every downstream
        // pass (ForwardOverlayPass, FoliagePass, WaterPass, SSAO, GTAO,
        // ParticlePass for soft-particle fade, PostProcess fog/DoF/MB) sees
        // the same depth values deferred geometry wrote. Without this the
        // scene FB depth attachment remains at whatever clear value it had
        // and downstream depth tests/samples are meaningless in Deferred.
        auto const& samplingFB = m_GBuffer->GetSamplingFramebuffer();
        if (samplingFB)
        {
            glBlitNamedFramebuffer(
                samplingFB->GetRendererID(), sceneFBID,
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }

        m_SceneFramebuffer->Unbind();
    }

    Ref<Framebuffer> DeferredLightingPass::GetTarget() const
    {
        // The pass writes into the externally-owned scene framebuffer; no
        // target of its own to expose.
        return m_SceneFramebuffer;
    }

    void DeferredLightingPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void DeferredLightingPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void DeferredLightingPass::OnReset()
    {
    }
} // namespace OloEngine
