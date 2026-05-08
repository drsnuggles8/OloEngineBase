#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"

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

    void Renderer3D::EndScene()
    {
        OLO_PROFILE_FUNCTION();
        auto& pipeline = *s_Data.Pipeline;

        if (!s_Data.RGraph)
        {
            OLO_CORE_ERROR("Renderer3D::EndScene: Render graph is null!");
            return;
        }

        pipeline.ConfigurePassesForFrame(s_Data);

        // Populate the graph blackboard AFTER per-frame pass configuration so
        // AOBuffer / PostProcessColor imports resolve the current frame's
        // active technique and enabled outputs rather than last frame's state.
        pipeline.PopulateBlackboard(s_Data);

        pipeline.UploadExecutionState(s_Data);

        // Phase C: compile graph-native pass declarations before execution.
        s_Data.RGraph->BuildFrameGraph();

        {
            const auto& buildStats = s_Data.RGraph->GetLastBuildStats();
            static RenderGraph::FrameBuildStats s_LastBuildStats{};
            static bool s_HasLastBuildStats = false;

            const bool changed = !s_HasLastBuildStats ||
                                 buildStats.PassesVisited != s_LastBuildStats.PassesVisited ||
                                 buildStats.DeclaredReads != s_LastBuildStats.DeclaredReads ||
                                 buildStats.DeclaredWrites != s_LastBuildStats.DeclaredWrites ||
                                 buildStats.DerivedEdges != s_LastBuildStats.DerivedEdges;

            if (changed)
            {
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_TRACE("RenderGraph BuildFrameGraph stats: passes={}, reads={}, writes={}, derivedEdges={}",
                                   buildStats.PassesVisited,
                                   buildStats.DeclaredReads,
                                   buildStats.DeclaredWrites,
                                   buildStats.DerivedEdges);
                }
                s_LastBuildStats = buildStats;
                s_HasLastBuildStats = true;
            }
        }

        // Phase D Slice 1: after BuildFrameGraph, stable handles for transient resources
        // are assigned. Populate the blackboard so Execute callbacks can resolve them.
        pipeline.RefreshBlackboardHandles(s_Data);
        s_Data.RGraph->Execute();

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
        pipeline.StreamNodes.ForEach(clearRenderStreamAllocator);

        RendererProfiler::GetInstance().EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }
} // namespace OloEngine
