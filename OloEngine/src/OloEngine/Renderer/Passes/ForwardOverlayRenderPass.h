#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    // @brief Forward overlay pass for draws the deferred G-Buffer cannot
    // represent: the infinite grid, alpha-blended or transmissive meshes,
    // meshes whose material shader has no G-Buffer output, see-through debug
    // draws, and the fallback for skybox, light-cube, terrain and voxel draws
    // whose *_GBuffer shader failed to load (Renderer3DMeshSubmission.cpp,
    // Renderer3DUtilityDraws.cpp). Registered in the graph only on
    // RenderingPath::Deferred; on Forward and Forward+ the same draws go to
    // SceneRenderPass.
    //
    // The pass binds the scene framebuffer (already populated with lit HDR
    // colour by DeferredLightingPass) with colour attachments 0-2 (colour,
    // entity ID, view normal) selected; velocity (3) is not written. It runs
    // the forward shaders each command carries. G-Buffer depth is
    // expected to have been blitted into the scene FB's depth attachment by
    // DeferredLightingPass::Execute() immediately before this pass runs, so
    // depth-test against deferred geometry works naturally.
    //
    // Renders after DeferredLightingPass and before FoliagePass, GroomPass,
    // DecalPass and WaterPass in the SceneColor read-modify-write chain
    // (RenderPipelineBuilderScene.cpp).
    class ForwardOverlayRenderPass : public CommandBufferRenderPass
    {
      public:
        ForwardOverlayRenderPass();
        ~ForwardOverlayRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;

        // Setup() declares nothing without a submitted draw (#1315), so that is
        // a declaration input.
        // The path it also gates on is in FrameGraphDeclarationConfig.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(HasSubmittedCommands());
        }
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void ReleaseStaleFramebuffers(const std::function<bool(const Framebuffer*)>& isStale) override
        {
            RenderGraphNode::ReleaseStaleFramebuffers(isStale);
            ReleaseIfStale(m_SceneFramebuffer, isStale);
        }
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
    };
} // namespace OloEngine
