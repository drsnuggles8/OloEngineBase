#pragma once

// =============================================================================
// VulkanRendererAPI — the Vulkan implementation of the RendererAPI facade
// (issue #691 Phase 5).
//
// Phase 5 scope: the render graph's EXECUTION layer — barrier batches
// (IssueBarrierBatch via VulkanBarrierLowering + the layout tracker),
// transient clears (the poison instrument's ClearTexture*/ClearBuffer*),
// dynamic viewport/scissor, device queries, and debug labels. Everything
// pipeline-shaped (draws, shader binds, buffer lifecycle, queries, fences)
// is a WARN-ONCE stub naming Phase 6 — deliberately loud, never silent,
// which is what ADR 0011 amendment (42) demanded of the first
// VulkanRendererAPI ("~100 no-op virtuals inviting silent fallthrough" was
// the reason Phase 4 refused to build one; the stubs' warn-once counters are
// the mitigation).
//
// Recording model: Vulkan has no GL-style implicit current context — every
// vkCmd* needs a live VkCommandBuffer. Whoever drives execution (the
// device-gated execution test now, VulkanContext's frame loop when the graph
// is wired into it) brackets work in BeginRecording/EndRecording. A
// recording-dependent entry point called outside a bracket warn-once skips.
// =============================================================================

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RendererAPI.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#include <volk.h>

#include <array>
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

    // The render-target half of a thin PSO's baked state (attachment
    // formats + sample count). Lives HERE beside VulkanRecordedPipelineState
    // — both are recorded state VulkanPipelineBuilder consumes, and the
    // builder's header includes this one (declaring it there would cycle).
    struct VulkanRenderTargetDesc
    {
        u32 ColorCount = 0;
        std::array<VkFormat, 8> ColorFormats{};
        VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
        u32 Samples = 1;
    };

    // Pipeline-key state recorded from the facade's state-setter entry
    // points. Vulkan bakes nearly all of this into the VkPipeline, so
    // Phase 5 RECORDS it for Phase 6's pipeline-key derivation instead of
    // stubbing the setters — a state packet is a real dispatch, not a
    // Phase 6 loss (the execution test pins that state packets never touch
    // the stub counter).
    struct VulkanRecordedPipelineState
    {
        static constexpr u32 kMaxAttachments = 8;

        glm::vec4 ClearColor{ 0.0f };
        f32 ClearDepth = 1.0f;
        bool DepthTest = true;
        bool DepthWrite = true;
        RHI::CompareOp DepthFunc = RHI::CompareOp::Less;
        bool Blend = false;
        RHI::BlendFactor BlendSrcRGB = RHI::BlendFactor::One;
        RHI::BlendFactor BlendDstRGB = RHI::BlendFactor::Zero;
        RHI::BlendFactor BlendSrcAlpha = RHI::BlendFactor::One;
        RHI::BlendFactor BlendDstAlpha = RHI::BlendFactor::Zero;
        RHI::BlendOp BlendEquation = RHI::BlendOp::Add;
        bool StencilTest = false;
        RHI::CompareOp StencilFunc = RHI::CompareOp::Always;
        i32 StencilRef = 0;
        u32 StencilReadMask = 0xFFFFFFFFu;
        u32 StencilWriteMask = 0xFFFFFFFFu;
        RHI::StencilOp StencilFail = RHI::StencilOp::Keep;
        RHI::StencilOp StencilDepthFail = RHI::StencilOp::Keep;
        RHI::StencilOp StencilPass = RHI::StencilOp::Keep;
        bool Culling = false;
        RHI::CullMode CullFace = RHI::CullMode::Back;
        RHI::FrontFace FrontFaceWinding = RHI::FrontFace::CounterClockwise;
        RHI::PolygonMode PolygonMode = RHI::PolygonMode::Fill;
        bool PolygonOffsetEnabled = false;
        f32 PolygonOffsetFactor = 0.0f;
        f32 PolygonOffsetUnits = 0.0f;
        bool ScissorTest = false;
        bool Multisampling = true;
        f32 LineWidth = 1.0f;
        bool ColorMask[4] = { true, true, true, true };
        u32 PatchVertexCount = 3;

        bool AttachmentBlend[kMaxAttachments] = {};
        RHI::BlendFactor AttachmentBlendSrc[kMaxAttachments] = {};
        RHI::BlendFactor AttachmentBlendDst[kMaxAttachments] = {};
        // GL parity (#691 Phase 7 Wave A, found by the OITResolve tenant):
        // glEnablei(GL_BLEND, i) alone does NOT give buffer i its own blend
        // func — the GLOBAL glBlendFunc applies until glBlendFunci names the
        // buffer. A pass that per-attachment-ENABLES but sets only the global
        // func (OITResolve) must therefore blend with the global factors, not
        // the never-written per-attachment defaults (Zero/Zero). This flag
        // records "SetBlendFuncForAttachment was called for i"; the global
        // SetBlendFunc/SetBlendFuncSeparate clear it (glBlendFunc overwrites
        // every buffer's func in GL).
        bool AttachmentBlendFuncSet[kMaxAttachments] = {};
        u8 AttachmentColorMask[kMaxAttachments] = { 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF };
    };

    // -------------------------------------------------------------------------
    // VulkanQueryRegistry — the object-less occlusion-query family behind the
    // CreateQueries / BeginQuery / EndQuery / GetQueryResult* / DeleteQueries
    // facade entries (#691 Phase 7 Wave C, ADR item A6).
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
            bool Recorded = false; ///< a Begin/End pair reached a command buffer
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

        // --- Phase 5 recording bracket (backend-internal, not facade) -----
        void BeginRecording(VkCommandBuffer cmd);
        void EndRecording();
        [[nodiscard]] VkCommandBuffer CurrentCommandBuffer() const
        {
            return m_Cmd;
        }
        [[nodiscard]] VulkanImageLayoutTracker& LayoutTracker()
        {
            return m_LayoutTracker;
        }

        // Observability for the execution test: how many packets/entry points
        // hit a Phase 6 stub (nothing may fall through silently).
        [[nodiscard]] u64 GetPhase6StubHitCount() const
        {
            return m_Phase6StubHits;
        }

        // Draw observability (issue #691 Phase 7): PrepareDraw's failure
        // paths drop the draw with at most a warn-once — a fixture asserting
        // "N draws prepared, 0 dropped" turns a silently black frame into a
        // named failure. Reset by BeginRecording.
        [[nodiscard]] u32 GetPreparedDrawsThisRecording() const
        {
            return m_PreparedDrawsThisRecording;
        }
        [[nodiscard]] u32 GetDroppedDrawsThisRecording() const
        {
            return m_DroppedDrawsThisRecording;
        }
        // Draws the host-side conditional-render predicate skipped. Kept apart
        // from the dropped counter on purpose: a conditional skip is the
        // requested behaviour, a drop is a failure.
        [[nodiscard]] u32 GetConditionallySkippedDrawsThisRecording() const
        {
            return m_ConditionallySkippedDrawsThisRecording;
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
        void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer) override;
        void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer, u32 indirectOffsetBytes, RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes, u32 maxDrawCount, u32 strideBytes) override;
        void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier(MemoryBarrierFlags flags) override;
        void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers) override;
        void BindDefaultFramebuffer() override;
        void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height) override;
        void BindTexture(u32 slot, RHI::ResourceHandle texture) override;
        void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered, u32 layer, RHI::Access access, RHI::Format format) override;
        void SetPolygonOffset(f32 factor, f32 units) override;
        void EnableMultisampling() override;
        void DisableMultisampling() override;
        void SetColorMask(bool red, bool green, bool blue, bool alpha) override;
        void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha) override;
        void SetBlendStateForAttachment(u32 attachment, bool enabled) override;
        void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) override;
        void CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget, RHI::ResourceHandle dst, TextureTargetType dstTarget, u32 width, u32 height) override;
        void CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ, RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ, u32 width, u32 height) override;
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
        void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value) override;
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
        void TextureBarrier() override;
        void CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries) override;
        void DeleteQueries(std::span<const RHI::ResourceHandle> queries) override;
        void BeginQuery(RHI::QueryType type, RHI::ResourceHandle query) override;
        void EndQuery(RHI::QueryType type) override;
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

      private:
        // Const: several facade getters are const-qualified and still must
        // count their stub hit (nothing may fall through silently).
        void Phase6Stub(const char* entryPoint) const;

        // --- Phase 7: lazy dynamic-rendering scope + draw assembly ----------
        //
        // GL passes freely interleave framebuffer binds, state calls, clears,
        // draws, barriers and copies; Vulkan forbids most non-draw commands
        // inside a vkCmdBeginRendering scope. The scope is therefore LAZY:
        // vkCmdBeginRendering is deferred to the first draw against the
        // currently-published framebuffer (VulkanBindingState), Clear()
        // before that folds into the attachments' loadOp, and any barrier /
        // clear-resource / copy / dispatch / target-change ENDS the scope
        // (the next draw resumes with LOAD_OP_LOAD). This is what lets
        // unmodified GL-shaped pass bodies record legal Vulkan.
        struct RenderingScope
        {
            bool Active = false;
            VulkanFramebuffer* Target = nullptr; ///< The scope's framebuffer (never null while Active).
            bool PendingClearColor = false;      ///< Fold into loadOp at scope begin.
            bool PendingClearDepth = false;
        };

        // GL's glNamedFramebufferDrawBuffers / ReadBuffer are PER-FRAMEBUFFER
        // PERSISTENT state, and both the bound form (SetDrawBuffers) and the
        // raw-handle form (SetFramebufferDrawAttachments) mutate the SAME
        // state on GL — so this backend models one map, keyed by the FB's
        // RHI handle, that both forms write (#691 Phase 7 Wave C; replaces
        // the earlier scope-transient DrawList, which forgot the selection on
        // every target switch and could not express the raw form at all).
        // The scope build consumes DrawList at vkCmdBeginRendering; blits
        // consume ReadAttachment (src) and DrawList (dst fan-out).
        // Entries for destroyed framebuffers linger harmlessly: the key packs
        // the generation, so a recycled FB index gets a fresh entry.
        struct FramebufferAttachmentSelection
        {
            // DrawList[i] = attachment index feeding fragment output location
            // i (RHI::NoAttachment → UNUSED). Count 0 = identity over every
            // color attachment (the engine-FB creation default on GL).
            std::array<u32, 8> DrawList{};
            u32 DrawListCount = 0;
            u32 ReadAttachment = 0; ///< glNamedFramebufferReadBuffer's selection.
        };

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
        // Bind the VAO's index buffer if it changed (redundant-bind cache,
        // reset per recording). False when the VAO has no index buffer.
        [[nodiscard]] bool BindIndexBufferFor(const VulkanVertexArray* vao);
        // Handle -> VkBuffer for the indirect-draw family (#691 Wave C). Any
        // Vulkan-backend buffer identity resolves (its registry native IS the
        // VkBuffer); null + a counted stub on kind mismatch / stale handles.
        [[nodiscard]] VkBuffer ResolveIndirectBuffer(RHI::ResourceHandle indirectBuffer, const char* entryPoint) const;
        // Root-struct assembly + arena push + vkCmdPushDataEXT — shared by
        // draws and dispatches (§4: one contract, no compute special case).
        // Kind-aware: CombinedImageSampler bindings read the TEXTURE slot
        // table, StorageImage bindings the IMAGE-UNIT table (amendment (29):
        // two namespaces), buffers the bind-point tables (57 = vertex pull).
        [[nodiscard]] bool AssembleAndPushRootData(const VulkanRootDataLayout& layout, const char* shaderName,
                                                   const VulkanVertexArray* vao);

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

        RenderingScope m_Scope;
        std::unordered_map<u64, FramebufferAttachmentSelection> m_FramebufferSelections;
        // CreateDepthArrayCompareOffViewHandle's per-image cache (see the
        // definition for the lifetime contract).
        std::unordered_map<u64, RHI::ResourceHandle> m_CompareOffViewHandles;
        VulkanRenderTargetDesc m_ScopeTargets; ///< Valid while m_Scope.Active.
        VkBuffer m_BoundIndexBuffer = VK_NULL_HANDLE;
        bool m_HeapBoundThisRecording = false;
        u32 m_PreparedDrawsThisRecording = 0;
        u32 m_DroppedDrawsThisRecording = 0;
        VulkanVertexArray* m_BoundVertexArray = nullptr; ///< BindVertexArrayRaw's publication.
        std::vector<u8> m_RootScratch;
        // Recorded scissor box (the WITH_COUNT dynamic state is emitted by
        // the draw front-end, not by the setter — see SetScissorBox).
        VkRect2D m_ScissorRect{};
        bool m_ScissorRectSet = false;
        // The occlusion query BeginQuery opened (GL keeps one per target;
        // EndQuery carries no handle, so the pair is tracked here).
        struct ActiveQuery
        {
            VkQueryPool Pool = VK_NULL_HANDLE;
            u32 Index = 0;
        };
        ActiveQuery m_ActiveQuery{};
        // Host-side conditional-render predicate: true between
        // BeginConditionalRender(occluded query) and EndConditionalRender, and
        // every draw in that window is skipped (see BeginConditionalRender for
        // why the predicate is evaluated on the host, not in a VK_EXT buffer).
        bool m_ConditionalRenderSkip = false;
        u32 m_ConditionallySkippedDrawsThisRecording = 0;

        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        VulkanImageLayoutTracker m_LayoutTracker;
        Viewport m_Viewport{};

        mutable u64 m_Phase6StubHits = 0;
        mutable std::unordered_set<std::string> m_WarnedStubs;

        VulkanRecordedPipelineState m_State;

        // Cached at Init() from the physical device.
        u32 m_MaxUniformBlockSize = 16384;
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
