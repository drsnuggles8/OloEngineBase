#pragma once

// =============================================================================
// VulkanRendererAPI — the Vulkan implementation of the RendererAPI facade
// (issue #691; brought to editor-parity scope later).
//
// Current scope: the full facade — barrier batches, transient clears, draws
// (lazy dynamic-rendering scopes + root-data assembly per ADR 0011 §4/§5),
// compute dispatch, buffer/texture lifecycle including the raw-handle
// facade families, staged uploads (frame command buffer, with a blocking
// one-shot fallback outside a bracket), readbacks (ReadTextureSubImage —
// the MCP diagnostics spine), per-target pending clears, and the sampler
// heap routing. The remaining warn-once stubs are counted BY KIND (see
// StubKind): a deferred feature, a precondition failure, or a call outside
// a recording bracket — deliberately loud, never silent (the amendment (42)
// contract).
//
// Recording model: Vulkan has no GL-style implicit current context — every
// vkCmd* needs a live VkCommandBuffer. VulkanContext's frame loop (and the
// device-gated tests) bracket work in BeginRecording/EndRecording; a
// mid-frame READ of GPU state flushes via FlushFrameRecordingAndWait
// (suspend / submit / fence-wait / resume). A recording-dependent entry
// point called outside a bracket either falls back to a one-shot (uploads,
// readbacks) or warn-once skips.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RendererAPI.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"

#include <volk.h>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class VulkanFramebuffer;
    class VulkanVertexArray;
    struct VulkanRootDataLayout;

    // -------------------------------------------------------------------------
    // VulkanQueryRegistry — the object-less occlusion-query family behind the
    // CreateQueries / BeginQuery / EndQuery / GetQueryResult* / DeleteQueries
    // facade entries (#691, ADR item A6).
    //
    // GL's shape is N independent query NAMES; Vulkan's is one VkQueryPool with
    // N slots. `CreateQueries` is the natural pool boundary — OcclusionQueryPool
    // calls it once per double-buffer half with the full capacity — so one
    // VkQueryPool backs one CreateQueries call and each minted handle names a
    // (pool, index) pair. There is no engine-side C++ object to hang the pool
    // off, hence this side table (the VulkanRawBufferRegistry shape).
    //
    // `Recorded` is the frame-crossing guard: `vkGetQueryPoolResults` on a slot
    // that was reset but never written blocks forever under WAIT, and reading a
    // slot that was never reset at all is undefined. Nothing may be read until
    // a Begin/End pair has actually reached a command buffer.
    //
    // Render-thread only, same as everything else here.
    // -------------------------------------------------------------------------
    class VulkanQueryRegistry
    {
      public:
        struct Entry
        {
            VkQueryPool Pool = VK_NULL_HANDLE;
            u32 Index = 0;
            bool Recorded = false; ///< the query's write(s) reached a command buffer
            // How this entry reads back (#691): OcclusionAnySamples is
            // one occlusion slot (raw count); Timestamp is one timestamp slot
            // (ticks × timestampPeriod → nanoseconds, stamped via
            // WriteTimestamp); TimeElapsed is a PAIR of timestamp slots
            // (Index, Index+1) bracketed by Begin/EndQuery — Vulkan has no
            // native elapsed query, and vkCmdBeginQuery on a TIMESTAMP pool is
            // invalid, which the pre-Phase-9 single-slot shape would have hit
            // the moment a TimeElapsed tenant ran (none did: the only user was
            // GL-gated until the tool conversions).
            RHI::QueryType Type = RHI::QueryType::OcclusionAnySamples;
        };

        [[nodiscard]] static VulkanQueryRegistry& Get();

        // Mints one identity per element of `outQueries`, all backed by a
        // single VkQueryPool of that size. Elements are set to
        // RHI::NullResource on failure (the GL twin's contract).
        void CreatePool(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries);
        // Null when the handle was never minted here (or is stale).
        [[nodiscard]] Entry* Lookup(RHI::ResourceHandle handle);
        // Retires the identities; the backing pool is deferred-reclaimed once
        // its last live handle goes away. Safe on foreign/stale handles.
        void Destroy(std::span<const RHI::ResourceHandle> queries);
        // Teardown net: destroy every remaining pool (caller guarantees idle).
        void ReleaseAll();
        // Diagnostic/test affordance.
        [[nodiscard]] sizet GetLivePoolCount() const
        {
            return m_Pools.size();
        }

      private:
        VulkanQueryRegistry() = default;

        [[nodiscard]] static u64 Key(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }

        struct Pool
        {
            VkQueryPool Handle = VK_NULL_HANDLE;
            u32 Count = 0;
            u32 LiveQueries = 0;
        };

        std::unordered_map<u64, Entry> m_Entries;
        std::vector<Pool> m_Pools;
    };

    class VulkanRendererAPI : public RendererAPI
    {
      public:
        VulkanRendererAPI() = default;
        ~VulkanRendererAPI() override = default;

        [[nodiscard]] const VulkanRecordedPipelineState& RecordedState() const
        {
            return m_State;
        }

        // --- Recording bracket (backend-internal, not facade) ------------
        void BeginRecording(VkCommandBuffer cmd);
        void EndRecording();
        [[nodiscard]] VkCommandBuffer CurrentCommandBuffer() const
        {
            return Ctx().Cmd;
        }

        // --- Mid-frame flush (#691) --------------------------------
        // A synchronous mid-frame readback (StorageBuffer::GetData between
        // two dispatches — the fluid solver's coupling shape) needs the
        // frame command buffer SUBMITTED first, or it reads the previous
        // frame's contents: queue submissions execute in submit order, and a
        // one-shot submitted now runs BEFORE the still-recording frame.
        // VulkanContext::FlushFrameRecordingAndWait drives these two around
        // its submit: Suspend closes the rendering scope and hands the
        // command buffer over (null = refuse: no recording, or an occlusion
        // query is open — a query span cannot cross command buffers); Resume
        // re-enters the bracket on the reset buffer, dropping only the
        // per-command-buffer bind caches. Frame-scoped state (the backbuffer
        // publication, a pending clear, draw counters, framebuffer
        // selections) deliberately survives — the frame continues, it does
        // not restart.
        [[nodiscard]] VkCommandBuffer SuspendRecordingForFlush();
        // Called only after the suspended command buffer was accepted by the
        // queue. Keeps the executed-layout view accurate while a graph records
        // later split segments in the same frame.
        void MarkSuspendedRecordingSubmitted();
        void ResumeRecordingAfterFlush(VkCommandBuffer cmd);

        // Backend-internal (#691): record a staged buffer→image copy
        // of one (mip, layer) region into the CURRENT frame command buffer,
        // with tracker-exact transitions and the staging buffer routed
        // through deferred reclaim. The cubemap face-upload paths call this
        // when a recording is live (the IBL cache load runs mid-frame on
        // this backend); false when no recording is open or staging failed —
        // callers then take their blocking one-shot arm.
        [[nodiscard]] bool RecordStagedImageUpload(VkImage image, u32 mip, u32 baseLayer, u32 width, u32 height,
                                                   const void* data, u64 sizeBytes);
        [[nodiscard]] VulkanImageLayoutTracker& LayoutTracker()
        {
            return Ctx().Tracker;
        }

        // --- The default framebuffer (#691, Final pass) -----------
        // GL's "default framebuffer" is a fixed object (name 0) that outlives
        // every frame; Vulkan's is a DIFFERENT image every frame — whichever
        // one vkAcquireNextImageKHR just handed the swap loop. So the backend
        // cannot own it: the frame recorder publishes it for the duration of
        // one recording bracket, and BindDefaultFramebuffer (i.e. GL's
        // glBindFramebuffer(0), which is also VulkanFramebuffer::Unbind)
        // resolves to whatever is published.
        //
        // "No framebuffer bound AND a backbuffer published" IS the default
        // framebuffer — there is deliberately no extra selected/unselected
        // flag, because that is exactly GL's rule and it keeps Unbind() and
        // BindDefaultFramebuffer() the same operation. With nothing published
        // (every headless test, and the live loop outside the callback) the
        // old behaviour stands: a draw with no target is dropped with the
        // warn-once.
        struct FrameBackbuffer
        {
            RHI::ResourceHandle Handle{}; ///< The neutral identity (barriers, layout tracking).
            VkImage Image = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE; ///< Attachment view; owned by the publisher.
            VkFormat Format = VK_FORMAT_UNDEFINED;
            u32 Width = 0;
            u32 Height = 0;

            [[nodiscard]] bool IsValid() const
            {
                return Image != VK_NULL_HANDLE && View != VK_NULL_HANDLE;
            }
        };
        /// Publish the acquired image as this recording's default framebuffer.
        /// The view is passed explicitly because it is the one piece the
        /// registries do not carry (VulkanImageInfo has no view); everything
        /// else is derived from `handle`. Publishing an invalid handle, or a
        /// null view, clears the publication.
        void SetFrameBackbuffer(RHI::ResourceHandle handle, VkImageView view, u32 width, u32 height);
        void ClearFrameBackbuffer();
        [[nodiscard]] const FrameBackbuffer& GetFrameBackbuffer() const
        {
            return m_Backbuffer;
        }
        /// True once this recording opened a rendering scope against the
        /// published backbuffer (i.e. the frame actually targeted the screen).
        [[nodiscard]] bool BackbufferWasWrittenThisRecording() const
        {
            return m_BackbufferWritten;
        }
        /// Close the frame's backbuffer work and leave it presentable.
        ///
        /// `frameRendered` is the recorder's own verdict. When it is true a
        /// still-PENDING clear is materialised as an empty CLEAR-loadOp scope
        /// — GL clears immediately, this backend folds the clear into the next
        /// draw's loadOp, so a pass that clears the screen and then bails
        /// (FinalRenderPass with no resolvable input) would otherwise present
        /// undefined memory. Returns true when the backbuffer holds defined
        /// content and has been transitioned to Present; false means nothing
        /// touched it and the caller's clear-only fallback is both safe and
        /// necessary.
        [[nodiscard]] bool FinalizeBackbufferForPresent(bool frameRendered);

        // Observability for the execution test: how many packets/entry points
        // hit an unimplemented stub (nothing may fall through silently). Split by
        // KIND (#691): a deferred FEATURE (no Vulkan lowering
        // yet), a PRECONDITION failure (the lowering exists but an input
        // handle/image did not resolve), and an outside-recording-bracket
        // call (the timing contract). The total stays the back-compat sum.
        enum class StubKind : u8
        {
            DeferredFeature = 0,
            PreconditionFailure,
            OutsideRecording,
            Count
        };
        /// Times a STORAGE binding was published as the arena's null block
        /// because nothing fed it (issue #1052). Non-zero means a shader is
        /// reading a zero-filled stand-in it may index out of; it is the
        /// device-loss precursor, so a tenant that renders real geometry should
        /// assert this is 0 the way it asserts the stub count is.
        [[nodiscard]] u64 GetUnfedStorageBindingCount() const
        {
            return m_UnfedStorageBindings.load(std::memory_order_relaxed);
        }

        [[nodiscard]] u64 GetUnimplementedStubHitCount() const
        {
            return m_UnimplementedStubHits;
        }
        [[nodiscard]] u64 GetStubHitCount(StubKind kind) const
        {
            // Count (and anything out of range) is not a bucket — 0, not an
            // out-of-bounds read.
            const auto index = static_cast<sizet>(kind);
            return index < static_cast<sizet>(StubKind::Count) ? m_StubHitsByKind[index] : 0u;
        }

        // Draw observability (issue #691): PrepareDraw's failure
        // paths drop the draw with at most a warn-once — a fixture asserting
        // "N draws prepared, 0 dropped" turns a silently black frame into a
        // named failure. Reset by BeginRecording.
        [[nodiscard]] u32 GetPreparedDrawsThisRecording() const
        {
            return m_Main.PreparedDraws;
        }
        [[nodiscard]] u32 GetDroppedDrawsThisRecording() const
        {
            return m_Main.DroppedDraws;
        }
        [[nodiscard]] u32 GetGpuWrittenRootDrawsThisRecording() const
        {
            return m_Main.GpuWrittenRootDraws;
        }
        // Draws the host-side conditional-render predicate skipped. Kept apart
        // from the dropped counter on purpose: a conditional skip is the
        // requested behaviour, a drop is a failure.
        [[nodiscard]] u32 GetConditionallySkippedDrawsThisRecording() const
        {
            return m_Main.ConditionallySkippedDraws;
        }

        // --- RendererAPI ---------------------------------------------------
        void Init() override;
        void SetViewport(u32 x, u32 y, u32 width, u32 height) override;
        void SetClearColor(const glm::vec4& color) override;
        void Clear() override;
        void ClearDepthOnly() override;
        void ClearColorAndDepth() override;
        Viewport GetViewport() const override;
        void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount) override;
        void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 instanceCount) override;
        void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices) override;
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount) override;
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex) override;
        void DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex, u32 instanceCount) override;
        void DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 patchVertices) override;
        void SetLineWidth(f32 width) override;
        void EnableCulling() override;
        void DisableCulling() override;
        void FrontCull() override;
        void BackCull() override;
        void SetCullFace(RHI::CullMode face) override;
        void SetDepthMask(bool value) override;
        void SetDepthTest(bool value) override;
        void SetDepthFunc(RHI::CompareOp func) override;
        void SetBlendState(bool value) override;
        void SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor) override;
        void SetBlendEquation(RHI::BlendOp mode) override;
        void EnableStencilTest() override;
        void DisableStencilTest() override;
        bool IsStencilTestEnabled() const override;
        void SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask) override;
        void SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass) override;
        void SetStencilMask(u32 mask) override;
        void ClearStencil() override;
        void SetPolygonMode(RHI::PolygonMode mode) override;
        void EnableScissorTest() override;
        void DisableScissorTest() override;
        void SetScissorBox(i32 x, i32 y, u32 width, u32 height) override;
        void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override;
        void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override;
        void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer, RHI::PrimitiveTopology topology) override;
        void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer, u32 indirectOffsetBytes, RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes, u32 maxDrawCount, u32 strideBytes) override;
        void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void DispatchComputeIndirect(RHI::ResourceHandle argsBuffer, u32 offsetBytes) override;
        void DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier(MemoryBarrierFlags flags) override;
        [[nodiscard]] bool WriteBufferDeviceAddress(RHI::ResourceHandle destination, u32 destinationOffset,
                                                    RHI::ResourceHandle source) override;
        [[nodiscard]] bool QueryGpuDrivenRootDataLayout(RHI::ResourceHandle shader, u32 storageBinding,
                                                        GpuDrivenRootDataLayout& outLayout) override;
        [[nodiscard]] bool SetNextDrawRootData(RHI::ResourceHandle rootData, u32 gpuWrittenStorageBinding,
                                               u32 expectedFieldOffsetBytes) override;
        void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers) override;
        [[nodiscard]] bool SupportsRenderGraphFenceSubmission() const override;
        [[nodiscard]] bool SubmitRenderGraphFenceSegment() override;
        void BindDefaultFramebuffer() override;
        void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height) override;
        void BindTexture(u32 slot, RHI::ResourceHandle texture) override;
        void BindTexture(u32 slot, RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler) override;
        void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered, u32 layer, RHI::Access access, RHI::Format format) override;
        void SetPolygonOffset(f32 factor, f32 units) override;
        void EnableMultisampling() override;
        void DisableMultisampling() override;
        void SetColorMask(bool red, bool green, bool blue, bool alpha) override;
        void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha) override;
        void SetBlendStateForAttachment(u32 attachment, bool enabled) override;
        void ResetBlendStateForAttachment(u32 attachment) override;
        void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) override;
        void CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget, RHI::ResourceHandle dst, TextureTargetType dstTarget, u32 width, u32 height) override;
        void CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ, RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ, u32 width, u32 height) override;
        void CopyImageSubDataRegion(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcX, i32 srcY, i32 srcZ, RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstX, i32 dstY, i32 dstZ, u32 width, u32 height) override;
        void CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height) override;
        void SetDrawBuffers(std::span<const u32> attachments) override;
        void RestoreAllDrawBuffers(u32 colorAttachmentCount) override;
        [[nodiscard]] RHI::ResourceHandle CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture, u32 numLayers) override;
        void SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter) override;
        void SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap) override;
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height, RHI::Format sourceFormat, const void* data) override;
        void BeginConditionalRender(RHI::ResourceHandle query) override;
        void EndConditionalRender() override;
        void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override;
        void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override;
        void BindShaderProgram(RHI::ResourceHandle program) override;
        void BindVertexArrayRaw(RHI::ResourceHandle vertexArray) override;
        void BindFramebuffer(RHI::ResourceHandle framebuffer) override;
        void DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType indexType, u32 baseIndex) override;
        void DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType indexType, u32 baseIndex, u32 instanceCount) override;
        void DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount) override;
        void SetPatchVertexCount(u32 patchVertices) override;
        void SetFrontFace(RHI::FrontFace face) override;
        void SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB, RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha) override;
        void SetClearDepth(f32 depth) override;
        void AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex, RHI::ResourceHandle texture, u32 mipLevel) override;
        void AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture, u32 mipLevel) override;
        [[nodiscard("Store this!")]] bool IsFramebufferComplete(RHI::ResourceHandle framebuffer) override;
        void SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, std::span<const u32> attachmentIndices) override;
        void RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, u32 colorAttachmentCount) override;
        void SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex) override;
        void ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex, const glm::vec4& color) override;
        void ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth) override;
        void BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer, i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1, i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1, RHI::BlitAspect aspect, RHI::Filter filter) override;
        void AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes, RHI::MemoryResidency residency) override;
        void* AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes) override;
        void UnmapBuffer(RHI::ResourceHandle buffer) override;
        void UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, const void* data) override;
        void ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest) override;
        void CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer, u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes) override;
        void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value, u64 offset = 0, u64 size = ~0ull) override;
        void ClearBufferFloat(RHI::ResourceHandle buffer, f32 value) override;
        [[nodiscard]] RHI::ResourceHandle CreateTexture2DHandle(u32 width, u32 height, RHI::Format internalFormat) override;
        [[nodiscard]] RHI::ResourceHandle CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format internalFormat) override;
        [[nodiscard]] RHI::ResourceHandle CreateFramebufferHandle() override;
        [[nodiscard]] RHI::ResourceHandle CreateBufferHandle() override;
        [[nodiscard]] RHI::ResourceHandle CreateVertexArrayHandle() override;
        void DeleteTexture(RHI::ResourceHandle texture) override;
        void DeleteFramebuffer(RHI::ResourceHandle framebuffer) override;
        void DeleteBuffer(RHI::ResourceHandle buffer) override;
        void DeleteVertexArray(RHI::ResourceHandle vertexArray) override;
        void SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer) override;
        void ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color) override;
        void ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value) override;
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, u32 width, u32 height, RHI::Format sourceFormat, const void* data) override;
        void UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset, u32 width, u32 height, u32 depth, RHI::Format sourceFormat, const void* data) override;
        [[nodiscard("Store this!")]] bool ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel, RHI::Format destFormat, sizet destSizeBytes, void* dest) override;
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel, i32 x, i32 y, i32 z, u32 width, u32 height, u32 depth, RHI::Format destFormat, sizet destSizeBytes, void* dest) override;
        void GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth, u32& outHeight) override;
        [[nodiscard]] bool QueryTextureFormat(RHI::ResourceHandle texture, u32 mipLevel,
                                              RHI::TextureFormatInfo& out) override;
        [[nodiscard]] RHI::ResourceHandle CreateMatchingTextureHandle(RHI::ResourceHandle source) override;
        void TextureBarrier() override;
        void CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries) override;
        void DeleteQueries(std::span<const RHI::ResourceHandle> queries) override;
        void BeginQuery(RHI::QueryType type, RHI::ResourceHandle query) override;
        void EndQuery(RHI::QueryType type) override;
        void WriteTimestamp(RHI::ResourceHandle query) override;

        // The bind-time layout seam, callable by BOTH descriptor routes (#691
        // closes the "amendment (63) covers the slot path only" debt
        // the issue text carried). BindTexture / BindImageTexture used
        // to own private copies for the SLOT path; the HEAP route
        // (VulkanDescriptorHeapBackend::UploadSlots) wrote descriptors
        // declaring SHADER_READ_ONLY / GENERAL with no transition at all —
        // harmless only while every heap-bound image happens to be
        // transitioned by the graph's plan or a pass's own barriers, which is
        // not a property anything enforces. No-op outside a recording
        // (load-time descriptor writes get their layouts from first use).
        // This closes the named gap; it is NOT the fix for the per-resize
        // validation error (#800), which survives it.
        void EnsureImageLayoutForDescriptor(VkImage image, VkImageLayout target,
                                            const VkImageSubresourceRange& range);
        [[nodiscard("Store this!")]] bool IsQueryResultAvailable(RHI::ResourceHandle query) override;
        [[nodiscard("Store this!")]] u32 GetQueryResultU32(RHI::ResourceHandle query) override;
        [[nodiscard("Store this!")]] u64 GetQueryResultU64(RHI::ResourceHandle query) override;
        [[nodiscard("Store this!")]] u64 CreateFence() override;
        [[nodiscard("Store this!")]] RHI::FenceStatus ClientWaitFence(u64 fence, u64 timeoutNanoseconds) override;
        [[nodiscard("Store this!")]] bool IsFenceSignaled(u64 fence) override;
        void DestroyFence(u64 fence) override;
        void PushDebugGroup(u32 id, std::string_view label) override;
        void PopDebugGroup() override;
        void WaitForDeviceIdle() override;
        [[nodiscard("Store this!")]] u32 GetMaxFramebufferSamples() const override;
        [[nodiscard("Store this!")]] u32 GetMaxColorTextureSamples() const override;
        [[nodiscard("Store this!")]] u32 GetMaxDepthTextureSamples() const override;
        void SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value) override;
        [[nodiscard("Store this!")]] bool IsDeviceAvailable() const override;
        [[nodiscard("Store this!")]] u32 GetMaxUniformBlockSize() const override;
        [[nodiscard("Store this!")]] bool SupportsInt64ShaderAtomics() const override;
        [[nodiscard("Store this!")]] bool SupportsMeshShaders() const override;
        [[nodiscard("Store this!")]] RayTracing::Capabilities GetRayTracingCapabilities() const override;

        // Acceleration-structure builds (#978) are queue commands and are
        // ILLEGAL inside a dynamic-rendering scope, which this backend opens
        // lazily. This closes any open scope and returns the frame's primary
        // command buffer so the RT backend can record its builds in frame
        // order — the same shape RecordStagedImageUpload uses internally,
        // exposed because the acceleration-structure builder is a sibling TU
        // rather than a member.
        //
        // Returns VK_NULL_HANDLE when no frame is recording, which the caller
        // must treat as "not this frame" rather than as an error: a one-shot
        // submit would execute AHEAD of the frame that produced the geometry.
        [[nodiscard]] VkCommandBuffer BeginAccelerationStructureRecording();

        // --- Parallel command recording (#806, amendment (92)) ---------------
        [[nodiscard]] bool SupportsParallelRecording() const override;
        void RecordParallel(u32 itemCount, const std::function<void(u32 item)>& body, u32 instanceCapacity = 1u) override;
        void RecordParallelOrdered(u32 itemCount, const std::function<void(u32)>& body,
                                   const std::function<void(u32)>& beforeExecute,
                                   const std::function<void(u32)>& afterExecute,
                                   u32 instanceCapacity = 1u, std::span<const std::string> itemPassNames = {}) override;
        void ReleaseParallelRecordingResources() override;
        [[nodiscard]] ParallelRecordingFrameStats GetParallelRecordingStats() const override
        {
            return m_ParallelStats;
        }
        // The context the calling thread records on (main, or a running
        // item's). Backend-internal: the ImGui overlay and the texture
        // upload paths read the current command buffer through it.
        [[nodiscard]] const VulkanRecordingContext& CurrentRecordingContext() const
        {
            return Ctx();
        }
        [[nodiscard]] bool IsInParallelRegion() const
        {
            return m_InParallelRegion;
        }
        [[nodiscard]] bool IsRecordingParallelItem() const override
        {
            return OnWorkerContext();
        }
        // The depth-array layer selection for `framebuffer` on the current
        // context (Framebuffer::AttachDepthTextureArrayLayer's Vulkan half —
        // the selection is recording-context state, amendment (92) rule 4).
        void SetFramebufferDepthArraySelection(RHI::ResourceHandle framebuffer,
                                               const VulkanRecordingContext::FramebufferAttachmentSelection::DepthArrayLayer& selection);
        [[nodiscard]] VulkanRecordingContext::FramebufferAttachmentSelection::DepthArrayLayer
        GetFramebufferDepthArraySelection(RHI::ResourceHandle framebuffer) const;
        // A cached per-layer view is being destroyed: no context may keep
        // selecting it.
        void ForgetDepthArrayView(VkImageView view);

      private:
        // Const: several facade getters are const-qualified and still must
        // count their stub hit (nothing may fall through silently).
        void UnimplementedStub(const char* entryPoint, StubKind kind = StubKind::DeferredFeature) const;

        // Per-command-buffer state lives in VulkanRecordingContext (#806).
        using RenderingScope = VulkanRecordingContext::RenderingScope;

        using PendingClear = VulkanRecordingContext::PendingClear;

        /// True when the live scope still describes what a draw/clear would
        /// target right now: same framebuffer AND same depth-array layer
        /// selection. Used by both the scope-open path and the clear paths.
        [[nodiscard]] bool ScopeMatchesCurrentTarget() const;

        /// True when a draw right now would target the published backbuffer:
        /// nothing bound (GL's framebuffer 0) and a publication live.
        [[nodiscard]] bool ShouldTargetBackbuffer() const;

        // --- Pending-clear plumbing (#691; see PendingClear) --------
        /// True when the pending clear's recorded requester is what a draw
        /// would target right now (the ScopeMatchesCurrentTarget triple).
        [[nodiscard]] bool PendingClearMatchesCurrentTarget() const;
        /// Record a clear request against the CURRENTLY bound target,
        /// capturing the live clear values. Materializes any earlier pending
        /// clear that belongs to a different target first.
        void RecordPendingClear(bool color, bool depth);
        /// Eagerly clear the pending clear's target with transfer clears
        /// (vkCmdClearColorImage / vkCmdClearDepthStencilImage through the
        /// layout tracker) and drop the record. Called when the pending
        /// requester is superseded by another target's clear/draw, and at
        /// EndRecording so an unconsumed clear still happens (GL semantics).
        /// Must be called OUTSIDE a rendering scope.
        void MaterializePendingClear();
        /// The live scope's render area — the framebuffer's spec, or the
        /// published backbuffer's extent. {0,0} when no scope is open.
        [[nodiscard]] VkExtent2D ScopeExtent() const;

        using FramebufferAttachmentSelection = VulkanRecordingContext::FramebufferAttachmentSelection;

        // End the scope if active (barriers/copies/dispatches are illegal
        // inside a rendering instance).
        void EndRenderingScope();
        // Begin (or continue) the scope for the published framebuffer.
        // Returns false when no target is published or attachment views
        // cannot be built. Fills m_ScopeTargets for the PSO fetch.
        [[nodiscard]] bool EnsureRenderingScopeForDraw();
        // Shared draw-front-end: resolve the current shader + root layout,
        // ensure the scope, fetch + bind the thin PSO, flush dynamic state,
        // set the topology, bind the resource heap once per recording,
        // assemble the root struct from VulkanBindingState + the vertex
        // array, arena-push it, and vkCmdPushDataEXT the 8-byte address.
        // Returns false (warn-once, dropped draw) on any missing piece.
        [[nodiscard]] bool PrepareDraw(const VulkanVertexArray* vao, VkPrimitiveTopology topology);
        // The topology-free common part of PrepareDraw, shared with the
        // task/mesh path (issue #813): everything above EXCEPT
        // vkCmdSetPrimitiveTopology and the primitive-restart dynamic state
        // — a mesh pipeline has no input assembly, and setting either is
        // invalid against one. `meshPipeline` must match the bound shader's
        // stage set (a mesh shader under vkCmdDraw*, or a vertex shader
        // under vkCmdDrawMeshTasksEXT, is invalid either way — mismatches
        // drop loudly here). Counts prepared/dropped draws exactly as
        // PrepareDraw always did.
        [[nodiscard]] bool PrepareDrawCommon(const VulkanVertexArray* vao, bool meshPipeline);
        // Bind the VAO's index buffer if it changed (redundant-bind cache,
        // reset per recording). False when the VAO has no index buffer.
        [[nodiscard]] bool BindIndexBufferFor(const VulkanVertexArray* vao);
        // Handle -> VkBuffer for the indirect-draw family (#691). Any
        // Vulkan-backend buffer identity resolves (its registry native IS the
        // VkBuffer); null + a counted stub on kind mismatch / stale handles.
        [[nodiscard]] VkBuffer ResolveIndirectBuffer(RHI::ResourceHandle indirectBuffer, const char* entryPoint) const;
        // Root-struct assembly + arena push + vkCmdPushDataEXT — shared by
        // draws and dispatches (§4: one contract, no compute special case).
        // Kind-aware: CombinedImageSampler bindings read the TEXTURE slot
        // table, StorageImage bindings the IMAGE-UNIT table (amendment (29):
        // two namespaces), buffers the bind-point tables (57 = vertex pull).
        // `commandOrderedBufferReads` — draws pass true so SSBO reads embed
        // the command-ordered SetData snapshot (VulkanStorageBuffer::
        // GetRootDataAddress); compute passes false to keep the persistent
        // address its GPU-write participants require (#691).
        [[nodiscard]] bool AssembleAndPushRootData(const VulkanRootDataLayout& layout, const char* shaderName,
                                                   const VulkanVertexArray* vao, bool commandOrderedBufferReads);
        void AssembleRootData(const VulkanRootDataLayout& layout, const char* shaderName,
                              const VulkanVertexArray* vao, bool commandOrderedBufferReads);
        [[nodiscard]] bool PushRootDataAddress(VkDeviceAddress rootAddress);

        // Selection-map plumbing (see FramebufferAttachmentSelection above).
        [[nodiscard]] static u64 SelectionKey(RHI::ResourceHandle framebuffer)
        {
            return (static_cast<u64>(framebuffer.Generation) << 32) | framebuffer.Index;
        }
        [[nodiscard]] FramebufferAttachmentSelection* FindSelection(RHI::ResourceHandle framebuffer);
        // Ends the live rendering scope when it targets `framebuffer` — a
        // draw-list change re-shapes the attachment array, so the next draw
        // must re-open the scope with the new mapping.
        void EndScopeIfTargets(RHI::ResourceHandle framebuffer);
        // Shared exact-oldLayout transfer-transition shape (the
        // ClearTextureFloat / CopyImageSubData discipline): stage per-layout-run
        // barriers into `out` and advance the tracker to `newLayout`.
        void StageTransferTransition(VkImage image, const VkImageSubresourceRange& range, VkImageLayout newLayout,
                                     VkAccessFlags2 dstAccess, std::vector<VkImageMemoryBarrier2>& out);
        // Shared read path for IsQueryResultAvailable / GetQueryResultU32|64
        // (and the conditional-render gate). `wait` picks the blocking form
        // (GL's glGetQueryObject*v(GL_QUERY_RESULT) semantics); false is the
        // availability probe. Returns false — leaving `outValue` 0 — for a
        // stale handle or one no command buffer has written yet.
        [[nodiscard]] bool ReadQueryResult(RHI::ResourceHandle query, bool wait, u64& outValue) const;

      public:
        // Called by ~VulkanFramebuffer (before the identity resets) so no
        // API-side state outlives the object: the live scope ends if it
        // targets the dying framebuffer, and a pending lazy clear naming it
        // is dropped — materializing it later would dereference the freed
        // object (review finding, #691). Covers both death paths
        // (raw-registry Destroy and object-owned destruction) through the
        // one destructor they share.
        void NotifyFramebufferDestroyed(const VulkanFramebuffer* framebuffer, RHI::ResourceHandle handle);

      private:
        // The render thread's recording context (#806): every per-command-
        // buffer fact the API used to keep as a member. Ctx() below picks
        // between this and the worker context of a running RecordParallel
        // item, so member functions never name m_Main directly except for
        // the frame bracket itself.
        VulkanRecordingContext m_Main;
        // Item contexts of the parallel recorder, reused across regions and
        // grown on demand (a context never moves: its tracker self-registers).
        std::vector<Scope<VulkanWorkerRecordingContext>> m_Items;
        bool m_InParallelRegion = false;
        u64 m_RegionSerial = 0;                ///< Stamps each region (the buffer writer check, amendment (92) rule 6).
        VulkanLayoutClaimTable m_LayoutClaims; ///< Record-time rule-5 claims for the region in flight.
        // Join scratch, kept for its capacity across regions.
        std::vector<VkCommandBuffer> m_JoinSecondaries;
        std::vector<RHI::Barrier> m_ForkBarriers;
        // Accumulates over one recording; reset at the NEXT BeginRecording, so
        // RendererProfiler::EndFrame (inside the frame callback, after every
        // region joined) and a test reading after EndRecording both see this
        // frame's numbers.
        ParallelRecordingFrameStats m_ParallelStats{};

        [[nodiscard]] VulkanRecordingContext& Ctx()
        {
            if (VulkanRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
                return *worker;
            return m_Main;
        }
        [[nodiscard]] const VulkanRecordingContext& Ctx() const
        {
            if (const VulkanRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
                return *worker;
            return m_Main;
        }
        // True on a thread running a RecordParallel item. Every entry point
        // outside amendment (92)'s envelope — creation and destruction,
        // uploads, readbacks, queries, timestamps, fences, device waits, the
        // mid-frame flush, conditional rendering — opens with RefuseOnWorker.
        [[nodiscard]] static bool OnWorkerContext()
        {
            return CurrentVulkanWorkerContext() != nullptr;
        }
        // Debug: assert; otherwise warn-once. Returns true when refused.
        [[nodiscard]] bool RefuseOnWorker(const char* entryPoint) const;
        // The per-item half of RecordParallel: runs on whichever thread
        // ParallelFor hands the item to.
        void RecordParallelItem(VulkanWorkerRecordingContext& item, const std::function<void(u32 item)>& body,
                                std::exception_ptr& firstFailure, std::mutex& failureMutex);
        // The fork's attachment pre-transition (amendment (92) rule 5): bring
        // the bound target's colour / depth / selected depth-array layer to
        // their attachment layouts on the primary, so items open their scopes
        // with identity transitions.
        void TransitionBoundTargetAttachmentsForFork();
        void TransitionSeededSampledImagesForFork();

        FrameBackbuffer m_Backbuffer;     ///< Live only inside a frame-callback recording.
        bool m_BackbufferWritten = false; ///< A backbuffer scope opened this recording.
        // CreateDepthArrayCompareOffViewHandle's per-image cache (see the
        // definition for the lifetime contract).
        std::unordered_map<u64, RHI::ResourceHandle> m_CompareOffViewHandles;

        // Stub bookkeeping may be touched from RecordParallel workers (a
        // refused entry point counts as a stub hit), hence the mutex.
        mutable std::mutex m_StubMutex;
        // Unfed STORAGE bindings that took the null-block substitution
        // (issue #1052). Atomic, not mutex-guarded like the stub tally: the
        // publication site runs per draw and may fork across recording workers.
        mutable std::atomic<u64> m_UnfedStorageBindings{ 0 };
        mutable u64 m_UnimplementedStubHits = 0;
        mutable std::array<u64, static_cast<sizet>(StubKind::Count)> m_StubHitsByKind{};
        mutable std::unordered_set<std::string> m_WarnedStubs;

        VulkanRecordedPipelineState m_State;

        // Cached at Init() from the physical device.
        u32 m_MaxUniformBlockSize = 16384;
        // limits.timestampPeriod: nanoseconds per timestamp tick, cached at
        // Init so query readback (Timestamp / TimeElapsed scaling to the
        // facade's nanosecond contract) never touches the physical device.
        f64 m_TimestampPeriodNs = 1.0;
        u32 m_MaxFramebufferSamples = 1;
        u32 m_MaxColorTextureSamples = 1;
        u32 m_MaxDepthTextureSamples = 1;
        bool m_SupportsInt64Atomics = false;
        bool m_LimitsCached = false;
        // Barrier stage masks may only name stages whose device features are
        // ENABLED (VUID-…-03929/-03930): all-ones minus the tessellation /
        // geometry bits when VulkanDevice did not enable those features.
        // ANDed onto every per-resource barrier's stage masks; the catch-all
        // bits (ALL_COMMANDS / ALL_TRANSFER) are single distinct bits and
        // pass through untouched.
        VkPipelineStageFlags2 m_EnabledStageMask = ~VkPipelineStageFlags2{ 0 };
        void CacheDeviceLimits();
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
