#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ShaderDebugDrawPass.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <array>
#include <span>

namespace OloEngine
{
    ShaderDebugDrawPass::ShaderDebugDrawPass()
    {
        SetName("ShaderDebugDrawPass");
        OLO_CORE_INFO("Creating ShaderDebugDrawPass.");
    }

    void ShaderDebugDrawPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // Declare nothing while off — this is what makes the disabled path free
        // at the graph level as well as the shader level. The enable is hashed
        // into the blackboard fingerprint, so flipping it rebuilds rather than
        // reusing a cached build that has us undeclared (issue #530 rule).
        if (!m_Enabled || !blackboard.Scene.SceneColor.IsValid())
            return;

        // Inter-pass RMW on the scene framebuffer, same shape as
        // ForwardOverlayRenderPass: read the prior SceneColor version, then
        // advertise a renamed output so the validator does not see a same-pass
        // feedback loop and downstream name-based readers trace back to us.
        // DependsOnPreviousWriter pins us after the last SceneColor writer, which
        // is what puts the debug geometry on top of the finished scene.
        SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        [[maybe_unused]] const auto sceneColorRead =
            builder.Read(blackboard.Scene.SceneColor, RGReadUsage::RenderTargetRead);
        constexpr std::string_view debugDrawVersionTag = "ShaderDebugDrawPass";
        [[maybe_unused]] const auto sceneColorNew =
            builder.WriteNewVersion(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget, debugDrawVersionTag);
        builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
    }

    void ShaderDebugDrawPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Shader = Shader::Create("assets/shaders/DebugDrawPrimitives.glsl");
        m_EmptyVertexArray = VertexArray::Create();

        OLO_CORE_INFO("ShaderDebugDrawPass: Initialized.");
    }

    bool ShaderDebugDrawPass::IsReadyForExecution() const noexcept
    {
        return m_Shader && m_Shader->IsReady() && m_EmptyVertexArray;
    }

    void ShaderDebugDrawPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Enabled || !IsReadyForExecution() || !ShaderDebugDraw::IsEnabled())
            return;

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }
        if (!m_SceneFramebuffer)
            return;

        // Restore rather than Ignore: this pass publishes no engine-global
        // binding (the render-pass-published-state rule), so a restore cannot
        // revert a publication, and it is the cheapest net for a debug pass that
        // touches a lot of state. It also FOUND two real leaks during bring-up
        // (a left-behind GL_LEQUAL depth func and cull-face disabled) — both now
        // restored explicitly at the end, so the only diffs the guard still
        // reports are the deliberate program/VAO unbinds below.
        GLStateGuard guard("ShaderDebugDrawPass", GLStateGuard::Policy::Restore);

        // The channels were written by shader atomics (SHADER_STORAGE) and are
        // about to be read as draw arguments (COMMAND) and as SSBO entry data
        // (SHADER_STORAGE again, from the vertex stage). Both bits are required:
        // without COMMAND the indirect draw can legally read a stale instance
        // count, which presents as "the first frame after a push draws nothing"
        // and then works, i.e. the most confusing possible symptom.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        const RHI::ResourceHandle sceneFBID = m_SceneFramebuffer->GetRHIHandle();

        u32 sceneColorAttachmentCount = 0;
        for (const auto& att : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++sceneColorAttachmentCount;
        }

        // Bind() also sets the viewport, honouring the DRS render-viewport
        // override. Setting it ourselves from the spec would render at the full
        // physical size while the rest of the frame renders reduced.
        m_SceneFramebuffer->Bind();

        // Narrow to colour[0]. The debug fragment stage has no entity-ID,
        // view-normal or velocity output, and leaving those attachments selected
        // would write undefined values over what the geometry passes produced —
        // breaking editor picking and SSAO normals wherever a debug line crosses.
        constexpr u32 colorAttachment = 0;
        RenderCommand::SetFramebufferDrawAttachments(sceneFBID, std::span<const u32>(&colorAttachment, 1));

        // Depth TEST against the scene, depth WRITE off. Testing is what makes a
        // world-space bound read as being in the scene rather than pasted over
        // it; not writing keeps the debug geometry out of every downstream
        // depth-consuming pass (AO, SSR, fog) — a debug overlay must not change
        // what the frame computes.
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetDepthFunc(RHI::CompareOp::LessOrEqual);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);

        m_Shader->Bind();
        m_EmptyVertexArray->Bind();

        // The quad expansion works in PIXELS, so it needs the size actually being
        // rendered — under dynamic resolution that is the render-viewport
        // override, not the framebuffer's physical size. Getting this wrong scales
        // line width by the DRS factor, which reads as "the lines get thinner when
        // the scene gets busy" and is easy to mistake for an LOD effect.
        const u32 viewportWidth =
            m_SceneFramebuffer->GetRenderViewportWidth() > 0 ? m_SceneFramebuffer->GetRenderViewportWidth()
                                                             : sceneSpec.Width;
        const u32 viewportHeight =
            m_SceneFramebuffer->GetRenderViewportHeight() > 0 ? m_SceneFramebuffer->GetRenderViewportHeight()
                                                              : sceneSpec.Height;
        const glm::vec2 viewportSize(static_cast<f32>(viewportWidth), static_cast<f32>(viewportHeight));

        constexpr auto primitives = std::array{
            ShaderDebugDrawPrimitive::Line,
            ShaderDebugDrawPrimitive::Circle,
            ShaderDebugDrawPrimitive::Rectangle,
            ShaderDebugDrawPrimitive::AABB,
            ShaderDebugDrawPrimitive::Box,
            ShaderDebugDrawPrimitive::Cone,
            ShaderDebugDrawPrimitive::Sphere,
        };
        static_assert(primitives.size() == kShaderDebugDrawPrimitiveCount);

        for (const auto primitive : primitives)
        {
            const auto channel = ShaderDebugDraw::GetChannelBuffer(primitive);
            if (!channel)
                continue;

            // One indirect draw per channel. The instance count inside the
            // command was produced on the GPU; issuing the draw unconditionally
            // (rather than skipping an "empty" channel) is deliberate — the CPU
            // cannot know whether a compute shader appended to it this frame, and
            // an indirect draw with instanceCount 0 costs nothing.
            ShaderDebugDraw::UploadDrawParams(m_ViewProjection, m_ObserverInvViewProjection, viewportSize, primitive);
            channel->Bind();
            RenderCommand::DrawArraysIndirect(m_EmptyVertexArray, channel->GetRHIHandle());
        }

        // Stage this frame's headers for the NEXT frame's BeginFrame() to read.
        // GPU->GPU copy into DeviceToHost buffers; see ShaderDebugDraw.cpp for
        // why the channels themselves must never be read directly.
        ShaderDebugDraw::StageStatsForReadback();

        if (sceneColorAttachmentCount > 0)
            RenderCommand::RestoreAllFramebufferDrawAttachments(sceneFBID, sceneColorAttachmentCount);

        // Put back depth mask + depth func + blend + cull + polygon mode in one
        // canonical call. The GLStateGuard above would restore them anyway, but
        // relying on it means the pass genuinely leaks GL_LEQUAL and
        // cull-disabled in any build where the guard is downgraded to Ignore —
        // and those leaks change what a LATER pass renders (coplanar geometry
        // stops z-fighting, backfaces reappear), which reads as an unrelated
        // regression. Verified against the guard's own diff: with this call the
        // only state it still reports are the deliberate program/VAO unbinds.
        context.ResetOpaqueForwardDrawState();
        // ResetOpaqueForwardDrawState restores the cull MODE (glCullFace) but
        // never re-enables the CAPABILITY, so on its own it does not undo the
        // DisableCulling() above — the GLStateGuard kept reporting
        // `CullFace: true -> false` escaping this pass, which is how this was
        // caught. Culling has to be off for the draw itself: a segment's quad
        // winding depends on the direction the segment happens to run.
        RenderCommand::EnableCulling();
        m_SceneFramebuffer->Unbind();
        RenderCommand::BindVertexArrayRaw(RHI::NullResource);
        RenderCommand::BindShaderProgram(RHI::NullResource);
        CommandDispatch::InvalidateRenderStateCache();

        // m_Target is deliberately NOT set to the scene framebuffer (mirrors
        // ForwardOverlayRenderPass): the inherited ApplyRenderViewport() resizes
        // m_Target on a DRS change, and this pass does not own the scene FB —
        // letting it drive that resize would give the framebuffer two owners.
    }

    Ref<Framebuffer> ShaderDebugDrawPass::GetTarget() const
    {
        return m_SceneFramebuffer;
    }

    void ShaderDebugDrawPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ShaderDebugDrawPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ShaderDebugDrawPass::OnReset()
    {
        m_SceneFramebuffer.Reset();
    }
} // namespace OloEngine
