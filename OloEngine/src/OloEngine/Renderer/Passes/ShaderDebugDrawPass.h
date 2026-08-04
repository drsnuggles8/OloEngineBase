#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Shader;
    class VertexArray;

    // @brief Draws the GPU-pushable shader debug-draw channels (issue #725).
    //
    // Seven indirect draws, one per primitive channel, expanding each appended
    // entry into screen-space line quads (`assets/shaders/DebugDrawPrimitives.glsl`).
    // The instance count of every draw was written on the GPU by the same atomic
    // that appended the entry, so this pass never learns how many primitives it
    // is about to draw — which is the whole point: a compute shader's decisions
    // become visible in the same frame without a readback.
    //
    // PLACEMENT. Registered as the LAST SceneColor writer — after OITResolve,
    // before SSS/AOApply (see the comment at the registration site in
    // RenderPipelineBuilderTransparency.cpp for why both halves of that matter).
    // Consequences:
    //   * every pass that might PUSH (cluster cull, DDGI, particle/fluid computes,
    //     any G-Buffer or forward fragment stage) has already run;
    //   * the scene framebuffer's depth attachment is fully populated, so
    //     world-space debug geometry depth-tests against real scene geometry
    //     rather than floating over it;
    //   * the lines are pre-tonemap, matching every other debug overlay in the
    //     engine (the CPU gizmos routed through ScenePass / ForwardOverlayPass
    //     are tonemapped too), so an author comparing a CPU bound against its
    //     GPU counterpart sees the two in the same colour space.
    //
    // The pass declares NOTHING when disabled — no resource, no version, no
    // dependency — and `RendererSettings::ShaderDebugDrawEnabled` is hashed into
    // the blackboard fingerprint so flipping it rebuilds the graph (the #530
    // rule: an enable that gates a declaration must invalidate the cache).
    class ShaderDebugDrawPass : public RenderGraphNode
    {
      public:
        ShaderDebugDrawPass();
        ~ShaderDebugDrawPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
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
        [[nodiscard]] bool IsReadyForExecution() const noexcept override;

        // Per-frame camera state, pushed by RenderPipeline::UploadExecutionState.
        // `observerInvViewProjection` is the inverse view-projection that gives
        // meaning to ShaderDebugDrawSpace::ObserverCameraNDC; until the observer
        // camera lands (issue #726) the pipeline passes the MAIN camera's, which
        // makes that space round-trip to MainCameraNDC instead of producing
        // garbage.
        void SetCameraState(const glm::mat4& viewProjection, const glm::mat4& observerInvViewProjection) noexcept
        {
            m_ViewProjection = viewProjection;
            m_ObserverInvViewProjection = observerInvViewProjection;
        }

      private:
        bool m_Enabled = false;
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<Shader> m_Shader;
        // An attribute-less VAO. `glDrawArraysIndirect` requires a bound vertex
        // array even when every vertex is synthesised from gl_VertexIndex, and
        // borrowing one that HAS enabled attributes (the fullscreen triangle,
        // say) would have the hardware fetch far past the end of its 3-vertex
        // buffer for our 576-vertex sphere draws.
        Ref<VertexArray> m_EmptyVertexArray;
        glm::mat4 m_ViewProjection{ 1.0f };
        glm::mat4 m_ObserverInvViewProjection{ 1.0f };
    };
} // namespace OloEngine
