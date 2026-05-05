#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glad/gl.h>

#include <array>

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

        // Phase F slice 33 — declare G-Buffer reads so the hazard validator
        // derives the ordering edges for the deferred chain:
        //   ScenePass→DeferredOpaqueDecalPass (SceneDepth RAW already works)
        //   DeferredOpaqueDecalPass→DeferredLightingPass  via DeclareRead(SceneColor)
        //   ScenePass→DeferredLightingPass (fallback)     via DeclareRead(SceneDepth)
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::SceneNormals, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("DeferredLightingPass: Initialized ({}x{}); writes into scene FB color[0]",
                      spec.Width, spec.Height);
    }

    void DeferredLightingPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void DeferredLightingPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 41 — self-resolving scene color framebuffer from the
        // active render graph blackboard.
        if (const auto* board = context.GetBlackboard())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(board->SceneColor))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        // Only runs when registered in the graph, which `Renderer3D::
        // ConfigureRenderGraph` does solely for RenderingPath::Deferred.
        // The guards here are for genuine invalid states (shader load
        // failure, G-Buffer not yet lazily created, explicit debug-channel
        // override bypassing lighting) — not for path selection.
        if (!m_Shader || !m_GBuffer || !m_SceneFramebuffer || !m_ControlsUBO || m_DebugChannel != 0)
            return;

        // Restoring guard: captures core GL state on entry (FBO / program /
        // depth / blend / stencil / cull / polygon / viewport / scissor)
        // and rolls it back in the destructor. Explicit restore calls
        // below remain for clarity and to keep invariants close to the
        // mutations, but the guard now also serves as a safety net for
        // any intermediate state this function forgets to revert. Per-
        // slot texture / UBO bindings are out of scope for ApplyCore.
        GLStateGuard guard("DeferredLightingPass", GLStateGuard::Policy::Restore);

        m_SceneFramebuffer->Bind();

        const u32 w = m_GBuffer->GetWidth();
        const u32 h = m_GBuffer->GetHeight();
        context.SetViewport(0, 0, w, h);

        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();

        // Count color attachments on the scene FB from its specification so
        // the full-layout restore below is exact regardless of the configured
        // attachment set (TAA velocity RT3 may or may not be present).
        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        u32 sceneColorAttachmentCount = 0;
        for (const auto& att : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++sceneColorAttachmentCount;
        }

        const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(sceneFBID, 1, &drawBuf);

        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        RenderCommand::SetCullFace(GL_BACK);

        const u32 sampleCount = m_GBuffer->GetSampleCount();
        const bool useMSAAShading = m_PerSampleLighting && sampleCount > 1u && m_ShaderMSAA;
        Ref<Shader>& shader = useMSAAShading ? m_ShaderMSAA : m_Shader;
        shader->Bind();

        Renderer3D::BindSceneUBOs();

        // Upload per-frame controls — IBL enable + intensity + cascade-debug
        // flag. Light-probe toggle mirrors RendererSettings::Deferred
        // .EnableLightProbes: Scene::OnUpdateRender always uploads the
        // LightProbeVolume UBO + SH SSBO every frame (a "disabled" UBO is
        // uploaded when no active volume exists), so the shader can safely
        // sample whenever this flag is on. When off, the shader falls back
        // to the global IBL cubemap.
        DeferredControlsData controls{};
        const bool iblAvailable = Renderer3D::GetGlobalIrradianceMapID() != 0 && Renderer3D::GetGlobalPrefilterMapID() != 0 && Renderer3D::GetGlobalBRDFLutMapID() != 0;
        controls.Controls.x = iblAvailable ? 1.0f : 0.0f;
        controls.Controls.y = Renderer3D::GetRendererSettings().Deferred.EnableLightProbes ? 1.0f : 0.0f;
        // Runtime IBL strength multiplier: plumb the global scalar set via
        // Renderer3D::SetGlobalIBL() so the ImGui slider actually reaches
        // DeferredLighting.glsl. Before, this was pinned at 1.0 and IBL
        // intensity tweaks only applied to forward PBR draws.
        controls.Controls.z = Renderer3D::GetGlobalIBLIntensity();
        // Cascade-debug visualization flag mirrors ShadowMap::SetCascadeDebugEnabled,
        // which is the single source of truth across forward and deferred paths.
        controls.Controls.w = Renderer3D::GetShadowMap().IsCascadeDebugEnabled() ? 1.0f : 0.0f;
        controls.MSAAParams.x = static_cast<f32>(useMSAAShading ? sampleCount : 1u);
        controls.MSAAParams.y = 0.0f;
        controls.MSAAParams.z = 0.0f;
        controls.MSAAParams.w = 0.0f;
        m_ControlsUBO->SetData(&controls, sizeof(controls));
        m_ControlsUBO->Bind();

        // G-Buffer samplers (slots 43-47). In per-sample shading mode we
        // bind the raw multisample attachments; otherwise bind the resolved
        // single-sample copies produced by GBuffer::Resolve().
        //
        // Phase F slice 41 — self-resolving G-Buffer texture handles from the
        // render-graph blackboard. When a handle resolves to 0 (headless /
        // unit-test / when not in deferred path), fall back to raw `m_GBuffer`
        // accessors. MSAA companion handles are only available when the
        // G-Buffer's sample count > 1.
        //
        // Defensive: if any required attachment ID is zero we bail before
        // issuing the fullscreen draw. A zero ID means the G-Buffer was
        // constructed but a format/attachment was dropped (e.g. velocity RT
        // disabled in a non-TAA configuration). Binding 0 to a sampler
        // reads undefined data, which on NVIDIA surfaces as random black
        // pixels and on AMD as driver crashes.
        const u32 albedoID = useMSAAShading
                                 ? [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferAlbedoMS);
            if (id == 0)
                id = m_GBuffer->GetMSColorAttachmentID(GBuffer::Albedo);
            return id;
        }()
                                 : [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferAlbedo);
            if (id == 0)
                id = m_GBuffer->GetColorAttachmentID(GBuffer::Albedo);
            return id;
        }();
        const u32 normalID = useMSAAShading
                                 ? [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferNormalMS);
            if (id == 0)
                id = m_GBuffer->GetMSColorAttachmentID(GBuffer::Normal);
            return id;
        }()
                                 : [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferNormal);
            if (id == 0)
                id = m_GBuffer->GetColorAttachmentID(GBuffer::Normal);
            return id;
        }();
        const u32 emissiveID = useMSAAShading
                                   ? [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferEmissiveMS);
            if (id == 0)
                id = m_GBuffer->GetMSColorAttachmentID(GBuffer::Emissive);
            return id;
        }()
                                   : [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->GBufferEmissive);
            if (id == 0)
                id = m_GBuffer->GetColorAttachmentID(GBuffer::Emissive);
            return id;
        }();
        const u32 velocityID = useMSAAShading
                                   ? [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->VelocityMS);
            if (id == 0)
                id = m_GBuffer->GetMSColorAttachmentID(GBuffer::Velocity);
            return id;
        }()
                                   : m_GBuffer->GetColorAttachmentID(GBuffer::Velocity);
        const u32 depthID = useMSAAShading
                                ? [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->SceneDepthMS);
            if (id == 0)
                id = m_GBuffer->GetMSDepthAttachmentID();
            return id;
        }()
                                : [&]
        {
            u32 id = 0;
            if (const auto* board = context.GetBlackboard())
                id = context.ResolveTexture(board->SceneDepth);
            if (id == 0)
                id = m_GBuffer->GetDepthAttachmentID();
            return id;
        }();
        if (albedoID == 0 || normalID == 0 || emissiveID == 0 || depthID == 0)
        {
            OLO_CORE_ERROR("DeferredLightingPass: required G-Buffer attachment missing (albedo={}, normal={}, emissive={}, depth={}) - aborting lighting",
                           albedoID, normalID, emissiveID, depthID);
            return;
        }
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_ALBEDO, albedoID);
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_NORMAL, normalID);
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_EMISSIVE, emissiveID);
        // Velocity RT is optional (skipped outside TAA-enabled configs);
        // the fragment shader already handles a zero bind as "no motion".
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_VELOCITY, velocityID);
        context.BindTexture(ShaderBindingLayout::TEX_GBUFFER_DEPTH, depthID);

        // IBL (safe to rebind regardless — shader branches on DeferredControls).
        if (iblAvailable)
        {
            context.BindTexture(ShaderBindingLayout::TEX_USER_0,
                                Renderer3D::GetGlobalIrradianceMapID());
            context.BindTexture(ShaderBindingLayout::TEX_USER_1,
                                Renderer3D::GetGlobalPrefilterMapID());
            context.BindTexture(ShaderBindingLayout::TEX_USER_2,
                                Renderer3D::GetGlobalBRDFLutMapID());
        }

        // Shadow maps — reuse the same slots the Forward shader expects so
        // shadow UBO binding 6 carries compatible matrices.
        auto& shadow = Renderer3D::GetShadowMap();
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW, shadow.GetCSMRendererID());
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_SPOT, shadow.GetSpotRendererID());
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_0, shadow.GetPointRendererID(0));
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_1, shadow.GetPointRendererID(1));
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_2, shadow.GetPointRendererID(2));
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_POINT_3, shadow.GetPointRendererID(3));

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        // Restore the full multi-attachment draw-buffer list so later passes
        // writing into RT1 (normal) / RT2 (emissive) / RT3 (velocity) target
        // the correct attachments. Build the list dynamically from the scene
        // FB spec so removing e.g. the velocity RT doesn't leave dangling
        // GL_COLOR_ATTACHMENT3 entries.
        if (sceneColorAttachmentCount > 0)
        {
            std::array<GLenum, 16> fullDrawBufs{};
            const u32 n = std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(fullDrawBufs.size()));
            for (u32 i = 0; i < n; ++i)
                fullDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
            glNamedFramebufferDrawBuffers(sceneFBID, static_cast<GLsizei>(n), fullDrawBufs.data());
        }

        context.SetDepthTest(true);
        context.SetDepthMask(true);

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
