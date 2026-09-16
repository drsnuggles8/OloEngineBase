#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/RayTracingScenePass.h"

#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"

namespace OloEngine
{
    RayTracingScenePass::RayTracingScenePass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("RayTracingScenePass");
        // The node's whole output is a VkAccelerationStructureKHR, which the
        // graph's resource model has no kind for — so backward reachability
        // from the final pass sees a node nobody reads and prunes it. Same
        // flag and same reason as VirtualShadowMapMarkPass.
        SetSideEffects(SideEffect::NeverCull);
        // Compute work, and an async candidate: nothing in the frame's
        // graphics work depends on it until a ray-query consumer declares a
        // read, so the hoister is free to move it earlier.
        SetPassWorkType(RenderGraphPassWorkType::Compute);
    }

    bool RayTracingScenePass::IsEnabled() const noexcept
    {
        // Deliberately NOT "is ray tracing available". The graph caches its
        // topology behind a frame fingerprint that does not include a device
        // capability, so a node gated on one would stay culled for the whole
        // session if the answer ever changed — and the symptom is a
        // permanently empty TLAS with nothing in the log.
        return m_Scene != nullptr && m_GPUScene != nullptr;
    }

    void RayTracingScenePass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        // The one edge this node declares (issue #1229). An animated surface's
        // BLAS is built from a vertex buffer SkeletalDeformPass wrote with a
        // compute dispatch earlier in this same command buffer; a build that
        // ran first would hold whatever that memory contained, with no error
        // and no validation message, because every API call involved is legal.
        //
        // By NAME, and declared here rather than left to registration order,
        // for the reason this node's own header gives for existing at all: an
        // ordering that holds because two AddNode calls happen to be adjacent
        // is invisible to the graph and silently wrong the first time someone
        // reorders the pipeline.
        builder.DependsOnPass("SkeletalDeformPass");
        // Nothing is declared as a graph read or write: the acceleration
        // structure is not a graph resource, and the vertex/index streams the
        // build consumes are reached by device address rather than through the
        // graph. The ORDERING that matters is expressed by NeverCull, by the
        // edge above, and by the node's position near the front of the frame;
        // the memory hazard by the explicit AS-build -> AS-read barrier Execute
        // emits. Every ray-query consumer declares an execution dependency on
        // this node by name, the same way this one declares its producer.
    }

    void RayTracingScenePass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        static_cast<void>(context);
        if (m_Scene == nullptr || m_GPUScene == nullptr)
        {
            return;
        }
        // Self-disabling here, not in IsEnabled(): see the header. On a
        // machine without a ray-tracing device this is one predicate and a
        // return.
        if (!m_Scene->IsAvailable())
        {
            return;
        }

        auto& gpuTimers = GPUPassTimerPool::GetInstance();
        gpuTimers.BeginSubPass("AccelerationStructureBuild");
        m_Scene->Update(*m_GPUScene);
        gpuTimers.EndSubPass();

        // The build -> read edge. Emitted here rather than by each consumer so
        // there is exactly one place that can get it wrong, and emitted even
        // when nothing was built this frame is cheap: the backend no-ops when
        // it has nothing outstanding.
        m_Scene->RecordBuildToReadBarrier();
    }
} // namespace OloEngine
