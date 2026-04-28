#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <glad/gl.h>

namespace OloEngine
{
    void RGCommandContext::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
    {
        RenderCommand::SetViewport(x, y, width, height);
    }

    void RGCommandContext::SetClearColor(const glm::vec4& color)
    {
        RenderCommand::SetClearColor(color);
    }

    void RGCommandContext::Clear()
    {
        RenderCommand::Clear();
    }

    void RGCommandContext::ResetGraphicsStateToDefault()
    {
        // Keep in sync with the engine's default render-state contract.
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableCulling();
        RenderCommand::SetCullFace(GL_BACK);
        RenderCommand::SetLineWidth(1.0f);
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::DisableScissorTest();
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetPolygonOffset(0.0f, 0.0f);
        RenderCommand::EnableMultisampling();
    }

    void RGCommandContext::BindDefaultFramebuffer()
    {
        RenderCommand::BindDefaultFramebuffer();
    }

    void RGCommandContext::SetDepthTest(const bool enabled)
    {
        RenderCommand::SetDepthTest(enabled);
    }

    void RGCommandContext::SetDepthMask(const bool enabled)
    {
        RenderCommand::SetDepthMask(enabled);
    }

    void RGCommandContext::SetBlendState(const bool enabled)
    {
        RenderCommand::SetBlendState(enabled);
    }

    void RGCommandContext::SetAlphaBlendStandard()
    {
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void RGCommandContext::SetOpaqueReplaceBlend()
    {
        RenderCommand::SetBlendFunc(GL_ONE, GL_ZERO);
    }

    void RGCommandContext::SetCulling(const bool enabled)
    {
        if (enabled)
            RenderCommand::EnableCulling();
        else
            RenderCommand::DisableCulling();
    }

    void RGCommandContext::SetDrawBuffers(const std::span<const u32> attachments)
    {
        RenderCommand::SetDrawBuffers(attachments);
    }

    void RGCommandContext::BindTexture(const u32 slot, const u32 textureID)
    {
        RenderCommand::BindTexture(slot, textureID);
    }

    void RGCommandContext::MemoryBarrier(const MemoryBarrierFlags flags)
    {
        if (flags == MemoryBarrierFlags::None)
            return;
        RenderCommand::MemoryBarrier(flags);
    }

    void RGCommandContext::DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount)
    {
        RenderCommand::DrawIndexed(vertexArray, indexCount);
    }

    u32 RGCommandContext::ResolveTexture(const RGTextureHandle handle) const
    {
        if (!m_RenderGraph)
            return 0;

        return m_RenderGraph->ResolveTexture(handle);
    }

    Ref<Framebuffer> RGCommandContext::ResolveFramebuffer(const RGFramebufferHandle handle) const
    {
        if (!m_RenderGraph)
            return nullptr;

        return m_RenderGraph->ResolveFramebuffer(handle);
    }
} // namespace OloEngine
