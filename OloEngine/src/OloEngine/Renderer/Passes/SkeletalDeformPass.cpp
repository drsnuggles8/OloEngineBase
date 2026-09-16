#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SkeletalDeformPass.h"

#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RayTracing/DeformedSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"

namespace OloEngine
{
    SkeletalDeformPass::SkeletalDeformPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("SkeletalDeformPass");
        // The output is a set of vertex buffers the graph's resource model has
        // no kind for, so reachability from the final pass would prune this
        // node. Same flag and same reason as RayTracingScenePass.
        SetSideEffects(SideEffect::NeverCull);
        SetPassWorkType(RenderGraphPassWorkType::Compute);
    }

    bool SkeletalDeformPass::IsEnabled() const noexcept
    {
        // Not "is ray tracing available" and not "is there work this frame":
        // the graph caches its topology behind a fingerprint that includes
        // neither, so a node gated on either would stay culled for the rest of
        // the session the first time the answer was no. The cheap per-frame
        // answers live in Execute.
        return m_Cache != nullptr && m_Scene != nullptr;
    }

    void SkeletalDeformPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        // Nothing is declared as a graph read or write. The rest vertex stream,
        // the bone influences, the palette buffer and the deformed output are
        // all reached by DEVICE ADDRESS rather than through the graph, so the
        // graph has no handle to express the dependency with. What it does
        // express is the execution edge: RayTracingScenePass declares
        // DependsOnPass("SkeletalDeformPass"), and the memory hazard between
        // them is the explicit barrier Execute emits below.
    }

    void SkeletalDeformPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        static_cast<void>(context);
        if (m_Cache == nullptr || m_Scene == nullptr)
        {
            return;
        }
        // Self-disabling here, not in IsEnabled(): see the header. On a machine
        // without a ray-tracing device the cache is never enabled and this is
        // one predicate and a return.
        if (!m_Cache->IsEnabled() || !m_Scene->IsAvailable())
        {
            return;
        }
        // An idle scene genuinely has nothing to do: a surface whose pose did
        // not advance is not re-deformed. Skipping the whole bracket rather
        // than dispatching zero times keeps the timer honest — a frame that
        // deformed nothing reports no deformation cost instead of the cost of
        // asking.
        if (!m_Cache->HasWork())
        {
            return;
        }

        auto& gpuTimers = GPUPassTimerPool::GetInstance();
        gpuTimers.BeginSubPass("SkeletalDeformToBuffer");
        const u32 dispatched = m_Cache->Dispatch();
        gpuTimers.EndSubPass();

        if (dispatched == 0u)
        {
            // Every queued surface was refused inside Dispatch (no shader, no
            // palette address). The cache has already counted and logged it;
            // emitting the barrier anyway would be harmless but dishonest — it
            // would claim a write that never happened.
            return;
        }

        // THE HAZARD. The next node builds acceleration structures out of the
        // buffers these dispatches just wrote, and a build that runs before the
        // writes land holds whatever the memory contained — with no validation
        // message and no error, because the API usage is entirely legal.
        //
        // Emitted here rather than inside RayTracingScenePass so that the
        // producer owns the barrier for its own writes, which is the same rule
        // that puts the AS-build -> AS-read barrier in the node that builds.
        m_Scene->RecordDeformToBuildBarrier();
    }
} // namespace OloEngine
