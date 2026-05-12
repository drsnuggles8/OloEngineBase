#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    // @brief Forward overlay pass for geometry that does NOT participate in the
    // deferred G-Buffer write (skybox, terrain, voxel terrain, infinite grid,
    // debug light cubes). Only active in RenderingPath::Deferred; in Forward /
    // Forward+ these commands are routed to SceneRenderPass as usual and this
    // pass executes with an empty bucket (no-op).
    //
    // The pass binds the scene framebuffer (already populated with lit HDR
    // colour by DeferredLightingPass) with GL_COLOR_ATTACHMENT0 selected and
    // runs the forward shaders each command carries. G-Buffer depth is
    // expected to have been blitted into the scene FB's depth attachment by
    // DeferredLightingPass::Execute() immediately before this pass runs, so
    // depth-test against deferred geometry works naturally.
    //
    // Renders AFTER DeferredLightingPass and BEFORE FoliagePass so that
    // skybox/terrain still sit beneath translucent foliage/water.
    class ForwardOverlayRenderPass : public CommandBufferRenderPass
    {
      public:
        ForwardOverlayRenderPass();
        ~ForwardOverlayRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::Mixed;
        }
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
    };
} // namespace OloEngine
