#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <glad/gl.h>

namespace OloEngine
{
    void RGCommandContext::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height) const
    {
        RenderCommand::SetViewport(x, y, width, height);
    }

    void RGCommandContext::SetClearColor(const glm::vec4& color) const
    {
        RenderCommand::SetClearColor(color);
    }

    void RGCommandContext::Clear() const
    {
        RenderCommand::Clear();
    }

    void RGCommandContext::ResetGraphicsStateToDefault() const
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

    void RGCommandContext::BindDefaultFramebuffer() const
    {
        RenderCommand::BindDefaultFramebuffer();
    }

    void RGCommandContext::SetDepthTest(const bool enabled) const
    {
        RenderCommand::SetDepthTest(enabled);
    }

    void RGCommandContext::SetDepthMask(const bool enabled) const
    {
        RenderCommand::SetDepthMask(enabled);
    }

    void RGCommandContext::SetBlendState(const bool enabled) const
    {
        RenderCommand::SetBlendState(enabled);
    }

    void RGCommandContext::SetAlphaBlendStandard() const
    {
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void RGCommandContext::SetOpaqueReplaceBlend() const
    {
        RenderCommand::SetBlendFunc(GL_ONE, GL_ZERO);
    }

    void RGCommandContext::SetCulling(const bool enabled) const
    {
        if (enabled)
            RenderCommand::EnableCulling();
        else
            RenderCommand::DisableCulling();
    }

    void RGCommandContext::SetDrawBuffers(const std::span<const u32> attachments) const
    {
        RenderCommand::SetDrawBuffers(attachments);
    }

    void RGCommandContext::BindTexture(const u32 slot, const u32 textureID) const
    {
        RenderCommand::BindTexture(slot, textureID);
    }

    void RGCommandContext::MemoryBarrier(const MemoryBarrierFlags flags) const
    {
        if (flags == MemoryBarrierFlags::None)
            return;
        RenderCommand::MemoryBarrier(flags);
    }

    void RGCommandContext::DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount) const
    {
        RenderCommand::DrawIndexed(vertexArray, indexCount);
    }

    void RGCommandContext::BeginAsyncBatch(const u32 batchIndex) const
    {
        // GL 4.6 runs a single command stream — no true async queue overlap.
        // Insert a KHR_debug group label so the batch region is visible in
        // RenderDoc / Nsight.  The guard prevents crashes in headless / test
        // contexts where glad has not been initialised.
        if (GLAD_GL_KHR_debug)
        {
            const std::string label = "AsyncBatch[" + std::to_string(batchIndex) + "]";
            glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, batchIndex,
                             static_cast<GLsizei>(label.size()), label.c_str());
        }
    }

    void RGCommandContext::EndAsyncBatch([[maybe_unused]] const u32 batchIndex) const
    {
        if (GLAD_GL_KHR_debug)
            glPopDebugGroup();
    }

    u32 RGCommandContext::ResolveTexture(const RGTextureHandle handle) const
    {
        if (!m_RenderGraph)
            return 0;

        if (!handle.IsValid())
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "invalid-texture-handle");
            return 0;
        }

        if (!m_RenderGraph->IsTextureHandleCurrent(handle))
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "stale-texture-handle");
            return 0;
        }

        const auto resolved = m_RenderGraph->ResolveTexture(handle);
        if (resolved == 0)
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "texture-resolve-zero");

        return resolved;
    }

    Ref<Framebuffer> RGCommandContext::ResolveFramebuffer(const RGFramebufferHandle handle) const
    {
        if (!m_RenderGraph)
            return nullptr;

        if (!handle.IsValid())
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "invalid-framebuffer-handle");
            return nullptr;
        }

        if (!m_RenderGraph->IsFramebufferHandleCurrent(handle))
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "stale-framebuffer-handle");
            return nullptr;
        }

        auto resolved = m_RenderGraph->ResolveFramebuffer(handle);
        if (!resolved)
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "framebuffer-resolve-null");

        return resolved;
    }

    void RGCommandContext::ExtractHistoryTexture(std::string_view historyResource,
                                                 const RGTextureHandle sourceHandle,
                                                 std::function<void(u32)> callback)
    {
        if (!m_RenderGraph || historyResource.empty() || !callback)
            return;

        if (!sourceHandle.IsValid())
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "invalid-history-source-texture-handle");
            return;
        }

        if (!m_RenderGraph->IsTextureHandleCurrent(sourceHandle))
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "stale-history-source-texture-handle");
            return;
        }

        m_RenderGraph->ExtractHistoryTexture(historyResource, sourceHandle, std::move(callback));
    }

    void RGCommandContext::ExtractHistoryTexture(std::string_view historyResource,
                                                 const RGFramebufferHandle sourceHandle,
                                                 std::function<void(u32)> callback,
                                                 const u32 colorAttachmentIndex)
    {
        if (!m_RenderGraph || historyResource.empty() || !callback)
            return;

        if (!sourceHandle.IsValid())
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "invalid-history-source-framebuffer-handle");
            return;
        }

        if (!m_RenderGraph->IsFramebufferHandleCurrent(sourceHandle))
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "stale-history-source-framebuffer-handle");
            return;
        }

        m_RenderGraph->ExtractHistoryTexture(historyResource, sourceHandle, std::move(callback), colorAttachmentIndex);
    }

    const FrameBlackboard* RGCommandContext::GetBlackboard() const noexcept
    {
        if (!m_RenderGraph)
            return nullptr;

        return &m_RenderGraph->GetBlackboard();
    }
} // namespace OloEngine
