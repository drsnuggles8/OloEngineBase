#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    // @brief Prepares the graph-owned weighted-blended OIT target for the frame.
    //
    // Clears the accumulation / revealage attachments and seeds the OIT depth
    // attachment from the current scene framebuffer so transparent contributors
    // can render into a fully graph-owned transient target.
    class OITPrepareRenderPass : public RenderPass
    {
      public:
        OITPrepareRenderPass();
        ~OITPrepareRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

      private:
        static void PrepareFramebuffer(const Ref<Framebuffer>& oitFramebuffer,
                                       const Ref<Framebuffer>& sceneFramebuffer);

        bool m_Enabled = false;
    };
} // namespace OloEngine
