#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    namespace RayTracing
    {
        class DeformedSurfaceCache;
        class RayTracingScene;
    } // namespace RayTracing

    // @brief Skinning-to-memory node (issue #1229) — the frame's first node,
    // immediately ahead of RayTracingScenePass.
    //
    // WHAT IT DOES. Records one compute dispatch per animated surface whose
    // pose advanced this frame, writing that surface's skinned-and-morphed
    // vertices into its own buffer, and then emits the compute-write ->
    // AS-build-read barrier. RayTracingScenePass builds acceleration structures
    // out of exactly those buffers on the very next node.
    //
    // WHY IT IS A SEPARATE NODE rather than four lines at the top of
    // RayTracingScenePass::Execute. Two reasons, and the first is the
    // load-bearing one:
    //
    //  * The hazard is real and directional — a BLAS built before its vertices
    //    are written holds garbage, with no error anywhere — so it should be an
    //    edge the GRAPH knows rather than an ordering that happens to hold
    //    because two statements sit in one function. RayTracingScenePass
    //    declares DependsOnPass("SkeletalDeformPass") for that reason, which is
    //    the same argument RayTracingScenePass's own header makes for existing
    //    at all.
    //  * It gets its own GPUPassTimerPool bracket, so "deformation ms" and
    //    "AS build ms" are two numbers rather than one. Criterion 4 asks for
    //    build/refit timings, and a combined figure cannot answer whether a
    //    frame is expensive because a crowd is skinning or because a topology
    //    change forced rebuilds.
    //
    // NeverCull, for the reason RayTracingScenePass and VirtualShadowMapMarkPass
    // both document: the output is a set of buffers the graph's resource model
    // does not represent, so reachability analysis from the final pass sees a
    // node nobody reads and prunes it.
    //
    // Registered unconditionally and self-disabling in Execute, NOT gated in
    // IsEnabled(): the graph caches its topology behind a fingerprint that does
    // not include a device capability, so a node gated on one stays culled for
    // the whole session. On OpenGL, and on any device without ray tracing, the
    // cache is never enabled and this costs one predicate per frame.
    class SkeletalDeformPass : public RenderGraphNode
    {
      public:
        SkeletalDeformPass();
        ~SkeletalDeformPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

        // Wired at renderer init. Both are borrowed, never owned.
        void SetDeformedSurfaceCache(RayTracing::DeformedSurfaceCache* cache) noexcept
        {
            m_Cache = cache;
        }
        void SetRayTracingScene(RayTracing::RayTracingScene* scene) noexcept
        {
            m_Scene = scene;
        }

        // TRUE whenever both sources are attached — deliberately NOT "is ray
        // tracing available", and not "is there work". See the class comment.
        [[nodiscard]] bool IsEnabled() const noexcept override;

      private:
        RayTracing::DeformedSurfaceCache* m_Cache = nullptr;
        RayTracing::RayTracingScene* m_Scene = nullptr;
    };
} // namespace OloEngine
