#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class WaterRenderPass;

    // Replays the real displaced water geometry into independent depth before
    // groom draws. Water colour stays in the transparent band after groom.
    class GroomWaterDepthPass : public RenderGraphNode
    {
      public:
        explicit GroomWaterDepthPass(WaterRenderPass* water);
        void Setup(RGBuilder& builder, FrameBlackboard& board) override;
        void Execute(RGCommandContext& context) override;
        void AppendDeclarationInputs(RGDeclarationKey& key) const override;

      private:
        WaterRenderPass* m_Water;
        RGFramebufferHandle m_Target{};
        RGFramebufferHandle m_Scene{};
    };
} // namespace OloEngine
