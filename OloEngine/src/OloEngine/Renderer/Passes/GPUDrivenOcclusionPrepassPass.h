#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class GPUDrivenOcclusionPass;

    // @brief The GPU-driven instanced batches' share of the forward prepass
    // (issue #1452).
    //
    // With screen-space AO live on Forward / Forward+, the AO passes run between
    // the depth prepass and forward colour, and read the depth and view normals
    // the prepass left. GPUDrivenOcclusionPass draws its instanced survivors
    // AFTER ScenePass, so without this node they would be missing from the AO
    // input and would sample the occlusion of whatever lies behind them. This
    // node runs both of that pass's phases depth + view-normal only, ahead of the
    // AO passes — building the phase-2 Hi-Z from a complete depth buffer as it
    // goes — and GPUDrivenOcclusionPass then replays the same draws in colour at
    // GL_LEQUAL. Without a forward AO buffer it does nothing and the pass runs as
    // it always did.
    class GPUDrivenOcclusionPrepassPass : public RenderGraphNode
    {
      public:
        explicit GPUDrivenOcclusionPrepassPass(GPUDrivenOcclusionPass* occlusion);
        ~GPUDrivenOcclusionPrepassPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

      private:
        GPUDrivenOcclusionPass* m_Occlusion = nullptr;
    };
} // namespace OloEngine
