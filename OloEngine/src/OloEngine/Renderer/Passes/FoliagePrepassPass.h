#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class FoliageRenderPass;

    // @brief The foliage share of the forward prepass (issue #1474).
    //
    // With screen-space AO live on Forward / Forward+, the AO passes run between
    // the depth prepass and forward colour, and a surface takes AO only if the
    // prepass drew it (issue #1452). FoliageRenderPass draws after ScenePass, so
    // without this node the AO buffer at a leaf's pixels holds the occlusion of
    // the ground behind it, and forward foliage took no AO at all. This node
    // replays the foliage bucket depth + view normal (Foliage_*_DepthNormal.glsl)
    // after ScenePrepassPass and GPUDrivenOcclusionPrepassPass and before the AO
    // passes; FoliageRenderPass then draws the same leaves in colour at GL_LEQUAL
    // and applies the AO to their ambient term. Without a forward AO buffer it
    // does nothing and foliage draws as it always did.
    class FoliagePrepassPass : public RenderGraphNode
    {
      public:
        explicit FoliagePrepassPass(FoliageRenderPass* foliage);
        ~FoliagePrepassPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

      private:
        FoliageRenderPass* m_Foliage = nullptr;
    };
} // namespace OloEngine
