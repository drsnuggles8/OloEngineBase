#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
// HeapSlotLifetime / SamplerDesc for BindTextureOrHeapOffset (issue #691).
// RHITypes.h alone is not enough — those live with the resource descriptions.
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
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
        // The opaque-forward draw state that forward passes re-establish around a
        // CommandBucket execution, because skybox / debug / grid commands inside
        // the bucket flip these and would otherwise leak into the next pass.
        //
        // Deliberately narrower than ResetGraphicsStateToDefault(), which also
        // DISABLES culling and resets stencil / scissor / colour mask / polygon
        // offset / multisampling — none of which these sites want touched.
        //
        // Deliberately does NOT set the depth *test*: callers entering the pass
        // enable it themselves, while the post-bucket restore sites leave it
        // alone, and re-enabling one a bucket command legitimately disabled
        // would change what renders.
        void ResetOpaqueForwardDrawState() const;
        void BindDefaultFramebuffer() const;
        void SetDepthTest(bool enabled) const;
        void SetDepthMask(bool enabled) const;
        void SetBlendState(bool enabled) const;
        void SetAlphaBlendStandard() const;
        void SetOpaqueReplaceBlend() const;
        void SetCulling(bool enabled) const;
        void SetDrawBuffers(std::span<const u32> attachments) const;
        void BindTexture(u32 slot, RHI::ResourceHandle texture) const;

        // ---------------------------------------------------------------------
        // The heap-bindless form of the call above (issue #691).
        //
        // ONE call replaces the bind at a pass call site, and it forks for you:
        //
        //   * heap enabled  -> mints/looks up the view, records the offset in the
        //     shared heap-offset table at index `slot`, and binds NOTHING.
        //   * heap disabled, no extension, heap full, dead resource -> falls back
        //     to `BindTexture(slot, texture)` exactly as before.
        //
        // `slot` keeps its meaning either way, which is the point ADR 0011 §1.1
        // makes: what dies is the ACT of binding; the `TEX_*` number survives,
        // promoted from a compile-time constant to runtime data. A converted
        // shader indexes the offset table with the same constant it used to
        // declare `layout(binding = N)` with, so the two variants cannot
        // disagree about which texture is which.
        //
        // Call `FlushHeapOffsets()` once before the draw. The return value is
        // for a pass that wants to put the offset somewhere of its own (a
        // material struct, an SSBO); most callers can ignore it.
        RHI::HeapOffset BindTextureOrHeapOffset(
            u32 slot, RHI::ResourceHandle texture, RHI::HeapSlotLifetime lifetime,
            const RHI::SamplerDesc& sampler = {},
            RHI::NullSamplerKind kind = RHI::NullSamplerKind::Texture2D) const;

        // Uploads the offsets recorded since the last flush. No-op when the heap
        // is disabled, so a converted pass costs nothing on the slot-based path.
        void FlushHeapOffsets() const;
        void MemoryBarrier(MemoryBarrierFlags flags) const;
        // ADR 0011 §1.5: the pre-pass barrier batch carrying both
        // currencies — the GL flags bitmask AND the handle-resolved
        // per-resource transitions. Forwards to RendererAPI::IssueBarrierBatch;
        // GL executes the flags exactly as MemoryBarrier() did, an
        // explicit-barrier backend lowers the RHI::Barrier span instead.
        void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers) const;
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0) const;
        // Async-compute batch boundaries.
        // In GL 4.6 (single command stream) these insert KHR_debug group labels
        // for profiling tools. Future Vulkan/DX12 backends map them to
        // queue-wait / queue-signal operations.
        void BeginAsyncBatch(u32 batchIndex) const;
        void EndAsyncBatch(u32 batchIndex) const;
        [[nodiscard]] u32 ResolveTexture(RGTextureHandle handle) const;
        // Identity form (issue #691). Returns the null handle
        // for a resource imported as a native id — a resource migrates its
        // whole creator/import/resolve/bind chain in one slice.
        [[nodiscard]] RHI::ResourceHandle ResolveTextureHandle(RGTextureHandle handle) const;
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
