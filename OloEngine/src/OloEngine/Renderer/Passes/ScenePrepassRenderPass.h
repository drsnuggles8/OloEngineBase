#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class SceneRenderPass;

    // @brief The forward depth prepass as its OWN graph node (issue #1452).
    //
    // Screen-space AO is visibility for the ambient term. For a forward shader
    // to apply it there, the AO buffer has to exist before forward colour runs
    // — so the prepass that fills depth and the view normals the AO passes read
    // runs first, as this node, then SSAO / GTAO / the sphere proxies, then
    // ScenePass's colour sub-pass. The depth and normals it exports are what the
    // AO nodes registered after it read; ScenePass exports both again after
    // colour for the consumers that come later (water, SSR, the overlays).
    //
    // It owns no bucket and no framebuffer: the geometry is ScenePass's bucket,
    // rendered into ScenePass's target, through SceneRenderPass's own entry
    // point. Registered on the forward paths only. The deferred path keeps its
    // prepass inside ScenePass, because its AO reads the finished G-Buffer.
    class ScenePrepassRenderPass : public RenderGraphNode
    {
      public:
        explicit ScenePrepassRenderPass(SceneRenderPass* scene);
        ~ScenePrepassRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

      private:
        SceneRenderPass* m_Scene = nullptr;
    };
} // namespace OloEngine
