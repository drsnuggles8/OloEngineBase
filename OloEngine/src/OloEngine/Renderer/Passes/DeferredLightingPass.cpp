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
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

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
        if (const bool useMSAAShading = m_UseMSAAShading)
        {
            m_SelectedInputs.GBufferAlbedo = blackboard.GBuffer.GBufferAlbedoMS;
            m_SelectedInputs.GBufferNormal = blackboard.GBuffer.GBufferNormalMS;
            m_SelectedInputs.GBufferEmissive = blackboard.GBuffer.GBufferEmissiveMS;
            m_SelectedInputs.SceneDepth = blackboard.GBuffer.SceneDepthMS;
            m_SelectedInputs.Velocity = blackboard.GBuffer.VelocityMS;
        }
        else
        {
            m_SelectedInputs.GBufferAlbedo = blackboard.GBuffer.GBufferAlbedo;
            m_SelectedInputs.GBufferNormal = blackboard.GBuffer.GBufferNormal;
            m_SelectedInputs.GBufferEmissive = blackboard.GBuffer.GBufferEmissive;
            m_SelectedInputs.SceneDepth = blackboard.Scene.SceneDepth;
            m_SelectedInputs.Velocity = blackboard.GBuffer.Velocity;
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

        // Raw-depth views alias the array storage tracked via the handles below,
        // so they need no separate Read()/barrier — just carry the GL ids.
        m_SelectedInputs.ShadowMapCSMRawID = blackboard.Shadows.ShadowMapCSMRawID;
        m_SelectedInputs.ShadowMapAtlasRawID = blackboard.Shadows.ShadowMapAtlasRawID;
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            m_SelectedInputs.ShadowMapCSM = blackboard.Shadows.ShadowMapCSM;
            [[maybe_unused]] const auto shadowCSMRead = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            m_SelectedInputs.ShadowMapAtlas = blackboard.Shadows.ShadowMapAtlas;
            [[maybe_unused]] const auto shadowAtlasRead = builder.Read(blackboard.Shadows.ShadowMapAtlas, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedInputs.AOBuffer = blackboard.AO.AOBuffer;
            [[maybe_unused]] const auto aoRead = builder.Read(blackboard.AO.AOBuffer, RGReadUsage::ShaderSample);
        }
        if (blackboard.IBL.IrradianceMap.IsValid())
        {
            m_SelectedInputs.IrradianceMap = blackboard.IBL.IrradianceMap;
            [[maybe_unused]] const auto irradianceRead = builder.Read(blackboard.IBL.IrradianceMap, RGReadUsage::ShaderSample);
        }
        if (blackboard.IBL.PrefilterMap.IsValid())
        {
            m_SelectedInputs.PrefilterMap = blackboard.IBL.PrefilterMap;
            [[maybe_unused]] const auto prefilterRead = builder.Read(blackboard.IBL.PrefilterMap, RGReadUsage::ShaderSample);
        }
        if (blackboard.IBL.BrdfLut.IsValid())
        {
            m_SelectedInputs.BrdfLut = blackboard.IBL.BrdfLut;
            [[maybe_unused]] const auto brdfRead = builder.Read(blackboard.IBL.BrdfLut, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scene.SceneColor.IsValid())
        {
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
            builder.Write(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget);
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

        // Clustered Forward+ light lists (issue #435): BindSceneResources just
        // uploaded the disabled-UBO baseline, and ScenePass unbound the cluster
        // SSBOs after the G-Buffer pass — so without this re-bind the deferred
        // lighting shader's `fplusActive` gate reads 0 and every local light
        // falls back to the 256-cap MultiLight UBO loop. Re-bind so the
        // deferred lighting draw consumes the same per-cluster lists as the
        // forward paths (this was silently dead in the 2D-tile era).
        auto& forwardPlus = Renderer3D::GetForwardPlus();
        const bool clusteredActive = forwardPlus.ShouldUseForwardPlus();
        if (clusteredActive)
            forwardPlus.BindForShading();

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
        // Bind 1x1 placeholder shadow textures of the correct target type
        // when no real shadow map is available — the shader's
        // u_*ShadowEnabled flags still prevent sampling, but some drivers
        // validate the bound target against the sampler type at draw time.
        const u32 csmShadowID = m_SelectedInputs.ShadowMapCSM.IsValid()
                                    ? context.ResolveTexture(m_SelectedInputs.ShadowMapCSM)
                                    : ShadowMap::GetCSMPlaceholderRendererID();
        const u32 atlasShadowID = m_SelectedInputs.ShadowMapAtlas.IsValid()
                                      ? context.ResolveTexture(m_SelectedInputs.ShadowMapAtlas)
                                      : ShadowMap::GetAtlasPlaceholderRendererID();
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW, csmShadowID);
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_ATLAS, atlasShadowID);
        // Comparison-OFF raw-depth views for the PCSS blocker search (plain
        // sampler2DArray). Fall back to the raw placeholder so the declared
        // sampler always has a valid same-type binding.
        const u32 csmRawID = (m_SelectedInputs.ShadowMapCSMRawID != 0)
                                 ? m_SelectedInputs.ShadowMapCSMRawID
                                 : ShadowMap::GetCSMRawPlaceholderRendererID();
        const u32 atlasRawID = (m_SelectedInputs.ShadowMapAtlasRawID != 0)
                                   ? m_SelectedInputs.ShadowMapAtlasRawID
                                   : ShadowMap::GetAtlasRawPlaceholderRendererID();
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_CSM_RAW, csmRawID);
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW_ATLAS_RAW, atlasRawID);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        if (clusteredActive)
            forwardPlus.UnbindAfterShading();

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
        if (auto const& samplingFB = m_GBuffer->GetSamplingFramebuffer())
        {
            const u32 samplingFBID = samplingFB->GetRendererID();
            glBlitNamedFramebuffer(
                samplingFBID, sceneFBID,
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                GL_DEPTH_BUFFER_BIT, GL_NEAREST);

            // Copy the G-Buffer's per-pixel entity-ID attachment (RT4) into
            // the scene FB's entity-ID attachment (RT1). The forward path
            // writes entity IDs directly into Scene FB RT1 during ScenePass;
            // the deferred path's PBR shaders write into the G-Buffer
            // instead, so without this blit SelectionOutline (and the
            // viewport's pixel-picking readback) sees only the cleared
            // sentinel (-1) and selection never seeds.
            //
            // glBlitFramebuffer copies "the currently-selected colour
            // attachment", which `glNamedFramebufferReadBuffer` /
            // `glNamedFramebufferDrawBuffer` redirect. Integer attachments
            // require GL_NEAREST (per the GL 4.6 spec); MSAA → single-
            // sample resolution takes sample 0, which is correct for
            // discrete entity IDs.
            glNamedFramebufferReadBuffer(samplingFBID, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(std::to_underlying(GBuffer::EntityID)));
            glNamedFramebufferDrawBuffer(sceneFBID, GL_COLOR_ATTACHMENT1);
            glBlitNamedFramebuffer(
                samplingFBID, sceneFBID,
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                GL_COLOR_BUFFER_BIT, GL_NEAREST);

            // Restore the scene FB's draw-buffer set so the following
            // ForwardOverlayPass write-RT0-RT2 path still sees its intended
            // targets (it sets its own draw buffers but expects all
            // attachments to be available on bind).
            if (sceneColorAttachmentCount > 0)
            {
                std::array<GLenum, 16> fullDrawBufs{};
                const u32 n = std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(fullDrawBufs.size()));
                for (u32 i = 0; i < n; ++i)
                    fullDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
                glNamedFramebufferDrawBuffers(sceneFBID, static_cast<GLsizei>(n), fullDrawBufs.data());
            }
        }

        // Virtualized-geometry debug view (issue #629). Composited HERE, after the lighting
        // draw, because anything drawn before it is overwritten by it.
        BlitVirtualGeometryDebugOverlay();

        m_SceneFramebuffer->Unbind();

        // Unbind the fullscreen-triangle VAO and the deferred-lighting shader
        // so downstream passes see a clean slate. The GLStateGuard would
        // otherwise restore both via ApplyCore() — explicit clears here keep
        // the safety net pristine so it surfaces only genuine regressions.
        ::glBindVertexArray(0);
        ::glUseProgram(0);
    }

    void DeferredLightingPass::BlitVirtualGeometryDebugOverlay()
    {
        OLO_PROFILE_FUNCTION();

        const auto& settings = Renderer3D::GetRendererSettings();
        if (!settings.VirtualDebugToViewport || !m_SceneFramebuffer)
        {
            return;
        }

        auto& registry = VirtualMeshRegistry::Get();
        if (registry.GetDebugMode() == VirtualDebugMode::Off)
        {
            return;
        }

        const u32 debugTexID = registry.GetDebugColorTextureID();
        if (debugTexID == 0)
        {
            return;
        }

        // The debug image is sized to the G-Buffer. If the viewport resized this frame and the
        // debug target has not caught up yet, skip rather than sample a mismatched texture —
        // the overlay is a diagnostic, and a diagnostic that lies for one frame is worse than
        // one that is briefly absent. texelFetch (not a sampler) means the dimensions must
        // actually agree, not merely be filterable.
        if (m_GBuffer && (registry.GetDebugWidth() != m_GBuffer->GetWidth() ||
                          registry.GetDebugHeight() != m_GBuffer->GetHeight()))
        {
            return;
        }

        if (!m_VirtualDebugOverlay)
        {
            m_VirtualDebugOverlay = Shader::Create("assets/shaders/VirtualDebugOverlay.glsl");
        }
        if (!m_VirtualDebugOverlay)
        {
            return;
        }

        GLStateGuard guard("DeferredLightingPass::VirtualDebugOverlay", GLStateGuard::Policy::Restore);

        // Scene colour only (RT0). The scene FB also carries entity-id / normals attachments;
        // writing the overlay into those would corrupt mouse picking with cluster-hash colours.
        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();
        const GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
        glNamedFramebufferDrawBuffers(sceneFBID, 1, drawBufs);

        RenderCommand::SetViewport(0, 0, m_SceneFramebuffer->GetSpecification().Width,
                                   m_SceneFramebuffer->GetSpecification().Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(false); // the shader discards; it does not blend

        m_VirtualDebugOverlay->Bind();
        // Slot 0 is the engine's documented pass-local fullscreen-input slot (see
        // ShaderBindingLayout::IsKnownTextureBinding) — no material is bound during this draw.
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_DIFFUSE, debugTexID);

        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        // Restore the scene FB's full draw-buffer set — ForwardOverlayPass runs next and
        // expects every attachment to be available on bind (same contract as the lighting
        // draw's own restore above).
        const auto& spec = m_SceneFramebuffer->GetSpecification();
        u32 colorCount = 0;
        for (const auto& att : spec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++colorCount;
        }
        if (colorCount > 0)
        {
            std::array<GLenum, 16> fullDrawBufs{};
            const u32 n = std::min<u32>(colorCount, static_cast<u32>(fullDrawBufs.size()));
            for (u32 i = 0; i < n; ++i)
                fullDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
            glNamedFramebufferDrawBuffers(sceneFBID, static_cast<GLsizei>(n), fullDrawBufs.data());
        }

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
