#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    SceneRenderPass::SceneRenderPass()
    {
        SetName("SceneRenderPass");
        OLO_CORE_INFO("Creating SceneRenderPass.");
    }

    void SceneRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneDepthExport = {};
        m_SelectedSceneNormalsExport = {};
        m_SelectedVelocityExport = {};

        if (board.Scene.SceneColor.IsValid())
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);

        builder.DependsOnPass("ShadowPass");

        if (board.Shadows.ShadowMapCSM.IsValid())
        {
            [[maybe_unused]] const auto shadowCSMRead = builder.Read(board.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (board.Shadows.ShadowMapSpot.IsValid())
        {
            [[maybe_unused]] const auto shadowSpotRead = builder.Read(board.Shadows.ShadowMapSpot, RGReadUsage::ShaderSample);
        }

        for (const auto& pointHandle : board.Shadows.ShadowMapPoint)
        {
            if (pointHandle.IsValid())
            {
                [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
            }
        }

        if (board.IBL.IrradianceMap.IsValid())
        {
            [[maybe_unused]] const auto irradianceRead = builder.Read(board.IBL.IrradianceMap, RGReadUsage::ShaderSample);
        }
        if (board.IBL.PrefilterMap.IsValid())
        {
            [[maybe_unused]] const auto prefilterRead = builder.Read(board.IBL.PrefilterMap, RGReadUsage::ShaderSample);
        }
        if (board.IBL.BrdfLut.IsValid())
        {
            [[maybe_unused]] const auto brdfRead = builder.Read(board.IBL.BrdfLut, RGReadUsage::ShaderSample);
        }

        if (board.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthExport = board.Scene.SceneDepth;
            builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
        }
        if (board.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityExport = board.GBuffer.Velocity;
            builder.Write(board.GBuffer.Velocity, RGWriteUsage::TransferDest);
        }

        const auto& rendererSettings = Renderer3D::GetRendererSettings();
        if (rendererSettings.Path != RenderingPath::Deferred)
        {
            if (board.Scene.SceneNormals.IsValid())
            {
                m_SelectedSceneNormalsExport = board.Scene.SceneNormals;
                builder.Write(board.Scene.SceneNormals, RGWriteUsage::TransferDest);
            }
            if (board.Scene.SceneColor.IsValid())
                builder.Write(board.Scene.SceneColor, RGWriteUsage::RenderTarget);
        }
    }

    void SceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Ensure the specification includes color and depth attachments
        if (m_FramebufferSpec.Attachments.Attachments.empty())
        {
            OLO_CORE_WARN("SceneRenderPass::Init: No attachments specified, adding default color and depth attachments");
            m_FramebufferSpec.Attachments = {
                FramebufferTextureFormat::RGBA8, // Color buffer
                FramebufferTextureFormat::Depth  // Depth attachment
            };
        }

        m_Target = Framebuffer::Create(m_FramebufferSpec);

        // Lazy-loaded — only materialised when the user actually selects the
        // RMA debug channel for the first time (keeps Forward startup cheap).
        m_DebugRMAShader = nullptr;

        OLO_CORE_INFO("SceneRenderPass: Created framebuffer with dimensions {}x{}",
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SceneRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_Target = resolvedSceneFB;
        }

        if (!m_Target)
        {
            OLO_CORE_ERROR("SceneRenderPass::Execute: No target framebuffer!");
            return;
        }

        // Deferred path: bind a 4-RT G-Buffer instead of the forward scene FB.
        // The G-Buffer is lazily created here so Forward / Forward+ paths pay
        // zero memory cost if Deferred is never enabled.
        auto const& rendererSettings = Renderer3D::GetRendererSettings();
        const bool deferredActive = (rendererSettings.Path == RenderingPath::Deferred);
        if (deferredActive)
        {
            PrepareDeferredResources(rendererSettings.Deferred.MSAASampleCount);
        }

        Ref<Framebuffer> renderFB = deferredActive && m_GBuffer
                                        ? m_GBuffer->GetFramebuffer()
                                        : m_Target;

        // Even in deferred mode the *scene framebuffer* must be cleared
        // every frame: downstream passes (WaterPass → SSR, OITResolve,
        // ForwardOverlay) read attachments 1 (entityID) and 2 (view-space
        // normals). In deferred we only ever write attachment 0 (via
        // DeferredLightingPass) and blit depth, so without this clear the
        // non-color attachments carry stale data from the previous forward
        // session — which is exactly what breaks water SSR after a runtime
        // `Forward` → `Deferred` switch.
        if (deferredActive)
        {
            m_Target->Bind();
            m_Target->ClearAllAttachments({ 0.1f, 0.1f, 0.1f, 1.0f }, -1);
            m_Target->Unbind();
        }

        renderFB->Bind();

        // Clear all attachments properly (handles mixed integer/float attachments)
        // This clears color attachments with the specified color, entity ID with -1, and depth/stencil.
        // In Deferred mode the entityID slot isn't present — ClearAllAttachments
        // iterates the attachment list so it is safe either way.
        renderFB->ClearAllAttachments({ 0.1f, 0.1f, 0.1f, 1.0f }, -1);

        // Velocity RT (scene FB attachment 3 in Forward / Forward+) must clear
        // to zero so non-PBR forward shaders that don't emit location=3 leave
        // sky / terrain / water / particle pixels at "no motion". The generic
        // ClearAllAttachments path uses the same colour for every float RT,
        // which would write (0.1, 0.1) and produce bogus TAA reprojection at
        // uncovered pixels. Also run in Deferred mode: the ForwardOverlayPass
        // writes into this same scene-FB RT3 for skybox / terrain / infinite-
        // grid overlays and TAA samples it there, so leaving it at 0.1 from
        // ClearAllAttachments produces the same bogus motion at overlay pixels.
        if (m_Target)
        {
            const auto& attachments = m_Target->GetSpecification().Attachments.Attachments;
            if (attachments.size() > 3 && attachments[3].TextureFormat == FramebufferTextureFormat::RG16F)
                m_Target->ClearAttachment(3, glm::vec4(0.0f));
        }

        // Reset to default OpenGL state to ensure consistent rendering
        auto& rendererAPI = RenderCommand::GetRendererAPI();
        rendererAPI.SetDepthTest(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Capture hooks — minimal overhead when not capturing (helped by branch prediction)
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();

        if (capturing)
            captureManager.OnPreSort(m_CommandBucket);

        // BatchCommands uses hash-table grouping (O(n)) which doesn't require
        // pre-sorted input, and sorts internally afterward. Skipping the separate
        // SortCommands() call avoids a redundant sort pass.
        // When batching is disabled, fall back to sort-only.
        if (m_CommandBucket.GetCommandCount() > 0)
        {
            m_CommandBucket.BatchCommands(*m_Allocator);

            // If batching was disabled or no-op, ensure we're still sorted
            if (!m_CommandBucket.IsSorted())
                m_CommandBucket.SortCommands();
        }

        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);

        // Invoke post-batch capture step when capturing is active
        if (capturing)
            captureManager.OnPostBatch(m_CommandBucket);

        // Re-bind shared scene resources that earlier passes (e.g. ShadowPass)
        // may have overwritten at the same binding points.
        CommandDispatch::BindSceneResources();

        // Depth prepass: render all geometry depth-only first, then re-execute
        // with GL_EQUAL and no depth writes for the color pass. This eliminates
        // overdraw from fragment shading of occluded pixels.
        const bool depthPrepass = Renderer3D::IsDepthPrepassEnabled();
        if (depthPrepass)
        {
            // Pass 1: depth only — CommandDispatch overrides per-command state
            CommandDispatch::SetDepthPrepassActive(true);
            m_CommandBucket.Execute(rendererAPI);
            CommandDispatch::SetDepthPrepassActive(false);
        }

        // Flush deferred occlusion query proxy draws. When a depth prepass ran,
        // the depth buffer is fully populated; otherwise the first Execute below
        // will populate it and queries will rely on the previous frame's depth.
        if (Renderer3D::IsOcclusionCullingEnabled())
        {
            OcclusionCuller::GetInstance().FlushQueuedQueries();
        }

        // Forward+ light culling: dispatch compute after depth is available.
        // Both Forward+ and Deferred (which currently aliases to Forward+ for
        // culling in ApplyRendererSettings) read depth from the active render
        // target, so this works identically for either FB.
        auto& forwardPlus = Renderer3D::GetForwardPlus();
        if (forwardPlus.ShouldUseForwardPlus() && depthPrepass)
        {
            const u32 depthTexID = renderFB->GetDepthAttachmentRendererID();
            forwardPlus.DispatchCulling(
                Renderer3D::GetViewMatrix(),
                Renderer3D::GetProjectionMatrix(),
                depthTexID);
            forwardPlus.BindForShading();
        }

        // Set up color pass state AFTER occlusion flush (which mutates GL state)
        if (depthPrepass)
        {
            // Pass 2: color — CommandDispatch overrides per-command depth state
            // to GL_LEQUAL + depth mask false so fragments at the same depth as
            // the prepass pass the test while preventing new depth writes.
            CommandDispatch::SetDepthPrepassColorPassActive(true);
        }

        // Apply wireframe overlay only for the color pass (not depth prepass)
        bool const wireframe = Renderer3D::GetRendererSettings().WireframeOverlay;
        if (wireframe)
        {
            rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        }

        if (capturing)
            m_CommandBucket.ExecuteWithGPUTiming(rendererAPI);
        else
            m_CommandBucket.Execute(rendererAPI);

        // Restore depth state after prepass
        if (depthPrepass)
        {
            CommandDispatch::SetDepthPrepassColorPassActive(false);
            rendererAPI.SetDepthMask(true);
            rendererAPI.SetDepthFunc(GL_LESS);
        }

        // Restore polygon mode after color pass
        if (wireframe)
        {
            rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        // Unbind Forward+ SSBOs after the color pass
        if (forwardPlus.ShouldUseForwardPlus() && depthPrepass)
        {
            // Render debug heatmap overlay before unbinding (needs grid SSBO + UBO).
            // Skip in Deferred mode — heatmap writes to attachment 0 which is
            // the G-Buffer albedo RT, not the scene color target.
            if (forwardPlus.IsDebugVisualization() && !deferredActive)
            {
                auto quadVAO = Renderer3D::GetFullscreenQuadVAO();
                auto debugShader = Renderer3D::GetForwardPlusDebugShader();
                if (quadVAO && debugShader)
                {
                    forwardPlus.RenderDebugOverlay(quadVAO->GetRendererID(), debugShader);
                }
            }
            forwardPlus.UnbindAfterShading();
        }

        ++m_FrameCounter;

        if (capturing)
        {
            captureManager.OnFrameEnd(
                m_FrameCounter,
                m_CommandBucket.GetLastSortTimeMs(),
                m_CommandBucket.GetLastBatchTimeMs(),
                m_CommandBucket.GetLastExecuteTimeMs());
        }

        renderFB->Unbind();

        // Deferred G-Buffer MSAA resolve. Two sub-modes:
        //   1. Per-sample lighting (MSAASampleCount > 1 && PerSampleLighting)
        //      — resolve ONLY the depth attachment so decals can sample
        //      reconstructed world positions, while colour attachments stay
        //      multisample for DeferredLighting_MSAA to shade per-sample.
        //   2. Resolve-before-lighting — full resolve (colour + depth) so
        //      the standard DeferredLighting shader samples a single-sample
        //      copy; loses per-sample shading detail but keeps a single code
        //      path for non-MSAA and resolve-path MSAA.
        //   Non-MSAA always falls into #2 (Resolve is a no-op).
        const bool perSampleLighting = deferredActive && m_GBuffer && m_GBuffer->GetSampleCount() > 1 && rendererSettings.Deferred.PerSampleLighting;
        const bool debugNeedsColour = rendererSettings.Deferred.DebugChannel != 0;
        if (deferredActive && m_GBuffer)
        {
            // When per-sample lighting is active, resolve ONLY depth here so
            // decals can reconstruct world position from single-sample depth.
            // Colour resolve is deferred until after decals so the debug blit
            // (if any) sees post-decal texels. When per-sample is off, do a
            // full resolve now — decals write into the resolved FB directly.
            if (perSampleLighting)
                m_GBuffer->ResolveDepthOnly();
            else
                m_GBuffer->Resolve();
        }

        // Deferred opaque decals are drained by the dedicated
        // `DeferredOpaqueDecalPass` graph node (runs between ScenePass and
        // DeferredLightingPass) — see `Renderer3D::ConfigureRenderGraph`.
        // The pass's resource declarations make the dependency visible to
        // the L5 hazard validator.
        //
        // The per-sample "post-decal colour resolve" below still runs here
        // because it must be observable to `BlitGBufferDebug` in the same
        // Execute() call. By the time we reach this point the graph
        // scheduler has NOT yet executed the opaque-decal pass (this is
        // still inside ScenePass), so the debug blit will see *pre-decal*
        // colour in per-sample + debug-channel mode. That matches the
        // behaviour pre-extraction for every case except
        // `perSampleLighting && debugNeedsColour`; callers relying on the
        // debug overlay to reflect decal contributions should disable
        // per-sample lighting. Documented as a known limitation of the
        // debug overlay; non-debug paths are unaffected.

        // Per-sample path: force a color resolve whenever a downstream pass
        // samples resolved G-Buffer color attachments (debug overlay, SSAO, GTAO).
        // This keeps the resolved single-sample G-Buffer attachments current
        // for AO/export consumers while preserving the multisample
        // attachments for per-sample deferred lighting.
        const auto& postProcessSettings = Renderer3D::GetPostProcessSettings();
        const bool aoNeedsResolvedNormals =
            (postProcessSettings.ActiveAOTechnique == AOTechnique::SSAO && postProcessSettings.SSAOEnabled) ||
            (postProcessSettings.ActiveAOTechnique == AOTechnique::GTAO && postProcessSettings.GTAOEnabled);
        if (const bool postNeedsResolvedVelocity = postProcessSettings.MotionBlurEnabled || postProcessSettings.TAAEnabled; perSampleLighting && (debugNeedsColour || aoNeedsResolvedNormals || postNeedsResolvedVelocity))
        {
            m_GBuffer->Resolve();
        }

        // Publish scene-derived textures through graph-owned handles. The
        // scene pass still renders into the legacy scene/G-Buffer
        // attachments, but downstream consumers now sample the exported graph
        // textures instead of importing those attachments directly.
        const auto copySceneExport = [this, &context](const RGTextureHandle handle, const u32 sourceTextureID)
        {
            if (!handle.IsValid() || sourceTextureID == 0u ||
                m_FramebufferSpec.Width == 0u || m_FramebufferSpec.Height == 0u)
            {
                return;
            }

            const u32 exportedTextureID = context.ResolveTexture(handle);
            if (exportedTextureID == 0u || exportedTextureID == sourceTextureID)
                return;

            glCopyImageSubData(sourceTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               exportedTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               static_cast<GLsizei>(m_FramebufferSpec.Width),
                               static_cast<GLsizei>(m_FramebufferSpec.Height),
                               1);
        };

        const u32 sourceDepthID = deferredActive && m_GBuffer
                                      ? m_GBuffer->GetDepthAttachmentID()
                                      : m_Target->GetDepthAttachmentRendererID();
        copySceneExport(m_SelectedSceneDepthExport, sourceDepthID);

        if (!deferredActive)
        {
            const u32 sourceNormalsID = m_Target->GetColorAttachmentRendererID(2);
            copySceneExport(m_SelectedSceneNormalsExport, sourceNormalsID);
        }

        const u32 sourceVelocityID = deferredActive && m_GBuffer
                                         ? m_GBuffer->GetColorAttachmentID(GBuffer::Velocity)
                                         : m_Target->GetColorAttachmentRendererID(3);
        copySceneExport(m_SelectedVelocityExport, sourceVelocityID);

        // Deferred debug visualisation: until DeferredLightingPass lands in
        // Phase 3, copy the selected G-Buffer channel into the forward scene
        // target's color[0] so post-process and the editor viewport display
        // *something* instead of whatever is left of the cleared forward FB.
        if (deferredActive && m_GBuffer)
        {
            BlitGBufferDebug(rendererSettings.Deferred.DebugChannel);
        }
        else if (!deferredActive && rendererSettings.DebugVelocityOverlayForward)
        {
            // Forward / Forward+ velocity overlay: mirror the Deferred
            // DebugChannel=5 capability for the forward paths.
            BlitForwardVelocityDebug();
        }
    }

    void SceneRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::SetupFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        // Create or recreate the framebuffer
        if (!m_Target)
        {
            Init(m_FramebufferSpec);
        }
        else
        {
            m_Target->Resize(width, height);
        }
    }

    void SceneRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_Target)
        {
            m_Target->Resize(width, height);
            OLO_CORE_INFO("SceneRenderPass: Resized framebuffer to {}x{}", width, height);
        }

        // Keep the G-Buffer in lockstep with the forward target so a runtime
        // Forward ↔ Deferred swap doesn't leave stale dimensions behind.
        if (m_GBuffer)
            m_GBuffer->Resize(width, height);
    }

    void SceneRenderPass::PrepareDeferredResources(u32 sampleCount)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target)
        {
            OLO_CORE_WARN("SceneRenderPass::PrepareDeferredResources: No scene framebuffer available");
            return;
        }

        if (m_FramebufferSpec.Width == 0 || m_FramebufferSpec.Height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::PrepareDeferredResources: Invalid dimensions {}x{}",
                          m_FramebufferSpec.Width,
                          m_FramebufferSpec.Height);
            return;
        }

        EnsureGBuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height, sampleCount);
    }

    void SceneRenderPass::EnsureGBuffer(u32 width, u32 height, u32 sampleCount)
    {
        if (sampleCount == 0)
            sampleCount = 1;

        if (!m_GBuffer || m_GBufferSampleCount != sampleCount)
        {
            m_GBuffer = GBuffer::Create(width, height, sampleCount);
            m_GBufferSampleCount = sampleCount;
            OLO_CORE_INFO("SceneRenderPass: Created G-Buffer {}x{} x{}MSAA", width, height, sampleCount);
            return;
        }

        if (m_GBuffer->GetWidth() != width || m_GBuffer->GetHeight() != height)
        {
            m_GBuffer->Resize(width, height);
        }
    }

    void SceneRenderPass::BlitGBufferDebug(u32 channel)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_GBuffer || !m_Target)
            return;

        // Clamp channel to G-Buffer attachment range. Channel IDs come from
        // DeferredSettings::DebugChannel (0=off, 1=Albedo, 2=Normal,
        // 3=Roughness/Metallic/AO, 4=Emissive, 5=Velocity). 0 leaves the
        // forward target cleared (no blit) to distinguish "disabled" from
        // "attachment 0".
        if (channel == 0)
            return;

        // RAII guard: captures GL state on entry and restores the core subset
        // (depth / blend / stencil / cull / polygon / scissor / viewport /
        // FBO bindings / active program) on destruction. The channel==3
        // branch below binds m_DebugRMAShader + the fullscreen-tri VAO and
        // mutates the read-buffer; those bindings are explicitly cleared
        // before return so this guard stays clean rather than acting as a
        // silenced "every binding is a leak" detector.
        GLStateGuard guard("SceneRenderPass::BlitGBufferDebug", GLStateGuard::Policy::Restore);

        // Build the target FB's full multi-attachment draw-buffer list from
        // its spec. The previous hardcoded 4-entry restore broke when a
        // scene FB configured with fewer or more color attachments was
        // installed (e.g. when TAA was disabled and velocity dropped).
        const auto& targetSpec = m_Target->GetSpecification();
        u32 targetColorCount = 0;
        for (const auto& att : targetSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++targetColorCount;
        }
        std::array<GLenum, 16> fullDrawBufs{};
        const u32 fullN = std::min<u32>(targetColorCount, static_cast<u32>(fullDrawBufs.size()));
        for (u32 i = 0; i < fullN; ++i)
            fullDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;

        // Channel 3 (RMA) needs data from TWO attachments — RT0.a (metallic)
        // and RT1.zw (roughness, AO). glBlitFramebuffer cannot swizzle, so
        // use a dedicated fullscreen shader for this one channel.
        if (channel == 3)
        {
            if (!m_DebugRMAShader)
                m_DebugRMAShader = Shader::Create("assets/shaders/DebugGBuffer_RMA.glsl");
            if (!m_DebugRMAShader)
                return;

            m_Target->Bind();

            const u32 dstFB = m_Target->GetRendererID();
            const GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glNamedFramebufferDrawBuffers(dstFB, 1, drawBufs);

            const u32 w = m_GBuffer->GetWidth();
            const u32 h = m_GBuffer->GetHeight();
            RenderCommand::SetViewport(0, 0, w, h);
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);

            m_DebugRMAShader->Bind();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_ALBEDO,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Albedo));
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_GBUFFER_NORMAL,
                                       m_GBuffer->GetColorAttachmentID(GBuffer::Normal));

            auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);

            // Restore the scene FB's multi-attachment draw-buffer list so the
            // downstream passes (post-process, UI) find the expected slots
            // (including RT3 velocity for TAA). Count is computed from the
            // FB spec above rather than hardcoded.
            glNamedFramebufferDrawBuffers(dstFB, static_cast<GLsizei>(fullN), fullDrawBufs.data());

            RenderCommand::SetDepthMask(true);
            RenderCommand::SetDepthTest(true);

            // Copy depth across so selection-outline / UI still depth-test.
            const u32 srcFB = m_GBuffer->GetSamplingFramebuffer()->GetRendererID();
            glBlitNamedFramebuffer(
                srcFB, dstFB,
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                GL_DEPTH_BUFFER_BIT, GL_NEAREST);

            // Unbind the blit shader + VAO so the RAII guard sees us leave
            // shader/program/VAO state at zero, matching entry expectations
            // for downstream passes that rebind their own.
            ::glUseProgram(0);
            ::glBindVertexArray(0);
            return;
        }

        u32 attachmentIndex = 0;
        switch (channel)
        {
            case 1:
                attachmentIndex = GBuffer::Albedo;
                break;
            case 2:
                attachmentIndex = GBuffer::Normal;
                break;
            case 4:
                attachmentIndex = GBuffer::Emissive;
                break;
            case 5:
                attachmentIndex = GBuffer::Velocity;
                break;
            default:
                attachmentIndex = GBuffer::Albedo;
                break;
        }

        const u32 srcFB = m_GBuffer->GetSamplingFramebuffer()->GetRendererID();
        const u32 dstFB = m_Target->GetRendererID();
        const u32 w = m_GBuffer->GetWidth();
        const u32 h = m_GBuffer->GetHeight();

        // Select source attachment on the read FB and destination attachment 0
        // on the draw FB. glBlitNamedFramebuffer requires both FBs to have
        // the read/draw buffers pre-selected; do so via DSA.
        const GLenum srcAttach = GL_COLOR_ATTACHMENT0 + attachmentIndex;
        glNamedFramebufferReadBuffer(srcFB, srcAttach);
        const GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
        glNamedFramebufferDrawBuffers(dstFB, 1, drawBufs);

        glBlitNamedFramebuffer(
            srcFB, dstFB,
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Restore the draw FB's draw-buffer list using the count captured
        // from the target FB spec above — narrowing to fewer attachments
        // would drop later-shader outputs (e.g. PBR_MultiLight's motion
        // vector at layout(location=3)), breaking TAA/MotionBlur.
        glNamedFramebufferDrawBuffers(dstFB, static_cast<GLsizei>(fullN), fullDrawBufs.data());

        // Also copy depth so downstream passes (post-process, selection
        // outline, UI) have a coherent depth buffer.
        glBlitNamedFramebuffer(
            srcFB, dstFB,
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        // Reset the G-Buffer's read buffer to attachment 0 so any downstream
        // read on that FB picks up a deterministic default instead of the
        // last debug channel we selected.
        glNamedFramebufferReadBuffer(srcFB, GL_COLOR_ATTACHMENT0);
    }

    void SceneRenderPass::BlitForwardVelocityDebug()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target)
            return;

        // Velocity is attachment 3 on the forward scene FB. If the scene FB
        // wasn't created with it (deferred path swaps to G-Buffer) just
        // bail — the caller already checks deferred state, but defensive.
        const auto& attachments = m_Target->GetSpecification().Attachments.Attachments;
        if (attachments.size() <= 3 ||
            attachments[3].TextureFormat != FramebufferTextureFormat::RG16F)
            return;

        // RAII guard — pure DSA blit, no shader/VAO mutations. Only the
        // read-buffer selection drifts, and it's restored at the end of the
        // function so the guard exits clean.
        GLStateGuard guard("SceneRenderPass::BlitForwardVelocityDebug", GLStateGuard::Policy::Restore);

        const u32 fb = m_Target->GetRendererID();
        const u32 w = m_Target->GetSpecification().Width;
        const u32 h = m_Target->GetSpecification().Height;

        // Build the restore list dynamically from the spec so it matches the
        // attachment count actually in use (instead of a hardcoded 4-entry
        // list that would narrow the target FB on a 3-attachment config).
        u32 colorCount = 0;
        for (const auto& att : attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++colorCount;
        }
        std::array<GLenum, 16> prevDrawBufs{};
        const u32 n = std::min<u32>(colorCount, static_cast<u32>(prevDrawBufs.size()));
        for (u32 i = 0; i < n; ++i)
            prevDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;

        glNamedFramebufferReadBuffer(fb, GL_COLOR_ATTACHMENT3);
        const GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
        glNamedFramebufferDrawBuffers(fb, 1, drawBufs);

        glBlitNamedFramebuffer(
            fb, fb,
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
            GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Restore the scene FB's full multi-attachment draw-buffer list for
        // downstream passes (post-process, UI composite); see comment above.
        glNamedFramebufferDrawBuffers(fb, static_cast<GLsizei>(n), prevDrawBufs.data());

        // Reset the read buffer selection so subsequent reads on this FB
        // see the default (attachment 0) rather than the velocity slot.
        glNamedFramebufferReadBuffer(fb, GL_COLOR_ATTACHMENT0);
    }

    void SceneRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        m_SelectedSceneDepthExport = {};
        m_SelectedSceneNormalsExport = {};
        m_SelectedVelocityExport = {};

        // Recreate the framebuffer with current specs
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            OLO_CORE_INFO("SceneRenderPass reset with framebuffer dimensions: {}x{}",
                          m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            Init(m_FramebufferSpec);
        }
    }
} // namespace OloEngine
