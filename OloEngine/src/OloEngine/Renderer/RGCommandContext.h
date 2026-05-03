#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glm/vec4.hpp>

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

        void SetViewport(u32 x, u32 y, u32 width, u32 height);
        void SetClearColor(const glm::vec4& color);
        void Clear();
        void ResetGraphicsStateToDefault();
        void BindDefaultFramebuffer();
        void SetDepthTest(bool enabled);
        void SetDepthMask(bool enabled);
        void SetBlendState(bool enabled);
        void SetAlphaBlendStandard();
        void SetOpaqueReplaceBlend();
        void SetCulling(bool enabled);
        void SetDrawBuffers(std::span<const u32> attachments);
        void BindTexture(u32 slot, u32 textureID);
        void MemoryBarrier(MemoryBarrierFlags flags);
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0);
        // Phase G Slice 6 — async-compute batch boundaries.
        // In GL 4.6 (single command stream) these insert KHR_debug group labels
        // for profiling tools. Future Vulkan/DX12 backends map them to
        // queue-wait / queue-signal operations.
        void BeginAsyncBatch(u32 batchIndex);
        void EndAsyncBatch(u32 batchIndex);
        [[nodiscard]] u32 ResolveTexture(RGTextureHandle handle) const;
        [[nodiscard]] Ref<Framebuffer> ResolveFramebuffer(RGFramebufferHandle handle) const;
        // Phase F slice 35 — expose the frame blackboard so Execute() callbacks
        // can resolve their own input handles without a per-frame side-channel
        // setter.  Returns nullptr when no render graph is attached (headless /
        // unit-test mode); callers must guard against nullptr.
        [[nodiscard]] const FrameBlackboard* GetBlackboard() const noexcept;
        void SetRenderGraph(const RenderGraph* graph)
        {
            m_RenderGraph = graph;
        }

      private:
        std::string m_ActivePassName;
        bool m_IsPassActive = false;
        const RenderGraph* m_RenderGraph = nullptr;
    };
} // namespace OloEngine
