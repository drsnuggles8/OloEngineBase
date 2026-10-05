#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class SceneRenderPass;

    // @brief On Deferred, the G-Buffer's velocity copied into SceneColor RT3
    // before the first forward pass draws (#1552).
    //
    // The temporal resolves and the post-process that run after every writer
    // -- TAA, motion blur, FSR2 and the depth/velocity upscale -- read
    // FrameBlackboard::Scene::SceneVelocity. On the forward paths that is
    // SceneColor RT3, which the opaque scene and every forward pass after it
    // write. On Deferred the opaque scene writes the G-Buffer's RT3 instead,
    // and the passes drawn on top of the lit frame -- the groom, foliage,
    // particles, water -- write SceneColor RT3, which no resolve read: the
    // Deferred TAA reprojected a walking coat by the velocity of whatever lay
    // behind it, and accumulated its stochastic coverage as if it stood still.
    // This node seeds SceneColor RT3 with the G-Buffer's velocity -- the same
    // RGBA16F layout, velocity + coverage + material profile (GBuffer.h) --
    // registered after the deferred lighting and before the forward overlay,
    // so each forward pass overwrites its own pixels and SceneVelocity holds
    // every surface's motion on both paths.
    //
    // On the forward paths SceneVelocity IS GBuffer.Velocity, and this node
    // declares nothing.
    class SceneVelocitySeedPass : public RenderGraphNode
    {
      public:
        explicit SceneVelocitySeedPass(SceneRenderPass* scene);
        ~SceneVelocitySeedPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

      private:
        SceneRenderPass* m_Scene = nullptr;
        RGTextureHandle m_Source{};
        RGTextureHandle m_Destination{};
    };
} // namespace OloEngine
