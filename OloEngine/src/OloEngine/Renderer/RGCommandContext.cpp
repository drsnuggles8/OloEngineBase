#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>

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
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableCulling();
        RenderCommand::SetCullFace(RHI::CullMode::Back);
        RenderCommand::SetLineWidth(1.0f);
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::DisableScissorTest();
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetPolygonOffset(0.0f, 0.0f);
        RenderCommand::EnableMultisampling();
    }

    void RGCommandContext::ResetOpaqueForwardDrawState() const
    {
        // Order matches the four call sites this replaced, so the emitted GL
        // sequence is unchanged. See the header for why depth test is excluded.
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetCullFace(RHI::CullMode::Back);
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
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
        RenderCommand::SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
    }

    void RGCommandContext::SetOpaqueReplaceBlend() const
    {
        RenderCommand::SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::Zero);
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

    void RGCommandContext::BindTexture(const u32 slot, const RHI::ResourceHandle texture) const
    {
        RenderCommand::BindTexture(slot, texture);
    }

    namespace
    {
        // The shared heap-offset table (issue #691 Phase 3). One std140 UBO of
        // `MAX_ENGINE_TEXTURE_SLOTS` uints, indexed by the very `TEX_*` constant
        // the pass used to bind with — so the bindless and slot-based variants
        // of a shader cannot disagree about which texture is which.
        //
        // Function-local rather than an RGCommandContext member because the
        // context is handed to passes by const reference and is recreated per
        // frame, while this buffer must outlive both. Render passes execute on
        // the game thread, so the unsynchronised scratch is safe; a
        // `.Parallelizable()` pass would need its own.
        struct HeapOffsetTable
        {
            // std140 pads a `uint` array to 16-byte stride, so the CPU side is
            // uvec4-shaped and the shader indexes [i >> 2][i & 3]. Getting this
            // wrong is the classic std140 trap: the array would read every
            // fourth offset and sample three wrong textures out of four.
            static constexpr u32 kSlots = ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS;
            static constexpr u32 kVec4s = (kSlots + 3u) / 4u;

            std::array<u32, kVec4s * 4u> Scratch{};
            Ref<UniformBuffer> Buffer;
            bool Dirty = false;
        };

        HeapOffsetTable& OffsetTable()
        {
            static HeapOffsetTable s_Table;
            return s_Table;
        }
    } // namespace

    RHI::HeapOffset RGCommandContext::BindTextureOrHeapOffset(const u32 slot, const RHI::ResourceHandle texture,
                                                              const RHI::HeapSlotLifetime lifetime,
                                                              const RHI::SamplerDesc& sampler) const
    {
        auto& heap = RHI::DescriptorHeap::Get();

        if (heap.IsEnabled() && slot < HeapOffsetTable::kSlots)
        {
            RHI::ViewDesc viewDesc;
            viewDesc.Resource = texture;

            if (const RHI::ViewHandle view = heap.GetOrCreateView(texture, viewDesc, sampler, lifetime);
                view.IsValid())
            {
                // Fetched at the point of write, never stored — ADR 0011 §1.2.
                // For a persistent view this is a stable value the memoised
                // lookup above already found; for a transient it is this frame's
                // ring slot and is stale the moment the frame ends.
                const RHI::HeapOffset offset = RHI::OffsetOf(view);
                if (offset.IsValid())
                {
                    auto& table = OffsetTable();
                    table.Scratch[slot] = offset.Value;
                    table.Dirty = true;
                    return offset;
                }
            }
        }

        // Every failure lands here, and that is the design: a machine without
        // the extension, a heap that filled up, a resource that died mid-frame,
        // and a slot outside the table all render the frame the old way rather
        // than rendering it wrong.
        RenderCommand::BindTexture(slot, texture);
        return {};
    }

    void RGCommandContext::FlushHeapOffsets() const
    {
        auto& table = OffsetTable();
        if (!table.Dirty || !RHI::DescriptorHeap::Get().IsEnabled())
        {
            return;
        }

        if (!table.Buffer)
        {
            table.Buffer = UniformBuffer::Create(static_cast<u32>(table.Scratch.size() * sizeof(u32)),
                                                 ShaderBindingLayout::UBO_HEAP_OFFSETS);
        }

        table.Buffer->SetData(table.Scratch.data(), static_cast<u32>(table.Scratch.size() * sizeof(u32)));
        table.Dirty = false;
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
        // Insert a debug group label so the batch region is visible in
        // RenderDoc / Nsight. The backend no-ops when the capability is absent
        // (or when no device is up), which is why the GLAD_GL_KHR_debug probe
        // that used to guard this is gone — a loader-symbol test is not a
        // portable way to ask "does this backend support debug markers".
        const std::string label = "AsyncBatch[" + std::to_string(batchIndex) + "]";
        RenderCommand::PushDebugGroup(batchIndex, label);
    }

    void RGCommandContext::EndAsyncBatch([[maybe_unused]] const u32 batchIndex) const
    {
        RenderCommand::PopDebugGroup();
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

    RHI::ResourceHandle RGCommandContext::ResolveTextureHandle(const RGTextureHandle handle) const
    {
        if (!m_RenderGraph)
            return {};

        if (!handle.IsValid())
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "invalid-texture-handle");
            return {};
        }

        if (!m_RenderGraph->IsTextureHandleCurrent(handle))
        {
            m_RenderGraph->RecordResolveFailure(m_ActivePassName, "stale-texture-handle");
            return {};
        }

        // NOT recorded as a resolve failure when the result is null: an
        // unmigrated resource legitimately has no identity yet, and counting
        // that as a failure would bury the real ones in noise for the whole
        // duration of the migration.
        return m_RenderGraph->ResolveTextureHandle(handle);
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
