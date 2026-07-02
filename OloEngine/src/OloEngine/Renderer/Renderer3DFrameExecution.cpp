#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void Renderer3D::SetParticleRenderCallback(RenderCallback callback)
    {
        s_Data.PendingParticleRenderCallback = std::move(callback);
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
        u32 depthTexID = 0;
        if (const Ref<GBuffer>& gbuffer = scenePass->GetGBuffer())
        {
            depthTexID = gbuffer->GetDepthAttachmentID();
        }
        else if (Ref<Framebuffer> target = scenePass->GetTarget())
        {
            depthTexID = target->GetDepthAttachmentRendererID();
        }

        const auto& spec = scenePass->GetFramebufferSpecification();
        if (depthTexID == 0 || spec.Width == 0 || spec.Height == 0)
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

        s_Data.OcclusionHZB.Generate(depthTexID);
        // Valid from here on — next frame's instance cull may sample it.
        s_Data.OcclusionHZBValid = true;
    }

    GPUFrustumCuller::HZBOcclusionInputs Renderer3D::BuildCurrentOcclusionHZB(u32 depthTextureID, u32 width, u32 height)
    {
        GPUFrustumCuller::HZBOcclusionInputs inputs; // Enabled = false by default

        if (depthTextureID == 0 || width == 0 || height == 0)
            return inputs;

        // Rebuild the persistent pyramid from THIS frame's partial depth
        // (occluders + phase-1 survivors). This overwrites the previous-frame
        // pyramid, which phase 1 already consumed at submission; the tail-of-
        // EndScene GenerateOcclusionHZB() rebuilds it again from the final depth
        // for next frame's phase 1.
        s_Data.OcclusionHZB.Resize(width, height);
        if (!s_Data.OcclusionHZB.IsValid())
            return inputs;
        s_Data.OcclusionHZB.Generate(depthTextureID);

        inputs.Enabled = true;
        inputs.HZBTextureID = s_Data.OcclusionHZB.GetHZBTextureID();
        inputs.MipCount = s_Data.OcclusionHZB.GetMipCount();
        // Current-frame pyramid → reproject phase-2 bounds with the CURRENT VP.
        inputs.PrevViewProjection = s_Data.ViewProjectionMatrix;
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
                if (IsRenderGraphDiagnosticsEnabled())
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

        bool validateCompiledHazards = IsRenderGraphDiagnosticsEnabled();
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
            else if (IsRenderGraphDiagnosticsEnabled() && buildStatsChanged)
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

        // Don't return the allocator to the pool - it's managed by FrameResourceManager
        // The allocator will be reset at the start of the next frame when this buffer is reused
        const auto clearRenderStreamAllocator = [](CommandBufferRenderPass* node)
        {
            if (node)
                node->SetCommandAllocator(nullptr);
        };
        pipeline.ForEachRenderStreamNode(clearRenderStreamAllocator);

        // Stamp the whole-frame GPU end timestamp after all of this frame's GPU
        // work has been submitted (graph execute, HZB rebuild, capture commit).
        GPUPassTimerPool::GetInstance().EndFrame();

        RendererProfiler::GetInstance().EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }
} // namespace OloEngine
