#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class GBuffer;

    // Resolve-before-lighting resolves before the single-sample late writers;
    // per-sample lighting resolves after the last multisample writer (#1553).
    class GBufferResolvePass : public RenderGraphNode
    {
      public:
        explicit GBufferResolvePass(bool afterLateWriters);
        ~GBufferResolvePass() override;
        void Setup(RGBuilder& builder, FrameBlackboard& board) override;
        void Execute(RGCommandContext& context) override;
        void AppendDeclarationInputs(RGDeclarationKey& key) const override;
        void SetGBuffer(const Ref<GBuffer>& buffer);
        void SetPerSampleLighting(bool enabled)
        {
            m_PerSampleLighting = enabled;
        }

      private:
        Ref<GBuffer> m_GBuffer;
        bool m_AfterLateWriters = false;
        bool m_PerSampleLighting = false;
        bool m_Resolve = false;
    };
} // namespace OloEngine
