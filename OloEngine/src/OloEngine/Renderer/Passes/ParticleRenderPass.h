#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include <functional>

namespace OloEngine
{
    // @brief Render pass for transparent particle rendering.
    //
    // Executes between SceneRenderPass and (OITResolvePass → SSSPass → ...).
    //
    // Two code paths:
    //   - Classic (OIT off): renders into the ScenePass framebuffer with
    //     depth-test read-only and GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA
    //     alpha blending. Sort order is back-to-front via DrawKey.
    //   - Weighted-blended OIT (toggle via
    //     RendererSettings::OITEnabled): renders into the
    //     graph-owned OIT framebuffer with per-attachment blend funcs
    //     (accum: GL_ONE/GL_ONE, revealage: GL_ZERO/GL_ONE_MINUS_SRC_COLOR)
    //     and order-independent accumulation. OITResolvePass composites
    //     the result over the scene FB afterwards.
    class ParticleRenderPass : public RenderGraphNode
    {
      public:
        using RenderCallback = std::function<void()>;

        ParticleRenderPass();
        ~ParticleRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;

        // Setup() declares nothing without a render callback, which only a Scene
        // frame sets: a renderer-only frame would otherwise cache a culled node.
        // The depth read follows whether the last frame's callback asked for it.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(HasRenderCallback());
            key.Add(m_OITEnabled);
            key.Add(m_SceneDepthWanted);
        }
        void Init(const FramebufferSpecification& spec) override;
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

        void SetRenderCallback(RenderCallback callback);
        [[nodiscard]] bool HasRenderCallback() const noexcept
        {
            return static_cast<bool>(m_RenderCallback);
        }
        void SetOITEnabled(bool enabled) noexcept
        {
            m_OITEnabled = enabled;
        }

        // The depth the soft-particle fade samples, asked for by the render
        // callback while it draws. Particles draw into SceneColor, whose depth
        // SceneDepth is on the forward paths (#1332), so this is the depth
        // SNAPSHOT, and only on a frame whose Setup declared the read. Setup
        // declares it after a frame that asked, so the snapshot is made only
        // while a soft system draws; one turning soft fades in a frame late.
        [[nodiscard]] RHI::ResourceHandle AcquireSceneDepth() noexcept
        {
            m_SceneDepthAsked = true;
            return m_SceneDepthID;
        }

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
        RGFramebufferHandle m_SelectedOITFramebuffer;
        RGTextureHandle m_SelectedSceneDepth;
        RHI::ResourceHandle m_SceneDepthID = RHI::NullResource;
        RenderCallback m_RenderCallback;
        bool m_OITEnabled = false;
        bool m_SceneDepthWanted = false;
        bool m_SceneDepthAsked = false;
    };
} // namespace OloEngine
