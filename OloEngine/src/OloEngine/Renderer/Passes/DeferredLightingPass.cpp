#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
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

    void DeferredLightingPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        m_SelectedInputs = {};
        m_UseMSAAShading = false;

        if (!m_GBuffer)
            return;

        m_UseMSAAShading = m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1u && static_cast<bool>(m_ShaderMSAA);
        const bool useMSAAShading = m_UseMSAAShading;

        if (useMSAAShading)
        {
            m_SelectedInputs.GBufferAlbedo = blackboard.GBufferAlbedoMS;
            m_SelectedInputs.GBufferNormal = blackboard.GBufferNormalMS;
            m_SelectedInputs.GBufferEmissive = blackboard.GBufferEmissiveMS;
            m_SelectedInputs.SceneDepth = blackboard.SceneDepthMS;
            m_SelectedInputs.Velocity = blackboard.VelocityMS;
        }
        else
        {
            m_SelectedInputs.GBufferAlbedo = blackboard.GBufferAlbedo;
            m_SelectedInputs.GBufferNormal = blackboard.GBufferNormal;
            m_SelectedInputs.GBufferEmissive = blackboard.GBufferEmissive;
            m_SelectedInputs.SceneDepth = blackboard.SceneDepth;
            m_SelectedInputs.Velocity = blackboard.Velocity;
        }

        if (m_SelectedInputs.GBufferAlbedo.IsValid())
        {
            [[maybe_unused]] const auto gbufferAlbedoRead = builder.Read(m_SelectedInputs.GBufferAlbedo, RGReadUsage::ShaderSample);
        }
        if (m_SelectedInputs.GBufferNormal.IsValid())
        {
            [[maybe_unused]] const auto gbufferNormalRead = builder.Read(m_SelectedInputs.GBufferNormal, RGReadUsage::ShaderSample);
        }
        if (m_SelectedInputs.GBufferEmissive.IsValid())
        {
            [[maybe_unused]] const auto gbufferEmissiveRead = builder.Read(m_SelectedInputs.GBufferEmissive, RGReadUsage::ShaderSample);
        }
        if (m_SelectedInputs.SceneDepth.IsValid())
        {
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(m_SelectedInputs.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (m_SelectedInputs.Velocity.IsValid())
        {
            [[maybe_unused]] const auto velocityRead = builder.Read(m_SelectedInputs.Velocity, RGReadUsage::ShaderSample);
        }

        if (blackboard.ShadowMapCSM.IsValid())
        {
            m_SelectedInputs.ShadowMapCSM = blackboard.ShadowMapCSM;
            [[maybe_unused]] const auto shadowCSMRead = builder.Read(blackboard.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.ShadowMapSpot.IsValid())
        {
            m_SelectedInputs.ShadowMapSpot = blackboard.ShadowMapSpot;
            [[maybe_unused]] const auto shadowSpotRead = builder.Read(blackboard.ShadowMapSpot, RGReadUsage::ShaderSample);
        }
        for (size_t i = 0; i < blackboard.ShadowMapPoint.size() && i < m_SelectedInputs.ShadowMapPoint.size(); ++i)
        {
            const auto& pointHandle = blackboard.ShadowMapPoint[i];
            if (pointHandle.IsValid())
            {
                m_SelectedInputs.ShadowMapPoint[i] = pointHandle;
                [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
            }
        }
        if (blackboard.AOBuffer.IsValid())
        {
            m_SelectedInputs.AOBuffer = blackboard.AOBuffer;
            [[maybe_unused]] const auto aoRead = builder.Read(blackboard.AOBuffer, RGReadUsage::ShaderSample);
        }
        if (blackboard.IrradianceMap.IsValid())
        {
            m_SelectedInputs.IrradianceMap = blackboard.IrradianceMap;
            [[maybe_unused]] const auto irradianceRead = builder.Read(blackboard.IrradianceMap, RGReadUsage::ShaderSample);
        }
        if (blackboard.PrefilterMap.IsValid())
        {
            m_SelectedInputs.PrefilterMap = blackboard.PrefilterMap;
            [[maybe_unused]] const auto prefilterRead = builder.Read(blackboard.PrefilterMap, RGReadUsage::ShaderSample);
        }
        if (blackboard.BrdfLut.IsValid())
        {
            m_SelectedInputs.BrdfLut = blackboard.BrdfLut;
            [[maybe_unused]] const auto brdfRead = builder.Read(blackboard.BrdfLut, RGReadUsage::ShaderSample);
        }

        if (blackboard.SceneColor.IsValid())
        {
            SetPrimaryInputFramebufferHandle(blackboard.SceneColor);
            builder.Write(blackboard.SceneColor, RGWriteUsage::RenderTarget);
        }
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

    void DeferredLightingPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Resolve the setup-selected scene framebuffer instead of replaying
        // a blackboard lookup ladder at execute time.
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        // Only runs when registered in the graph, which `Renderer3D::
        // ConfigureRenderGraph` does solely for RenderingPath::Deferred.
        // The guards here are for genuine invalid states (shader load
        // failure, G-Buffer not yet lazily created, explicit debug-channel
        // override bypassing lighting) — not for path selection.
        if (!m_Shader || !m_GBuffer || !m_SceneFramebuffer || !m_ControlsUBO || m_DebugChannel != 0)
            return;

        // Silent guard: kept in scope (rather than removed) so future leak
        // hunts can flip the policy back to Log/Restore and rerun without
        // touching the pass. Policy::Ignore is correct here because every
        // critical state field the guard tracks is explicitly restored
        // below (FBO/draw-buffers/depth/blend/cull/polygon) and the pass
        // unbinds its shader + VAO at exit, so the only "mutations" the
        // guard would otherwise diff are the unbinds themselves (entry
        // VAO != 0). Per-slot texture / UBO bindings are intentionally
        // left as-is — downstream passes set their own.
        GLStateGuard guard("DeferredLightingPass", GLStateGuard::Policy::Ignore);

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
        const bool useMSAAShading = m_UseMSAAShading;
        Ref<Shader>& shader = useMSAAShading ? m_ShaderMSAA : m_Shader;
        shader->Bind();

        CommandDispatch::BindSceneResources();

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

        // G-Buffer samplers (slots 43-47). Setup already chose the canonical
        // handle family (MSAA vs resolved); Execute only resolves those chosen
        // handles. When a chosen handle resolves to 0 (headless / unit-test /
        // when not in deferred path), fall back to raw `m_GBuffer` accessors
        // that match the setup-selected family.
        //
        // Defensive: if any required attachment ID is zero we bail before
        // issuing the fullscreen draw. A zero ID means the G-Buffer was
        // constructed but a format/attachment was dropped (e.g. velocity RT
        // disabled in a non-TAA configuration). Binding 0 to a sampler
        // reads undefined data, which on NVIDIA surfaces as random black
        // pixels and on AMD as driver crashes.
        const auto resolveSelectedTexture = [&context](RGTextureHandle handle, u32 fallbackID) -> u32
        {
            u32 id = 0;
            if (handle.IsValid())
                id = context.ResolveTexture(handle);
            if (id == 0)
                id = fallbackID;
            return id;
        };

        const u32 albedoID = resolveSelectedTexture(m_SelectedInputs.GBufferAlbedo,
                                                    useMSAAShading ? m_GBuffer->GetMSColorAttachmentID(GBuffer::Albedo)
                                                                   : m_GBuffer->GetColorAttachmentID(GBuffer::Albedo));
        const u32 normalID = resolveSelectedTexture(m_SelectedInputs.GBufferNormal,
                                                    useMSAAShading ? m_GBuffer->GetMSColorAttachmentID(GBuffer::Normal)
                                                                   : m_GBuffer->GetColorAttachmentID(GBuffer::Normal));
        const u32 emissiveID = resolveSelectedTexture(m_SelectedInputs.GBufferEmissive,
                                                      useMSAAShading ? m_GBuffer->GetMSColorAttachmentID(GBuffer::Emissive)
                                                                     : m_GBuffer->GetColorAttachmentID(GBuffer::Emissive));
        const u32 velocityID = resolveSelectedTexture(m_SelectedInputs.Velocity,
                                                      useMSAAShading ? m_GBuffer->GetMSColorAttachmentID(GBuffer::Velocity)
                                                                     : m_GBuffer->GetColorAttachmentID(GBuffer::Velocity));
        const u32 depthID = resolveSelectedTexture(m_SelectedInputs.SceneDepth,
                                                   useMSAAShading ? m_GBuffer->GetMSDepthAttachmentID()
                                                                  : m_GBuffer->GetDepthAttachmentID());
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

        // IBL — resolve through the graph from the setup-stored handles.
        // The graph imports these textures (see RenderPipeline::PopulateBlackboard),
        // so the resolved ID is identical to Renderer3D::GetGlobal*MapID() but the
        // bind now goes through the graph's resolve path for consistency with the
        // rest of the pass and future barrier / transition / debug-capture
        // infrastructure. The shader branches on DeferredControls.iblAvailable.
        if (iblAvailable)
        {
            const u32 irradianceID = m_SelectedInputs.IrradianceMap.IsValid()
                                         ? context.ResolveTexture(m_SelectedInputs.IrradianceMap)
                                         : 0u;
            const u32 prefilterID = m_SelectedInputs.PrefilterMap.IsValid()
                                        ? context.ResolveTexture(m_SelectedInputs.PrefilterMap)
                                        : 0u;
            const u32 brdfLutID = m_SelectedInputs.BrdfLut.IsValid()
                                      ? context.ResolveTexture(m_SelectedInputs.BrdfLut)
                                      : 0u;
            context.BindTexture(ShaderBindingLayout::TEX_USER_0, irradianceID);
            context.BindTexture(ShaderBindingLayout::TEX_USER_1, prefilterID);
            context.BindTexture(ShaderBindingLayout::TEX_USER_2, brdfLutID);
        }

        // Shadow maps — resolve through the graph from setup-stored handles.
        // The Forward shader expects these in the same slots so binding 6 (shadow
        // matrices UBO) carries compatible data either path.
        const u32 csmShadowID = m_SelectedInputs.ShadowMapCSM.IsValid()
                                    ? context.ResolveTexture(m_SelectedInputs.ShadowMapCSM)
                                    : 0u;
        const u32 spotShadowID = m_SelectedInputs.ShadowMapSpot.IsValid()
                                     ? context.ResolveTexture(m_SelectedInputs.ShadowMapSpot)
                                     : 0u;
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW, csmShadowID);
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_SPOT, spotShadowID);
        static constexpr std::array<u32, 4u> pointShadowSlots = {
            ShaderBindingLayout::TEX_SHADOW_POINT_0,
            ShaderBindingLayout::TEX_SHADOW_POINT_1,
            ShaderBindingLayout::TEX_SHADOW_POINT_2,
            ShaderBindingLayout::TEX_SHADOW_POINT_3,
        };
        for (u32 i = 0u; i < pointShadowSlots.size(); ++i)
        {
            const u32 pointID = m_SelectedInputs.ShadowMapPoint[i].IsValid()
                                    ? context.ResolveTexture(m_SelectedInputs.ShadowMapPoint[i])
                                    : 0u;
            context.BindTexture(pointShadowSlots[i], pointID);
        }

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

        // Unbind the fullscreen-triangle VAO and the deferred-lighting shader
        // so downstream passes see a clean slate. The GLStateGuard would
        // otherwise restore both via ApplyCore() — explicit clears here keep
        // the safety net pristine so it surfaces only genuine regressions.
        ::glBindVertexArray(0);
        ::glUseProgram(0);
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
        m_SelectedInputs = {};
        m_UseMSAAShading = false;
    }
} // namespace OloEngine
