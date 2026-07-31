#pragma once

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>
#include <span>
#include <string_view>

namespace OloEngine
{
    struct Viewport
    {
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    class RendererAPI
    {
      public:
        enum class API
        {
            None = 0,
            OpenGL = 1
        };

        enum class RendererType
        {
            None = 0,
            Renderer3D
        };

        // Renderer-agnostic texture target types (converted to GL enums by the backend)
        enum class TextureTargetType : u8
        {
            Texture2D = 0,
            TextureCubeMap,
            // Added by the Phase 2 step-2 sweep (issue #691): the per-sample
            // MSAA paths copy *multisample* G-Buffer attachments, and a
            // multisample image cannot be copied as if it were a plain 2D one —
            // glCopyImageSubData requires matching targets and Vulkan requires
            // matching VkImageCreateInfo::samples. Without this member those
            // call sites had to keep a raw GL_TEXTURE_2D_MULTISAMPLE.
            Texture2DMultisample
        };

      public:
        virtual ~RendererAPI() = default;

        virtual void Init() = 0;
        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;
        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear() = 0;
        virtual void ClearDepthOnly() = 0;
        virtual void ClearColorAndDepth() = 0;
        virtual Viewport GetViewport() const = 0;

        virtual void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) = 0;
        virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount) = 0;
        virtual void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 instanceCount) = 0;
        virtual void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) = 0;
        virtual void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices) = 0;

        // Raw VAO ID overloads for POD shadow casters (no Ref<VertexArray> available)
        virtual void DrawIndexedRaw(u32 vaoID, u32 indexCount) = 0;
        virtual void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex) = 0;
        // Instanced raw variant for batched shadow casters that share VAO + submesh range.
        virtual void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount) = 0;
        virtual void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices) = 0;

        virtual void SetLineWidth(f32 width) = 0;

        virtual void EnableCulling() = 0;
        virtual void DisableCulling() = 0;
        virtual void FrontCull() = 0;
        virtual void BackCull() = 0;
        // CullMode::None is not a face — culling-off is DisableCulling().
        virtual void SetCullFace(RHI::CullMode face) = 0;
        virtual void SetDepthMask(bool value) = 0;
        virtual void SetDepthTest(bool value) = 0;
        virtual void SetDepthFunc(RHI::CompareOp func) = 0;
        virtual void SetBlendState(bool value) = 0;
        virtual void SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor) = 0;
        virtual void SetBlendEquation(RHI::BlendOp mode) = 0;

        virtual void EnableStencilTest() = 0;
        virtual void DisableStencilTest() = 0;
        virtual bool IsStencilTestEnabled() const = 0;
        virtual void SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask) = 0;
        virtual void SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass) = 0;
        virtual void SetStencilMask(u32 mask) = 0;
        virtual void ClearStencil() = 0;

        // No face parameter, deliberately. Core-profile glPolygonMode accepts
        // only GL_FRONT_AND_BACK (anything else is GL_INVALID_ENUM) and every
        // call site in the engine passed it; Vulkan's polygonMode has no face
        // either. Carrying one would re-export a wart GL itself deprecated.
        virtual void SetPolygonMode(RHI::PolygonMode mode) = 0;

        virtual void EnableScissorTest() = 0;
        virtual void DisableScissorTest() = 0;
        virtual void SetScissorBox(i32 x, i32 y, u32 width, u32 height) = 0;

        // Indirect draw calls (GPU-driven rendering)
        virtual void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) = 0;
        virtual void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) = 0;
        // Raw-VAO variant used by the GPU-frustum-cull path which only has a
        // RendererID (the dispatcher's BindVAOIfNeeded() cache populates it).
        virtual void DrawElementsIndirectRaw(u32 vaoID, u32 indirectBufferID) = 0;
        // Multi-draw indirect with a GPU-sourced draw count (core GL 4.6, issue #629):
        // reads DrawElementsIndirectCommand records from indirectBufferID starting at
        // indirectOffsetBytes and the u32 draw count from parameterBufferID at
        // parameterOffsetBytes; maxDrawCount caps the count, strideBytes is the
        // command record stride.
        virtual void MultiDrawElementsIndirectCountRaw(u32 vaoID, u32 indirectBufferID, u32 indirectOffsetBytes,
                                                       u32 parameterBufferID, u32 parameterOffsetBytes,
                                                       u32 maxDrawCount, u32 strideBytes) = 0;

        // Compute shader dispatch
        virtual void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;
        virtual void MemoryBarrier(MemoryBarrierFlags flags) = 0;

        // New methods for render graph
        virtual void BindDefaultFramebuffer() = 0;
        virtual void BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height) = 0;
        virtual void BindTexture(u32 slot, u32 textureID) = 0;

        // ---------------------------------------------------------------------
        // Handle-taking siblings of the bind family (issue #691 step 3, slice 2).
        //
        // These are ADDED beside the u32 forms rather than replacing them,
        // because the conversion is asymmetric: `handle -> native` is a registry
        // lookup, but `native -> handle` is not recoverable. A facade taking only
        // handles could not serve a caller still holding a u32, so both must
        // coexist until the last caller migrates — then the u32 forms are deleted
        // and `facade_native_id_params` reaches zero (ADR 0011 step-3 amendments).
        //
        // Every implementation resolves through Utils::ResolveNative INSIDE
        // Platform/<Backend>/. A resolving helper in Renderer/ would be simpler
        // and would breach the boundary this phase exists to close —
        // RHIBoundaryRatchetTest's backend_resolve_hatch is what keeps it honest.
        // ---------------------------------------------------------------------
        virtual void BindTexture(u32 slot, RHI::ResourceHandle texture) = 0;
        virtual void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer,
                                      RHI::Access access, RHI::Format format) = 0;
        virtual void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                                      u32 layer, RHI::Access access, RHI::Format format) = 0;

        virtual void SetPolygonOffset(f32 factor, f32 units) = 0;
        virtual void EnableMultisampling() = 0;
        virtual void DisableMultisampling() = 0;
        virtual void SetColorMask(bool red, bool green, bool blue, bool alpha) = 0;
        virtual void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha) = 0;

        // Per-attachment blend control (needed for mixed integer/float framebuffer attachments)
        virtual void SetBlendStateForAttachment(u32 attachment, bool enabled) = 0;
        // Per-attachment blend function (needed for weighted-blended OIT — accum/revealage differ)
        virtual void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) = 0;

        // GPU-side image copy (used for staging textures to avoid read-write hazards)
        virtual void CopyImageSubData(u32 srcID, TextureTargetType srcTarget, u32 dstID, TextureTargetType dstTarget,
                                      u32 width, u32 height) = 0;
        // Full image copy with source/dest offsets (needed for cubemap face copies)
        virtual void CopyImageSubDataFull(u32 srcID, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                          u32 dstID, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                          u32 width, u32 height) = 0;
        // Copy from currently-bound READ framebuffer to a named texture
        virtual void CopyFramebufferToTexture(u32 textureID, u32 width, u32 height) = 0;

        // Restrict which color attachments are written to
        virtual void SetDrawBuffers(std::span<const u32> attachments) = 0;
        // Restore all color attachments for drawing (convenience for post-pass cleanup)
        virtual void RestoreAllDrawBuffers(u32 colorAttachmentCount) = 0;

        // Texture lifecycle abstractions (avoid raw gl* calls in passes)
        virtual u32 CreateTexture2D(u32 width, u32 height, RHI::Format internalFormat) = 0;
        virtual u32 CreateTextureCubemap(u32 width, u32 height, RHI::Format internalFormat) = 0;
        // Create a GL_TEXTURE_2D_ARRAY *view* aliasing the storage of an existing
        // immutable depth array, but with hardware depth comparison DISABLED, so
        // it can be sampled as a plain sampler2DArray to read raw depth (needed by
        // the PCSS blocker search, which the comparison sampler2DArrayShadow can't
        // provide). Source must be DEPTH_COMPONENT32F immutable storage. Returns 0
        // if the platform lacks texture-view support.
        virtual u32 CreateDepthArrayCompareOffView(u32 srcTextureID, u32 numLayers) = 0;
        // Replaces SetTextureParameter(id, GLenum pname, GLint value). `pname`
        // was an open-ended GL enum space, and mirroring it with an
        // RHI::TextureParameterName would have re-exported GL under a new name.
        // Every call site in the engine sets exactly min/mag filter and wrap
        // S/T/R, so two intent-named setters cover all of them; SetTextureWrap
        // applies one mode to all three axes because no call site ever used
        // different modes per axis (WRAP_R is inert on a 2D target).
        virtual void SetTextureFilter(u32 textureID, RHI::Filter minFilter, RHI::Filter magFilter) = 0;
        virtual void SetTextureWrap(u32 textureID, RHI::AddressMode wrap) = 0;
        // `sourceFormat` describes the layout of `data` — the CPU-side buffer —
        // NOT the texture's storage format. GL converts on upload, and the
        // engine relies on that: SSAO's noise texture is RG16Float storage fed
        // from RG32Float host data.
        virtual void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                             RHI::Format sourceFormat, const void* data) = 0;
        virtual void DeleteTexture(u32 textureID) = 0;

        // Occlusion / conditional rendering
        virtual void BeginConditionalRender(u32 queryID) = 0;
        virtual void EndConditionalRender() = 0;

        // =====================================================================
        // Phase 2 step 2 additions (issue #691) — the operations the sweep
        // found the facade had never abstracted at all.
        //
        // Step 1 converted the vocabulary of the 74 virtuals that already
        // existed. This block is the other half of the finding: 84 distinct GL
        // entry points appear across the 313 swept call sites and ~60% of them
        // had NO facade equivalent, so passes reached past it. See ADR 0011's
        // "Amendments from Phase 2 step 2" for the category table and the
        // reasoning behind each shape.
        // =====================================================================

        // --- Buffer binding points -------------------------------------------
        // The single biggest gap (26 call sites). A 0 id unbinds the point.
        virtual void BindUniformBuffer(u32 bindingPoint, u32 bufferID) = 0;
        virtual void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) = 0;
        virtual void BindStorageBuffer(u32 bindingPoint, u32 bufferID) = 0;
        virtual void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) = 0;

        // --- Program / VAO / framebuffer binding ------------------------------
        // The POD command dispatcher holds a raw program id by design (it
        // resolves materials to renderer IDs at build time and has no
        // Ref<Shader> on hand), so Shader::Bind() cannot serve it. 0 unbinds.
        virtual void BindShaderProgram(u32 programID) = 0;
        virtual void BindShaderProgram(RHI::ResourceHandle program) = 0;
        virtual void BindVertexArrayRaw(u32 vaoID) = 0;
        virtual void BindVertexArrayRaw(RHI::ResourceHandle vertexArray) = 0;
        // 0 selects the default framebuffer — same as BindDefaultFramebuffer().
        virtual void BindFramebuffer(u32 framebufferID) = 0;
        virtual void BindFramebuffer(RHI::ResourceHandle framebuffer) = 0;

        // --- Draws from already-bound geometry --------------------------------
        // Distinct from the DrawIndexedRaw(vaoID, ...) family above, which binds
        // its own VAO: CommandDispatch keeps a redundant-bind cache, so a draw
        // that re-binds would defeat it. This is also the NATIVE Vulkan shape
        // (vkCmdBindIndexBuffer then vkCmdDrawIndexed) — the combined
        // bind-and-draw form is the less portable of the two. Topology and index
        // width are explicit rather than hard-coded to triangles / u32.
        virtual void DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount,
                                      RHI::IndexType indexType, u32 baseIndex) = 0;
        virtual void DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount,
                                               RHI::IndexType indexType, u32 baseIndex,
                                               u32 instanceCount) = 0;
        virtual void DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount) = 0;
        // Split out rather than folded into a patch-draw variant: the
        // tessellation call sites set the count once and then draw many times.
        virtual void SetPatchVertexCount(u32 patchVertices) = 0;

        // --- Pipeline state the facade was missing -----------------------------
        virtual void SetFrontFace(RHI::FrontFace face) = 0;
        virtual void SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB,
                                          RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha) = 0;
        virtual void SetClearDepth(f32 depth) = 0;

        // --- Named framebuffers ------------------------------------------------
        // SetDrawBuffers/RestoreAllDrawBuffers above act on the CURRENTLY BOUND
        // framebuffer; every swept call site names a specific one through DSA.
        //
        // `attachmentIndices[i]` is the attachment written by draw slot i, or
        // RHI::NoAttachment for "slot i writes nowhere" — which is not an index
        // and which both backends need (GL_NONE / VK_ATTACHMENT_UNUSED).
        // DecalRenderPass depends on it to steer a decal into exactly one
        // G-Buffer attachment.
        virtual u32 CreateFramebuffer() = 0;
        virtual void DeleteFramebuffer(u32 framebufferID) = 0;
        virtual void AttachFramebufferColorTexture(u32 framebufferID, u32 attachmentIndex,
                                                   u32 textureID, u32 mipLevel) = 0;
        virtual void AttachFramebufferDepthTexture(u32 framebufferID, u32 textureID, u32 mipLevel) = 0;
        [[nodiscard("Store this!")]] virtual bool IsFramebufferComplete(u32 framebufferID) = 0;
        virtual void SetFramebufferDrawAttachments(u32 framebufferID, std::span<const u32> attachmentIndices) = 0;
        // The identity list { 0, 1, ... count-1 } — "draw to every colour
        // attachment this framebuffer has". Nine call sites were open-coding the
        // same std::array + fill loop + span; that is the named-framebuffer
        // counterpart of RestoreAllDrawBuffers(u32) above, which already existed
        // for the BOUND framebuffer. Restoring a narrower list than the target
        // actually has silently drops later shader outputs (PBR_MultiLight's
        // motion vector at location 3, breaking TAA), which is exactly the kind
        // of off-by-one an open-coded loop invites.
        virtual void RestoreAllFramebufferDrawAttachments(u32 framebufferID, u32 colorAttachmentCount) = 0;
        virtual void SetFramebufferReadAttachment(u32 framebufferID, u32 attachmentIndex) = 0;
        virtual void ClearFramebufferColorAttachment(u32 framebufferID, u32 attachmentIndex,
                                                     const glm::vec4& color) = 0;
        virtual void ClearFramebufferDepth(u32 framebufferID, f32 depth) = 0;
        virtual void BlitFramebuffer(u32 srcFramebufferID, u32 dstFramebufferID,
                                     i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                     i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                     RHI::BlitAspect aspect, RHI::Filter filter) = 0;

        // --- Raw buffer lifecycle ----------------------------------------------
        // UniformBuffer / StorageBuffer wrap *their own* buffers; VirtualMeshRegistry
        // hand-rolls a vertex/index arena plus a persistent-mapped upload ring and
        // needs the primitives directly.
        virtual u32 CreateBuffer() = 0;
        virtual void DeleteBuffer(u32 bufferID) = 0;
        // Mutable storage — re-callable to resize.
        virtual void AllocateBufferStorage(u32 bufferID, u64 sizeBytes, RHI::MemoryResidency residency) = 0;
        // Immutable storage + a persistent, coherent WRITE mapping in one step:
        // the only mapping mode the engine uses, so splitting it would invite a
        // storage/mapping flag mismatch that GL only reports at map time.
        // Returns the CPU pointer, or nullptr if the mapping failed.
        virtual void* AllocatePersistentUploadStorage(u32 bufferID, u64 sizeBytes) = 0;
        virtual void UnmapBuffer(u32 bufferID) = 0;
        virtual void UploadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, const void* data) = 0;
        virtual void ReadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, void* dest) = 0;
        virtual void CopyBufferSubData(u32 srcBufferID, u32 dstBufferID,
                                       u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes) = 0;
        virtual void ClearBufferUInt(u32 bufferID, u32 value) = 0;
        virtual void ClearBufferFloat(u32 bufferID, f32 value) = 0;

        // --- Vertex array lifecycle ---------------------------------------------
        virtual u32 CreateVertexArray() = 0;
        virtual void SetVertexArrayIndexBuffer(u32 vaoID, u32 bufferID) = 0;
        virtual void DeleteVertexArray(u32 vaoID) = 0;

        // --- Texture clear / upload / readback -----------------------------------
        // Two clears rather than one type-punned value pointer, mirroring
        // VkClearColorValue's float/uint union members. `mipLevel` clears one
        // level of every layer/face, matching glClearTexImage.
        virtual void ClearTextureFloat(u32 textureID, u32 mipLevel, const glm::vec4& color) = 0;
        virtual void ClearTextureUInt(u32 textureID, u32 mipLevel, u32 value) = 0;
        // Offset overloads of the whole-image UploadTextureSubImage2D above.
        // `sourceFormat` is the HOST buffer's layout, not the texture's storage
        // format — see ADR 0011 amendment (4).
        virtual void UploadTextureSubImage2D(u32 textureID, i32 xOffset, i32 yOffset,
                                             u32 width, u32 height,
                                             RHI::Format sourceFormat, const void* data) = 0;
        virtual void UploadTextureSubImage3D(u32 textureID, i32 xOffset, i32 yOffset, i32 zOffset,
                                             u32 width, u32 height, u32 depth,
                                             RHI::Format sourceFormat, const void* data) = 0;
        // Readbacks return success rather than leaving the caller to ask the
        // backend for an error: GL's error model is a global sticky flag and
        // Vulkan's is a per-call result, so exposing either would force the
        // other backend to fake it. ThumbnailCapture's glGetError() disappears
        // with no replacement (ADR 0011 amendment (7)).
        [[nodiscard("Store this!")]] virtual bool ReadTextureImage(u32 textureID, u32 mipLevel,
                                                                   RHI::Format destFormat,
                                                                   sizet destSizeBytes, void* dest) = 0;
        [[nodiscard("Store this!")]] virtual bool ReadTextureSubImage(u32 textureID, u32 mipLevel,
                                                                      i32 x, i32 y, i32 z,
                                                                      u32 width, u32 height, u32 depth,
                                                                      RHI::Format destFormat,
                                                                      sizet destSizeBytes, void* dest) = 0;
        virtual void GetTextureDimensions(u32 textureID, u32 mipLevel, u32& outWidth, u32& outHeight) = 0;
        // Orders a texture's use as a render target against a subsequent sample
        // of it in the same pass. Vulkan expresses this as a pipeline barrier.
        virtual void TextureBarrier() = 0;

        // --- Queries --------------------------------------------------------------
        virtual void CreateQueries(RHI::QueryType type, std::span<u32> outQueryIDs) = 0;
        virtual void DeleteQueries(std::span<const u32> queryIDs) = 0;
        virtual void BeginQuery(RHI::QueryType type, u32 queryID) = 0;
        virtual void EndQuery(RHI::QueryType type) = 0;
        [[nodiscard("Store this!")]] virtual bool IsQueryResultAvailable(u32 queryID) = 0;
        [[nodiscard("Store this!")]] virtual u32 GetQueryResultU32(u32 queryID) = 0;
        [[nodiscard("Store this!")]] virtual u64 GetQueryResultU64(u32 queryID) = 0;

        // --- Fences ---------------------------------------------------------------
        // An opaque u64 rather than a handle type: GLsync is a pointer and
        // VkFence a 64-bit handle, and FrameResourceManager stores one per
        // in-flight frame. 0 means "no fence" / creation failed.
        [[nodiscard("Store this!")]] virtual u64 CreateFence() = 0;
        [[nodiscard("Store this!")]] virtual RHI::FenceStatus ClientWaitFence(u64 fence, u64 timeoutNanoseconds) = 0;
        [[nodiscard("Store this!")]] virtual bool IsFenceSignaled(u64 fence) = 0;
        virtual void DestroyFence(u64 fence) = 0;

        // --- Debug markers ----------------------------------------------------------
        virtual void PushDebugGroup(u32 id, std::string_view label) = 0;
        virtual void PopDebugGroup() = 0;

        // --- Device ------------------------------------------------------------------
        // Full CPU/GPU sync. Expensive by construction — the two callers are an
        // IBL precompute and a virtual-geometry ring-buffer wrap.
        virtual void WaitForDeviceIdle() = 0;

        // MSAA capability caps. Three separate queries because GL reports them
        // separately and they genuinely differ on some drivers: a format may
        // support more colour samples than depth samples, and GBuffer must pick
        // a count both attachments can carry.
        [[nodiscard("Store this!")]] virtual u32 GetMaxFramebufferSamples() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetMaxColorTextureSamples() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetMaxDepthTextureSamples() const = 0;

        // Name-keyed default-block uniform. THIS IS THE ONE VIRTUAL A VULKAN
        // BACKEND CANNOT IMPLEMENT FAITHFULLY — SPIR-V has push constants and
        // UBO members, not a queryable default uniform block. It has exactly one
        // call site (CommandDispatch::DrawInfiniteGrid's u_GridScale) and Phase 6
        // must fold that into a UBO and delete this. Recorded deliberately in
        // ADR 0011 amendment (9) rather than left to surprise Phase 7 bring-up.
        virtual void SetProgramUniformFloat(u32 programID, std::string_view name, f32 value) = 0;

        // GPU capability queries

        // True when the backend can service resource creation and draws *right
        // now*. This is not "has Renderer::Init run" — it asks whether the
        // underlying device is usable in this process at all.
        //
        // It exists because asset code legitimately runs without one: headless
        // harnesses (Functional tests, asset preprocessors) load fonts and
        // meshes for their CPU-side data and never render them. Those paths must
        // build the data and defer the GPU upload rather than crash, so they
        // need to ask the question before calling Texture2D::Create.
        //
        // Replaces a `glad_glCreateTextures != nullptr` probe in
        // SlugFontProcessor — reaching into the GL loader's symbol table is a
        // real need expressed unportably, and it is invisible to the boundary
        // ratchet's `gl[A-Z]` scan because the character after `gl` is `a`
        // (issue #691 Phase 2).
        [[nodiscard("Store this!")]] virtual bool IsDeviceAvailable() const = 0;

        [[nodiscard("Store this!")]] virtual u32 GetMaxUniformBlockSize() const = 0;
        // True when the driver exposes 64-bit shader integers AND 64-bit shader
        // atomics (GL_ARB_gpu_shader_int64 + GL_NV_shader_atomic_int64), which
        // lets the virtualized-geometry software rasterizer resolve its
        // visibility buffer with a single atomicMin on a packed uint64_t instead
        // of the portable two-pass 2x32 scheme (issue #629). Cached at Init.
        [[nodiscard("Store this!")]] virtual bool SupportsInt64ShaderAtomics() const = 0;

        [[nodiscard("Store this!")]] static API GetAPI()
        {
            return s_API;
        }
        static Scope<RendererAPI> Create();

      private:
        static API s_API;
    };

} // namespace OloEngine
