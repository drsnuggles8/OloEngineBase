#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // @brief Weighted-blended OIT composite pass.
    //
    // Runs after weighted-blended contributors (particles and forward
    // transparent decals) have accumulated into the graph-owned `OITBuffer`
    // transient. Samples the accumulation and revealage attachments and
    // composites the resolved transparent colour over the scene FB in a
    // single fullscreen draw.
    //
    // Passthrough semantics: when `Enabled` is false (`OITEnabled` in
    // `RendererSettings::DeferredSettings`) the pass no-ops and
    // `GetTarget()` returns the input framebuffer so downstream stages
    // (SSS, post-process) see the unmodified scene colour. Transparent
    // passes also branch on the same flag and fall back to their classic
    // alpha-blend path.
    //
    class OITResolveRenderPass : public RenderGraphNode
    {
      public:
        OITResolveRenderPass();
        ~OITResolveRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
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

        void SetHasContributors(bool hasContributors) noexcept
        {
            m_HasContributors = hasContributors;
        }

      private:
        void DrawFullscreenTriangle(RGCommandContext& context);

        Ref<Shader> m_ResolveShader;
        RGTextureHandle m_SelectedOITAccumTexture{};
        RGTextureHandle m_SelectedOITRevealageTexture{};

        bool m_Enabled = false;
        bool m_HasContributors = false;
    };
} // namespace OloEngine
