#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class GPUScene;

    namespace RayTracing
    {
        class RayTracingScene;
    }

    // @brief Acceleration-structure build node (issue #978) — the first node in
    // the frame, and the owner of the AS-build -> AS-read hazard.
    //
    // WHY IT IS A GRAPH NODE AT ALL. The builds could be recorded inline in
    // Renderer3D::EndScene, and that would even be correct today. They are a
    // node because the hazard has to be a fact the graph knows: every future
    // ray-query consumer (shadows, reflections, GI) declares a read against
    // this node, and an edge the graph can see is an edge that survives someone
    // reordering the pipeline. A hand-placed barrier in EndScene is invisible
    // to the graph and silently wrong the first time a pass moves.
    //
    // It also gets per-pass GPU timing for free: RenderGraphPlanExecutor
    // brackets every node with GPUPassTimerPool, so "AS build ms" is reported
    // by the same channel as every other pass rather than a bespoke timer.
    //
    // NeverCull, for the reason VirtualShadowMapMarkPass documents: its output
    // is an acceleration structure the graph's resource model cannot represent,
    // so reachability analysis from the final pass sees a node nobody reads and
    // prunes it. The symptom would be a permanently empty TLAS with no error
    // anywhere.
    //
    // Registered unconditionally and self-disabling in Execute, NOT gated in
    // IsEnabled(): the graph caches its topology behind a fingerprint, so a
    // node whose enablement tracks a runtime capability stays culled for the
    // whole session. On a machine with no RT device Execute costs one
    // predicate.
    class RayTracingScenePass : public RenderGraphNode
    {
      public:
        RayTracingScenePass();
        ~RayTracingScenePass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

        // Wired at renderer init. Both are borrowed, never owned.
        void SetRayTracingScene(RayTracing::RayTracingScene* scene) noexcept
        {
            m_Scene = scene;
        }
        void SetGPUScene(const GPUScene* gpuScene) noexcept
        {
            m_GPUScene = gpuScene;
        }

        // TRUE whenever both sources are attached — deliberately NOT "is ray
        // tracing available". See the class comment.
        [[nodiscard]] bool IsEnabled() const noexcept override;

      private:
        RayTracing::RayTracingScene* m_Scene = nullptr;
        const GPUScene* m_GPUScene = nullptr;
    };
} // namespace OloEngine
