#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Core/PerformanceProfiler.h"
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

        RendererProfiler::GetInstance().EndFrame();

        // End frame for double-buffered resources (inserts GPU fence)
        FrameResourceManager::Get().EndFrame();
    }
} // namespace OloEngine
