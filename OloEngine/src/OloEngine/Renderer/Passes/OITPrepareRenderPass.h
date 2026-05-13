#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    // @brief Prepares the graph-owned weighted-blended OIT target for the frame.
    //
    // Clears the accumulation / revealage attachments and seeds the OIT depth
    // attachment from the current scene framebuffer so transparent contributors
    // can render into a fully graph-owned transient target.
    class OITPrepareRenderPass : public RenderGraphNode
    {
      public:
        OITPrepareRenderPass();
        ~OITPrepareRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        void SetHasContributors(bool hasContributors) noexcept
        {
            m_HasContributors = hasContributors;
        }

      private:
        static void PrepareFramebuffer(const Ref<Framebuffer>& oitFramebuffer,
                                       const Ref<Framebuffer>& sceneFramebuffer);

        RGFramebufferHandle m_SelectedOITFramebuffer;
        bool m_Enabled = false;
        bool m_HasContributors = false;
    };
} // namespace OloEngine
