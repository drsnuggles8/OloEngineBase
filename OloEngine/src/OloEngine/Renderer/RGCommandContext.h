#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glm/vec4.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace OloEngine
{
    class Framebuffer;
    class RenderGraph;
    class VertexArray;

    // Minimal graph-visible command context used to scope pass execution.
    // This is intentionally backend-agnostic and does not expose GL/VK/DX
    // types. Future phases can extend it with encoder operations.
    class RGCommandContext
    {
      public:
        void BeginPass(std::string_view passName)
        {
            m_ActivePassName = passName;
            m_IsPassActive = true;
        }

        void EndPass()
        {
            m_ActivePassName.clear();
            m_IsPassActive = false;
        }

        [[nodiscard]] bool IsPassActive() const
        {
            return m_IsPassActive;
        }

        [[nodiscard]] std::string_view GetActivePassName() const
        {
            return m_ActivePassName;
        }

        void SetViewport(u32 x, u32 y, u32 width, u32 height) const;
        void SetClearColor(const glm::vec4& color) const;
        void Clear() const;
        void ResetGraphicsStateToDefault() const;
        void BindDefaultFramebuffer() const;
        void SetDepthTest(bool enabled) const;
        void SetDepthMask(bool enabled) const;
        void SetBlendState(bool enabled) const;
        void SetAlphaBlendStandard() const;
        void SetOpaqueReplaceBlend() const;
        void SetCulling(bool enabled) const;
        void SetDrawBuffers(std::span<const u32> attachments) const;
        void BindTexture(u32 slot, u32 textureID) const;
        void MemoryBarrier(MemoryBarrierFlags flags) const;
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0) const;
        // Async-compute batch boundaries.
        // In GL 4.6 (single command stream) these insert KHR_debug group labels
        // for profiling tools. Future Vulkan/DX12 backends map them to
        // queue-wait / queue-signal operations.
        void BeginAsyncBatch(u32 batchIndex) const;
        void EndAsyncBatch(u32 batchIndex) const;
        [[nodiscard]] u32 ResolveTexture(RGTextureHandle handle) const;
        [[nodiscard]] Ref<Framebuffer> ResolveFramebuffer(RGFramebufferHandle handle) const;
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGTextureHandle sourceHandle,
                                   std::function<void(u32)> callback);
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGFramebufferHandle sourceHandle,
                                   std::function<void(u32)> callback,
                                   u32 colorAttachmentIndex = 0);
        // Expose the frame blackboard so Execute() callbacks
        // can resolve their own input handles without a per-frame side-channel
        // setter.  Returns nullptr when no render graph is attached (headless /
        // unit-test mode); callers must guard against nullptr.
        [[nodiscard]] const FrameBlackboard* GetBlackboard() const noexcept;
        void SetRenderGraph(RenderGraph* graph)
        {
            m_RenderGraph = graph;
        }

      private:
        std::string m_ActivePassName;
        bool m_IsPassActive = false;
        RenderGraph* m_RenderGraph = nullptr;
    };
} // namespace OloEngine
