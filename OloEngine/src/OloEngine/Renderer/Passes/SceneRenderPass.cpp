#include "OloEnginePCH.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/DebugViewProvenance.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    // Draw slot 0 -> colour attachment 0, nothing else. Hoisted to file
    // scope so the several blit helpers below share one definition.
    static constexpr std::array<u32, 1> kAttachment0Only = { 0u };

    namespace
    {
        // Blended mesh fragments deliberately do not write the opaque depth
        // prepass, so a depth-derived active list cannot prove their froxels
        // empty. Keep them on the fixed grid rather than silently dropping
        // local lights from glass/alpha-blended materials.
        [[nodiscard]] bool HasForwardLitBlendedGeometry(const CommandBucket& bucket)
        {
            const FrameDataBuffer& frameData = FrameDataBufferManager::Get();
            for (const CommandPacket* packet : bucket.GetPackets())
            {
                if (!packet)
                    continue;

                u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
                if (packet->GetCommandType() == CommandType::DrawMesh)
                    renderStateIndex = packet->GetCommandData<DrawMeshCommand>()->renderStateIndex;
                else if (packet->GetCommandType() == CommandType::DrawMeshInstanced)
                    renderStateIndex = packet->GetCommandData<DrawMeshInstancedCommand>()->renderStateIndex;

                if (renderStateIndex != INVALID_RENDER_STATE_INDEX &&
                    frameData.GetRenderState(renderStateIndex).blendEnabled)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

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
        if (board.Shadows.ShadowMapAtlas.IsValid())
        {
            [[maybe_unused]] const auto shadowAtlasRead = builder.Read(board.Shadows.ShadowMapAtlas, RGReadUsage::ShaderSample);
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

        // Deferred path: bind the G-Buffer instead of the forward scene FB.
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

        // Baked-GI RT (G-Buffer RT5, issue #865) must clear to zero, not to the
        // generic 0.1/1.0 fill: its .a is COVERAGE, and a cleared 1.0 reads as
        // "this pixel has a valid baked texel" everywhere the G-Buffer pass never
        // ran. Same reasoning as the velocity clear below — one shared clear
        // colour cannot be right for a target whose channels are not colour.
        if (deferredActive && m_GBuffer)
        {
            renderFB->ClearAttachment(std::to_underlying(GBuffer::BakedGI), glm::vec4(0.0f));
            renderFB->ClearAttachment(std::to_underlying(GBuffer::Velocity), glm::vec4(0.0f));
        }

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
        rendererAPI.SetDepthFunc(RHI::CompareOp::Less);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(RHI::CullMode::Back);
        rendererAPI.SetPolygonMode(RHI::PolygonMode::Fill);

        // Capture hooks — minimal overhead when not capturing (helped by branch prediction)
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();

        if (capturing)
        {
            // Open this pass's per-pass capture entry, and mark it the SOURCE pass
            // (its bucket becomes the frame's top-level / legacy view). The frame
            // is no longer committed here — Renderer3D::EndScene commits it after
            // the whole graph runs, so Water / Foliage / Decal / ForwardOverlay can
            // accumulate their own per-pass buckets first (issue #463).
            captureManager.BeginPass(GetName());
            captureManager.SetSourcePass(GetName());
            captureManager.OnPreSort(m_CommandBucket);
        }

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
        // Sub-pass GPU timestamp brackets (#316): split this pass's GPU time
        // into DepthPrepass vs Color inside the render-graph executor's pass
        // bracket. The pool prefixes the open pass's registered node name
        // (RenderPipeline names this node "ScenePass"), so these publish as
        // "ScenePass/DepthPrepass" and "ScenePass/Color"; surfaced as
        // subPasses in olo_perf_pass_timings. Strictly additive around the
        // existing Execute calls.
        auto& gpuSubTimers = GPUPassTimerPool::GetInstance();
        if (depthPrepass)
        {
            // Pass 1: depth only — CommandDispatch overrides per-command state
            gpuSubTimers.BeginSubPass("DepthPrepass");
            CommandDispatch::SetDepthPrepassActive(true);
            m_CommandBucket.ExecuteParallel(rendererAPI);
            CommandDispatch::SetDepthPrepassActive(false);
            gpuSubTimers.EndSubPass();
        }

        // Flush deferred occlusion query proxy draws. When a depth prepass ran,
        // the depth buffer is fully populated; otherwise the first Execute below
        // will populate it and queries will rely on the previous frame's depth.
        if (Renderer3D::IsOcclusionCullingEnabled())
        {
            OcclusionCuller::GetInstance().FlushQueuedQueries();
        }

        // Clustered Forward+ light culling. When a single-sample depth prepass
        // populated the current render target, issue #722 reduces each tile to
        // min/max + a 32-bit occupancy mask, compacts the active froxels, and
        // culls only those via an indirect dispatch. With the prepass disabled
        // (or an unresolved MSAA depth attachment), the original fixed-grid
        // dispatch remains the correctness-preserving fallback.
        auto& forwardPlus = Renderer3D::GetForwardPlus();
        if (forwardPlus.ShouldUseForwardPlus())
        {
            // Sub-pass bracket (issue #720): isolates LightCulling.comp's GPU-ms
            // under "ScenePass" instead of leaving it folded into the parent's
            // total (it previously ran outside both DepthPrepass and Color) —
            // needed to measure the thread-group swizzle adopted in the shader.
            gpuSubTimers.BeginSubPass("LightCulling");
            // Deferred virtual geometry is rasterized after ScenePass, so it
            // is absent from this depth attachment just like blended classic
            // geometry. Either case requires the conservative fixed grid.
            const FogSettings& fog = Renderer3D::GetFogSettings();
            const bool singleSampleDepth = renderFB->GetSpecification().Samples == 1u;
            const bool depthAwareLeverEnabled = Renderer3D::IsDepthAwareClusterCullingEnabled();
            const bool inspectDepthContributors = depthPrepass && singleSampleDepth && depthAwareLeverEnabled;
            const ClusteredLighting::DepthAwareFrameInputs depthAwareInputs{
                .DepthPrepassAvailable = depthPrepass,
                .SingleSampleDepth = singleSampleDepth,
                .LeverEnabled = depthAwareLeverEnabled,
                .HasBlendedGeometry = inspectDepthContributors && HasForwardLitBlendedGeometry(m_CommandBucket),
                .HasVirtualGeometry = !VirtualMeshRegistry::Get().GetSubmissions().empty(),
                .HasVolumetricFog = fog.Enabled && fog.EnableVolumetric,
            };
            forwardPlus.DispatchCulling(
                Renderer3D::GetViewMatrix(),
                Renderer3D::GetProjectionMatrix(),
                renderFB->GetDepthAttachmentHandle(),
                depthAwareInputs);
            gpuSubTimers.EndSubPass();
            forwardPlus.BindForShading();
        }

        // Distance-impostor reflection probes (issue #705): upload changed
        // array layers + the probe UBO and fill the per-cluster probe mask,
        // then publish for the colour sub-pass (forward consumers) — the
        // DeferredLightingPass re-binds for the fullscreen lighting draw.
        // Deliberately OUTSIDE the Forward+ gate: the probe grid carries its
        // own copy of the cluster parameters, so probes keep working when
        // Forward+ is inactive.
        {
            auto& reflectionProbes = Renderer3D::GetReflectionProbes();
            u32 const vpWidth = m_Target ? m_Target->GetSpecification().Width : 0;
            u32 const vpHeight = m_Target ? m_Target->GetSpecification().Height : 0;
            reflectionProbes.PrepareFrame(Renderer3D::GetViewMatrix(),
                                          Renderer3D::GetProjectionMatrix(),
                                          vpWidth, vpHeight);
            reflectionProbes.BindForShading();
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
            rendererAPI.SetPolygonMode(RHI::PolygonMode::Line);
        }

        // "Color" is emitted even without a depth prepass, so a missing
        // DepthPrepass sub-entry in olo_perf_pass_timings reads as "prepass
        // off" rather than "sub-pass timing broken".
        gpuSubTimers.BeginSubPass("Color");
        if (capturing)
            m_CommandBucket.ExecuteWithGPUTiming(rendererAPI);
        else
            m_CommandBucket.ExecuteParallel(rendererAPI);
        gpuSubTimers.EndSubPass();

        // Restore depth state after prepass
        if (depthPrepass)
        {
            CommandDispatch::SetDepthPrepassColorPassActive(false);
            rendererAPI.SetDepthMask(true);
            rendererAPI.SetDepthFunc(RHI::CompareOp::Less);
        }

        // Restore polygon mode after color pass
        if (wireframe)
        {
            rendererAPI.SetPolygonMode(RHI::PolygonMode::Fill);
        }

        // Unbind Forward+ SSBOs after the color pass
        if (forwardPlus.ShouldUseForwardPlus())
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
                    forwardPlus.RenderDebugOverlay(quadVAO->GetRHIHandle(), debugShader);
                }
            }
            forwardPlus.UnbindAfterShading();
        }

        ++m_FrameCounter;

        if (capturing)
        {
            // Record this (source) pass's timings into its per-pass entry; they
            // become the committed frame's top-level Stats. The commit itself runs
            // centrally in Renderer3D::EndScene after the whole graph executes.
            captureManager.RecordPassTimings(
                m_CommandBucket.GetLastSortTimeMs(),
                m_CommandBucket.GetLastBatchTimeMs(),
                m_CommandBucket.GetLastExecuteTimeMs());
        }

        renderFB->Unbind();

        // Content version (issue #1329): the opaque G-Buffer geometry is in.
        // Virtual geometry, deferred two-phase occlusion phase 2 and the
        // opaque decals each bump this again from their own graph nodes, and
        // GBufferDebugPass records the version it extracted so a debug image
        // taken ahead of a late writer is legible as such rather than passing
        // for the version lighting consumed.
        if (deferredActive && m_GBuffer)
            m_GBuffer->MarkWritten("ScenePass");

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
        if (deferredActive && m_GBuffer)
        {
            // When per-sample lighting is active, resolve ONLY depth here so
            // decals can reconstruct world position from single-sample depth.
            // Colour resolve is deferred until after decals so every
            // single-sample consumer sees post-decal texels. When per-sample
            // is off, do a full resolve now — decals write into the resolved
            // FB directly.
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
        // THE DEBUG EXTRACTION NO LONGER HAPPENS HERE (issue #1329). It used
        // to, and it could not be right: this is still inside ScenePass, so
        // the graph scheduler has not yet run virtual geometry, the deferred
        // two-phase occlusion phase 2 or the opaque decals, and the image
        // therefore described a G-Buffer that no lighting consumer ever saw.
        // `GBufferDebugPass` performs it now, registered immediately before
        // `DeferredLightingPass` — after every late writer — and records the
        // content version it read.

        // Per-sample path: force a color resolve whenever a downstream pass
        // samples resolved G-Buffer color attachments (SSAO, GTAO, velocity
        // consumers). This keeps the resolved single-sample G-Buffer
        // attachments current for AO/export consumers while preserving the
        // multisample attachments for per-sample deferred lighting. The debug
        // view is deliberately NOT one of the conditions any more: it resolves
        // for itself, after the late writers, in its own pass.
        const auto& postProcessSettings = Renderer3D::GetPostProcessSettings();
        const bool aoNeedsResolvedNormals =
            (postProcessSettings.ActiveAOTechnique == AOTechnique::SSAO && postProcessSettings.SSAOEnabled) ||
            (postProcessSettings.ActiveAOTechnique == AOTechnique::GTAO && postProcessSettings.GTAOEnabled);
        if (const bool postNeedsResolvedVelocity = postProcessSettings.MotionBlurEnabled || postProcessSettings.TAAEnabled || m_SelectedVelocityExport.IsValid(); perSampleLighting && (aoNeedsResolvedNormals || postNeedsResolvedVelocity))
        {
            m_GBuffer->Resolve();
        }

        // Publish scene-derived textures through graph-owned handles. The
        // scene pass still renders into the legacy scene/G-Buffer
        // attachments, but downstream consumers now sample the exported graph
        // textures instead of importing those attachments directly.
        // Identities throughout (issue #691): the export target
        // is a graph TRANSIENT, which only began answering ResolveTextureHandle
        // once the planner recorded a handle for pooled textures. The self-copy
        // guard now compares OBJECTS -- under driver names a recycled name could
        // make source and export look identical and skip a copy the frame needed.
        const auto copySceneExport = [this, &context](const RGTextureHandle handle,
                                                      const RHI::ResourceHandle sourceTexture)
        {
            if (!handle.IsValid() || !sourceTexture.IsValid() ||
                m_FramebufferSpec.Width == 0u || m_FramebufferSpec.Height == 0u)
            {
                return;
            }

            const RHI::ResourceHandle exportedTexture = context.ResolveTextureHandle(handle);
            if (!exportedTexture.IsValid() || exportedTexture == sourceTexture)
                return;

            RenderCommand::CopyImageSubData(sourceTexture, RendererAPI::TextureTargetType::Texture2D,
                                            exportedTexture, RendererAPI::TextureTargetType::Texture2D,
                                            m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        };

        const RHI::ResourceHandle sourceDepth = deferredActive && m_GBuffer
                                                    ? m_GBuffer->GetDepthAttachmentHandle()
                                                    : m_Target->GetDepthAttachmentHandle();
        copySceneExport(m_SelectedSceneDepthExport, sourceDepth);

        if (!deferredActive)
        {
            const RHI::ResourceHandle sourceNormals = m_Target->GetColorAttachmentHandle(2);
            copySceneExport(m_SelectedSceneNormalsExport, sourceNormals);
        }

        const RHI::ResourceHandle sourceVelocity = deferredActive && m_GBuffer
                                                       ? m_GBuffer->GetColorAttachmentHandle(GBuffer::Velocity)
                                                       : m_Target->GetColorAttachmentHandle(3);
        copySceneExport(m_SelectedVelocityExport, sourceVelocity);

        // Forward / Forward+ velocity overlay: mirrors the Deferred
        // DebugChannel=5 capability for the forward paths. It STAYS here, and
        // that is not an oversight — there is no G-Buffer on these paths, so
        // ScenePass is the last writer of the velocity attachment it reads and
        // this IS the final version. The deferred channels moved out to
        // `GBufferDebugPass` precisely because that was not true for them.
        if (!deferredActive && rendererSettings.DebugVelocityOverlayForward)
        {
            BlitForwardVelocityDebug();

            // It is a debug extraction, so it says so (issue #1329). There is
            // no G-Buffer on this path — version 0 and no writer is the honest
            // answer, not a placeholder — but the frame and the pass are what
            // a reader needs to tell a live overlay from a frozen one, and
            // they are as true here as they are on the deferred path.
            DebugViewProvenance record;
            record.Frame = FrameResourceManager::Get().GetTotalFrameCount();
            record.Channel = 5u; // the forward analogue of DebugChannel 5
            record.Stage = DebugViewStage::FinalGBuffer;
            record.Pass = GetName();
            DebugViewProvenanceRegistry::Publish(record);
        }
        else if (!deferredActive)
        {
            // GBufferDebugPass is not registered on the forward paths, so
            // nothing downstream would ever retire a record left over from a
            // Deferred session — it would sit there ageing, attached to a
            // frame that has nothing to do with what is on screen.
            DebugViewProvenanceRegistry::Invalidate();
        }
        else
        {
            // Deferred: GBufferDebugPass owns the record, either way.
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

        const RHI::ResourceHandle fb = m_Target->GetRHIHandle();
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
        RenderCommand::SetFramebufferReadAttachment(fb, 3);
        RenderCommand::SetFramebufferDrawAttachments(fb, kAttachment0Only);

        RenderCommand::BlitFramebuffer(
            fb, fb,
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            RHI::BlitAspect::Color, RHI::Filter::Nearest);

        // Restore the scene FB's full multi-attachment draw-buffer list for
        // downstream passes (post-process, UI composite); see comment above.
        RenderCommand::RestoreAllFramebufferDrawAttachments(fb, colorCount);

        // Reset the read buffer selection so subsequent reads on this FB
        // see the default (attachment 0) rather than the velocity slot.
        RenderCommand::SetFramebufferReadAttachment(fb, 0);
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
