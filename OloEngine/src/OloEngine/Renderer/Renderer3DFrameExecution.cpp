#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

namespace OloEngine
{
    void Renderer3D::SetParticleRenderCallback(RenderCallback callback)
    {
        s_Data.PendingParticleRenderCallback = std::move(callback);
    }

    void Renderer3D::SubmitFluidDraw(const FluidRenderData& draw)
    {
        s_Data.PendingFluidDraws.push_back(draw);
    }

    void Renderer3D::SetUICompositeRenderCallback(RenderCallback callback)
    {
        s_Data.PendingUICompositeRenderCallback = std::move(callback);
    }

    void Renderer3D::SetSelectionOutlineEntityIDs(const std::vector<i32>& ids)
    {
        s_Data.SelectionOutlineEntityIDs = ids;
    }

    void Renderer3D::GenerateOcclusionHZB()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.HZBOcclusionCullingEnabled)
            return;

        // Frozen culling camera (issue #726): stop regenerating. The depth that
        // just rendered is the OBSERVER's, and a pyramid built from it would
        // occlusion-cull against surfaces the frozen camera never saw -- a
        // plausible cut that is not the frozen one. Retaining the pre-freeze
        // pyramid keeps it paired with CullPrevViewProjectionMatrix, which is
        // pinned at the same instant, so the occlusion test stays exactly the
        // one the frozen camera made. OcclusionHZBValid deliberately stays true:
        // the pyramid is stale by design here, not missing.
        if (s_Data.CullingCameraFrozen)
            return;

        // Any path that fails to regenerate the pyramid must invalidate it:
        // leaving OcclusionHZBValid true would let next frame's phase-1 sample a
        // stale pyramid (older than PrevViewProjectionMatrix) and mis-cull.
        const Ref<SceneRenderPass>& scenePass = s_Data.Pipeline->FrameCorePasses.Scene;
        if (!scenePass)
        {
            s_Data.OcclusionHZBValid = false;
            return;
        }

        // Scene depth source: the G-Buffer depth in Deferred, the forward scene
        // target's depth attachment otherwise — mirrors SceneRenderPass's own
        // export resolution (SceneRenderPass.cpp). The depth attachment holds
        // the final geometry depth now that the whole graph has executed.
        //
        // The G-Buffer branch MUST be gated on the deferred path being active,
        // not merely on the G-Buffer existing: `SceneRenderPass` is a
        // process-global pass object whose `m_GBuffer` is lazily created on the
        // first Deferred frame and never released, so after any Deferred render
        // `GetGBuffer()` stays non-null for the rest of the process. In
        // Forward/Forward+ the scene renders to `GetTarget()`, NOT the G-Buffer,
        // so reading the retained-but-stale G-Buffer depth here builds the HZB
        // from a prior Deferred scene's depth — a false occlusion cull that
        // punches a hole in the forward frame. This surfaced as the #549
        // cross-test order-dependence (a Deferred test earlier in the shuffle
        // leaves the G-Buffer non-null), and is equally a runtime Deferred→
        // Forward path-switch bug. Mirror SceneRenderPass's `deferredActive &&
        // m_GBuffer` gate so the HZB always samples the depth the ACTIVE path
        // actually wrote this frame.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        RHI::ResourceHandle depthTex{};
        if (const Ref<GBuffer>& gbuffer = scenePass->GetGBuffer(); deferredActive && gbuffer)
        {
            depthTex = gbuffer->GetDepthAttachmentHandle();
        }
        else if (Ref<Framebuffer> target = scenePass->GetTarget())
        {
            depthTex = target->GetDepthAttachmentHandle();
        }

        const auto& spec = scenePass->GetFramebufferSpecification();
        if (!depthTex.IsValid() || spec.Width == 0 || spec.Height == 0)
        {
            s_Data.OcclusionHZBValid = false;
            return;
        }

        // Resize is a cheap no-op once the power-of-2 bucket is stable. Max
        // reduction was selected once at Init (conservative occlusion: each
        // coarse texel keeps the FARTHEST nearest-surface depth beneath it).
        s_Data.OcclusionHZB.Resize(spec.Width, spec.Height);
        if (!s_Data.OcclusionHZB.IsValid())
        {
            s_Data.OcclusionHZBValid = false;
            return;
        }

        s_Data.OcclusionHZB.Generate(depthTex);
        // Valid from here on — next frame's instance cull may sample it.
        s_Data.OcclusionHZBValid = true;
    }

    GPUFrustumCuller::HZBOcclusionInputs Renderer3D::BuildCurrentOcclusionHZB(RHI::ResourceHandle depthTexture,
                                                                              u32 width, u32 height)
    {
        GPUFrustumCuller::HZBOcclusionInputs inputs; // Enabled = false by default

        if (!depthTexture.IsValid() || width == 0 || height == 0)
            return inputs;

        // Frozen culling camera (issue #726): refuse. This rebuild overwrites
        // the retained pyramid IN PLACE, so running it while frozen would
        // destroy the frozen depth every phase-1 cull is testing against. Both
        // callers already gate their two-phase paths on the freeze; this is the
        // backstop, because the failure it prevents is silent -- the frame still
        // renders, with a cut that is not the frozen one.
        if (s_Data.CullingCameraFrozen)
            return inputs;

        // Rebuild the persistent pyramid from THIS frame's partial depth
        // (occluders + phase-1 survivors). This overwrites the previous-frame
        // pyramid, which phase 1 already consumed at submission; the tail-of-
        // EndScene GenerateOcclusionHZB() rebuilds it again from the final depth
        // for next frame's phase 1.
        s_Data.OcclusionHZB.Resize(width, height);
        if (!s_Data.OcclusionHZB.IsValid())
            return inputs;
        s_Data.OcclusionHZB.Generate(depthTexture);

        inputs.Enabled = true;
        inputs.HZBTexture = s_Data.OcclusionHZB.GetHZBTexture();
        inputs.MipCount = s_Data.OcclusionHZB.GetMipCount();
        // Current-frame pyramid → reproject phase-2 bounds with the CURRENT VP.
        inputs.PrevViewProjection = s_Data.ViewProjectionMatrix;
        inputs.HZBSize = glm::vec2(static_cast<f32>(s_Data.OcclusionHZB.GetHZBWidth()),
                                   static_cast<f32>(s_Data.OcclusionHZB.GetHZBHeight()));
        inputs.HZBUVFactor = s_Data.OcclusionHZB.GetUVFactor();
        inputs.DepthBias = s_Data.HZBOcclusionDepthBias;
        return inputs;
    }

    GPUFrustumCuller::HZBOcclusionInputs Renderer3D::GetRetainedOcclusionHZB()
    {
        GPUFrustumCuller::HZBOcclusionInputs inputs; // Enabled = false by default

        // Same three-part guard RenderPipeline applies before handing the pyramid
        // to the instance cull: the global toggle (which also honours the
        // force-disable-culling debug override), the frame-0 / post-invalidation
        // validity flag, and the generator actually holding a texture.
        if (!IsHZBOcclusionCullingEnabled() || !s_Data.OcclusionHZBValid || !s_Data.OcclusionHZB.IsValid())
            return inputs;

        inputs.Enabled = true;
        inputs.HZBTexture = s_Data.OcclusionHZB.GetHZBTexture();
        inputs.MipCount = s_Data.OcclusionHZB.GetMipCount();
        // The pyramid is in LAST frame's screen space; the transforms the cull
        // reads are shifted by THIS frame's render origin, so the previous VP has
        // to be made relative to that same origin or `clip = VP_world *
        // relativePos` is garbage far from the origin (issue #429).
        inputs.PrevViewProjection = MakeViewProjectionRelative(s_Data.CullPrevViewProjectionMatrix, s_Data.RenderOrigin);
        inputs.HZBSize = glm::vec2(static_cast<f32>(s_Data.OcclusionHZB.GetHZBWidth()),
                                   static_cast<f32>(s_Data.OcclusionHZB.GetHZBHeight()));
        inputs.HZBUVFactor = s_Data.OcclusionHZB.GetUVFactor();
        inputs.DepthBias = s_Data.HZBOcclusionDepthBias;
        return inputs;
    }

    void Renderer3D::DispatchOcclusionPhase2(const GPUFrustumCuller::TwoPhaseCullResult& cull,
                                             const GPUFrustumCuller::HZBOcclusionInputs& currentHZB)
    {
        if (s_Data.GPUFrustumCuller)
            s_Data.GPUFrustumCuller->DispatchPhase2(cull, currentHZB);
    }

    GPUDrivenOcclusionPass* Renderer3D::GetGPUOcclusionPass()
    {
        if (!s_Data.Pipeline)
            return nullptr;
        return s_Data.Pipeline->RenderStreamPasses.GPUOcclusion.Raw();
    }

    DeferredGPUOcclusionPass* Renderer3D::GetDeferredGPUOcclusionPass()
    {
        if (!s_Data.Pipeline)
            return nullptr;
        return s_Data.Pipeline->SceneCompositePasses.DeferredGPUOcclusion.Raw();
    }

    void Renderer3D::EndScene()
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE_AUTO("Renderer3D::EndScene");
        auto& pipeline = *s_Data.Pipeline;

        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }

        {
            OLO_PERF_SCOPE_AUTO("Renderer3D::ConfigurePassesForFrame");
            pipeline.ConfigurePassesForFrame(s_Data);
        }

        // Populate the graph blackboard AFTER per-frame pass configuration so
        // AOBuffer / PostProcessColor imports resolve the current frame's
        // active technique and enabled outputs rather than last frame's state.
        // Single fingerprint of all per-frame inputs that drive both the
        // blackboard population and the per-pass Setup() callbacks. Pass it to
        // both layers so they cache consistently — if the fingerprint matches
        // last frame's, both layers short-circuit and the cached handles +
        // submission plan are reused as-is.
        const u64 frameFingerprint = pipeline.ComputeBlackboardFingerprint(s_Data);

        {
            OLO_PERF_SCOPE_AUTO("Renderer3D::PopulateBlackboard");
            pipeline.PopulateBlackboard(s_Data);
        }

        {
            OLO_PERF_SCOPE_AUTO("Renderer3D::UploadExecutionState");
            pipeline.UploadExecutionState(s_Data);
        }

        // Phase C: compile graph-native pass declarations before execution.
        s_Data.RGraph->BuildFrameGraph(frameFingerprint);

        bool buildStatsChanged = false;
        {
            const auto& buildStats = s_Data.RGraph->GetLastBuildStats();
            static RenderGraph::FrameBuildStats s_LastBuildStats{};
            static bool s_HasLastBuildStats = false;

            buildStatsChanged = !s_HasLastBuildStats ||
                                buildStats.PassesVisited != s_LastBuildStats.PassesVisited ||
                                buildStats.DeclaredReads != s_LastBuildStats.DeclaredReads ||
                                buildStats.DeclaredWrites != s_LastBuildStats.DeclaredWrites ||
                                buildStats.DerivedEdges != s_LastBuildStats.DerivedEdges ||
                                buildStats.OrderSensitiveResults != s_LastBuildStats.OrderSensitiveResults;

            if (buildStatsChanged)
            {
                if (Levers::RenderGraphDiagnostics())
                {
                    OLO_CORE_TRACE("RenderGraph BuildFrameGraph stats: passes={}, reads={}, writes={}, derivedEdges={}, orderSensitiveResults={}",
                                   buildStats.PassesVisited,
                                   buildStats.DeclaredReads,
                                   buildStats.DeclaredWrites,
                                   buildStats.DerivedEdges,
                                   buildStats.OrderSensitiveResults);
                }
                s_LastBuildStats = buildStats;
                s_HasLastBuildStats = true;
            }
        }

        bool validateCompiledHazards = Levers::RenderGraphDiagnostics();
#if !defined(OLO_DIST)
        validateCompiledHazards = validateCompiledHazards || buildStatsChanged;
#endif

        if (validateCompiledHazards)
        {
            const auto compiledHazards = s_Data.RGraph->ValidateCompiledResourceHazards();
            if (!compiledHazards.empty())
            {
                OLO_CORE_ERROR("Renderer3D::EndScene: compiled RenderGraph validation found {} resource hazards — see previous log entries for details.",
                               compiledHazards.size());
                OLO_CORE_ASSERT(compiledHazards.empty(), "Compiled RenderGraph resource hazard detected (see log). Fix the offending setup-time resource declarations or ordering edges.");
            }
            else if (Levers::RenderGraphDiagnostics() && buildStatsChanged)
            {
                OLO_CORE_TRACE("Renderer3D::EndScene: compiled RenderGraph validation passed.");
            }
            else
            {
                // No additional handling required.
            }
        }

        s_Data.RGraph->Execute();

        // Rebuild the persistent Hi-Z occlusion pyramid (#431) from this frame's
        // final scene depth and retain it for next frame's GPU instance cull.
        // Runs after Execute() so geometry depth is complete; no-op when HZB
        // occlusion is disabled.
        GenerateOcclusionHZB();

        // Central frame-capture commit (issue #463 / #316 Part 4). The whole render
        // graph has now executed, so every command-bucket pass (Scene, Water,
        // Foliage, Decal, ForwardOverlay) has accumulated its own per-pass bucket
        // into the pending capture. Commit it here — relocated out of
        // SceneRenderPass::OnFrameEnd, which used to commit mid-graph (before the
        // other passes ran) and thus only ever captured the scene pass's bucket.
        // No-op when not capturing.
        FrameCaptureManager::GetInstance().CommitFrame();

        // End occlusion query frame after render graph execution
        if (s_Data.OcclusionCullingEnabled)
        {
            OcclusionQueryPool::GetInstance().EndFrame();
        }

        // Store current VP as previous for next frame's motion blur
        s_Data.PrevViewProjectionMatrix = s_Data.ViewProjectionMatrix;
        // The culling camera's own history rotates only while unfrozen, so it
        // keeps describing the pyramid GenerateOcclusionHZB() last produced
        // (issue #726). Two matrices because they answer different questions:
        // PrevViewProjectionMatrix is "where was the camera last frame" (motion
        // vectors), CullPrevViewProjectionMatrix is "what VP does the bound
        // depth pyramid correspond to".
        if (!s_Data.CullingCameraFrozen)
            s_Data.CullPrevViewProjectionMatrix = s_Data.CullViewProjectionMatrix;

        // Don't return the allocator to the pool - it's managed by FrameResourceManager
        // The allocator will be reset at the start of the next frame when this buffer is reused
        const auto clearRenderStreamAllocator = [](CommandBufferRenderPass* node)
        {
            if (node)
                node->SetCommandAllocator(nullptr);
        };
        pipeline.ForEachRenderStreamNode(clearRenderStreamAllocator);

        // Close the GPU readback-stats frame (issue #721): barrier, copy the live
        // stats block into the next ring slot, fence it. Here — after the graph,
        // the HZB rebuild and the capture commit — because everything above can
        // publish a counter, and a copy issued earlier would silently omit
        // whatever ran after it. That omission is invisible: the number is still
        // a plausible number, just a smaller one.
        GPUReadbackStats::EndFrame();

        // Stamp the whole-frame GPU end timestamp after all of this frame's GPU
        // work has been submitted (graph execute, HZB rebuild, capture commit).
        GPUPassTimerPool::GetInstance().EndFrame();

        RendererProfiler::GetInstance().EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }
} // namespace OloEngine
