#pragma once
#include "OloEngine/Renderer/RendererAPI.h"

#include <glad/gl.h>

#include <array>

namespace OloEngine
{

    // Two kinds of member below, and the distinction is the whole point of
    // issue #691 Phase 2 step 3:
    //
    //   * the `override`s take RHI::ResourceHandle. They ARE the facade, and no
    //     caller above Platform/ can express anything else.
    //   * the plain (non-virtual) members take a native GL name. They are the
    //     backend's own implementation seam — the identity forms resolve through
    //     Utils::ResolveNativeAs and call into them. They are reachable only
    //     through OpenGLRendererAPI itself, which nothing outside
    //     RendererAPI::Create names, so they cannot leak a driver name upward.
    class OpenGLRendererAPI : public RendererAPI
    {
      public:
        void Init() override;
        void SetViewport(u32 x, u32 y, u32 width, u32 height) override;

        void SetClearColor(const glm::vec4& color) override;
        void Clear() override;
        void ClearDepthOnly() override;
        void ClearColorAndDepth() override;
        Viewport GetViewport() const override;

        void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0) override;
        void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount = 0, u32 instanceCount = 1) override;
        void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices) override;

        void DrawIndexedRaw(u32 vaoID, u32 indexCount);
        void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex);
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount) override;
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex) override;
        void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount);
        void DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex,
                                     u32 instanceCount) override;
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices);
        void DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 patchVertices) override;

        void SetLineWidth(f32 width) override;

        void EnableCulling() override;
        void DisableCulling() override;
        void SetCullFace(RHI::CullMode face) override;
        void FrontCull() override;
        void BackCull() override;
        void SetDepthMask(bool value) override;
        void SetDepthTest(bool value) override;
        void SetBlendState(bool value) override;
        void SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor) override;
        void SetBlendEquation(RHI::BlendOp mode) override;
        void SetDepthFunc(RHI::CompareOp func) override;
        void EnableStencilTest() override;
        void DisableStencilTest() override;
        bool IsStencilTestEnabled() const override;
        void SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask) override;
        void SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass) override;
        void SetStencilMask(u32 mask) override;
        void ClearStencil() override;

        void SetPolygonMode(RHI::PolygonMode mode) override;
        void SetPolygonOffset(f32 factor, f32 units) override;
        void EnableMultisampling() override;
        void DisableMultisampling() override;
        void SetColorMask(bool red, bool green, bool blue, bool alpha) override;
        void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha) override;

        void EnableScissorTest() override;
        void DisableScissorTest() override;
        void SetScissorBox(i32 x, i32 y, u32 width, u32 height) override;

        void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override;
        void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override;
        void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer) override;
        void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer,
                                               u32 indirectOffsetBytes,
                                               RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes,
                                               u32 maxDrawCount, u32 strideBytes) override;

        void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier(MemoryBarrierFlags flags) override;

        void BindDefaultFramebuffer() override;
        void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height) override;
        void BindTexture(u32 slot, u32 textureID);
        void BindTexture(u32 slot, RHI::ResourceHandle texture) override;
        void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer,
                              RHI::Access access, RHI::Format format);
        void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                              u32 layer, RHI::Access access, RHI::Format format) override;

        void SetBlendStateForAttachment(u32 attachment, bool enabled) override;
        void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) override;
        void CopyImageSubData(u32 srcID, TextureTargetType srcTarget, u32 dstID, TextureTargetType dstTarget,
                              u32 width, u32 height);
        void CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget,
                              RHI::ResourceHandle dst, TextureTargetType dstTarget,
                              u32 width, u32 height) override;
        void CopyImageSubDataFull(u32 srcID, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                  u32 dstID, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                  u32 width, u32 height);
        void CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                  RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                  u32 width, u32 height) override;
        void CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height) override;
        void SetDrawBuffers(std::span<const u32> attachments) override;
        void RestoreAllDrawBuffers(u32 colorAttachmentCount) override;
        u32 CreateTexture2D(u32 width, u32 height, RHI::Format internalFormat);
        u32 CreateTextureCubemap(u32 width, u32 height, RHI::Format internalFormat);
        u32 CreateDepthArrayCompareOffView(u32 srcTextureID, u32 numLayers);
        [[nodiscard]] RHI::ResourceHandle CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture,
                                                                               u32 numLayers) override;
        void SetTextureFilter(u32 textureID, RHI::Filter minFilter, RHI::Filter magFilter);
        void SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter) override;
        void SetTextureWrap(u32 textureID, RHI::AddressMode wrap);
        void SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap) override;
        void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                     RHI::Format sourceFormat, const void* data);
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height,
                                     RHI::Format sourceFormat, const void* data) override;
        void DeleteTexture(u32 textureID);

        void BeginConditionalRender(RHI::ResourceHandle query) override;
        void EndConditionalRender() override;

        // --- Phase 2 step 2 additions (issue #691) ---------------------------
        void BindUniformBuffer(u32 bindingPoint, u32 bufferID);
        void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override;
        void BindStorageBuffer(u32 bindingPoint, u32 bufferID);
        void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override;
        void BindShaderProgram(u32 programID);
        void BindShaderProgram(RHI::ResourceHandle program) override;
        void BindVertexArrayRaw(u32 vaoID);
        void BindVertexArrayRaw(RHI::ResourceHandle vertexArray) override;
        void BindFramebuffer(u32 framebufferID);
        void BindFramebuffer(RHI::ResourceHandle framebuffer) override;

        void DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount,
                              RHI::IndexType indexType, u32 baseIndex) override;
        void DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount,
                                       RHI::IndexType indexType, u32 baseIndex, u32 instanceCount) override;
        void DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount) override;
        void SetPatchVertexCount(u32 patchVertices) override;

        void SetFrontFace(RHI::FrontFace face) override;
        void SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB,
                                  RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha) override;
        void SetClearDepth(f32 depth) override;

        u32 CreateFramebuffer();
        void DeleteFramebuffer(u32 framebufferID);
        void AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                           RHI::ResourceHandle texture, u32 mipLevel) override;
        void AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture,
                                           u32 mipLevel) override;
        [[nodiscard("Store this!")]] bool IsFramebufferComplete(RHI::ResourceHandle framebuffer) override;
        void SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                           std::span<const u32> attachmentIndices) override;
        void RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                  u32 colorAttachmentCount) override;
        void SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex) override;
        void ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                             const glm::vec4& color) override;
        void ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth) override;
        void BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer,
                             i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                             i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                             RHI::BlitAspect aspect, RHI::Filter filter) override;

        u32 CreateBuffer();
        void DeleteBuffer(u32 bufferID);
        void AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes, RHI::MemoryResidency residency) override;
        void* AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes) override;
        void UnmapBuffer(RHI::ResourceHandle buffer) override;
        void UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, const void* data) override;
        void ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest) override;
        void CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer,
                               u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes) override;
        void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value) override;
        void ClearBufferFloat(RHI::ResourceHandle buffer, f32 value) override;

        u32 CreateVertexArray();
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
        void DeleteVertexArray(u32 vaoID);

        void ClearTextureFloat(u32 textureID, u32 mipLevel, const glm::vec4& color);
        void ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color) override;
        void ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value) override;
        // Offset overload; the whole-image one is declared above.
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset,
                                     u32 width, u32 height,
                                     RHI::Format sourceFormat, const void* data) override;
        void UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset,
                                     u32 width, u32 height, u32 depth,
                                     RHI::Format sourceFormat, const void* data) override;
        [[nodiscard("Store this!")]] bool ReadTextureImage(u32 textureID, u32 mipLevel,
                                                           RHI::Format destFormat,
                                                           sizet destSizeBytes, void* dest);
        [[nodiscard("Store this!")]] bool ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                           RHI::Format destFormat,
                                                           sizet destSizeBytes, void* dest) override;
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(u32 textureID, u32 mipLevel,
                                                              i32 x, i32 y, i32 z,
                                                              u32 width, u32 height, u32 depth,
                                                              RHI::Format destFormat,
                                                              sizet destSizeBytes, void* dest);
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                              i32 x, i32 y, i32 z,
                                                              u32 width, u32 height, u32 depth,
                                                              RHI::Format destFormat,
                                                              sizet destSizeBytes, void* dest) override;
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
        void SetProgramUniformFloat(u32 programID, std::string_view name, f32 value);
        void SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value) override;

        [[nodiscard("Store this!")]] bool IsDeviceAvailable() const override;
        [[nodiscard("Store this!")]] u32 GetMaxUniformBlockSize() const override;
        [[nodiscard("Store this!")]] bool SupportsInt64ShaderAtomics() const override
        {
            return m_SupportsInt64Atomics;
        }

      private:
        // Per-attachment colour write masks, mirrored from glColorMaski so a
        // colour clear can lift them (glClear obeys the colour mask exactly as
        // it obeys the depth/stencil write masks the Clear* paths already
        // guard) and put them back. Index == draw-buffer index; only the first
        // m_MaxDrawBuffers entries are meaningful.
        struct AttachmentColorMask
        {
            bool R = true, G = true, B = true, A = true;

            [[nodiscard]] bool IsFullyEnabled() const
            {
                return R && G && B && A;
            }
        };
        static constexpr u32 kMaxTrackedDrawBuffers = 8;
        std::array<AttachmentColorMask, kMaxTrackedDrawBuffers> m_AttachmentColorMasks{};
        // True while any tracked attachment has a non-default mask, so the
        // clear paths can skip the save/restore entirely in the common case.
        bool m_AnyAttachmentColorMaskDisabled = false;

        // Lift every per-attachment colour mask for a clear, returning true if
        // anything was changed (in which case RestoreAttachmentColorMasks must
        // be called once the clear has been issued).
        bool LiftAttachmentColorMasksForClear();
        void RestoreAttachmentColorMasks();

        bool m_DepthTestEnabled = false;
        bool m_DepthMaskEnabled = true;
        bool m_StencilTestEnabled = false;
        GLint m_MaxDrawBuffers = 0; // Cached from glGetIntegerv(GL_MAX_DRAW_BUFFERS) in Init().
        // Cached from glGetIntegerv(GL_MAX_PATCH_VERTICES) in Init(), so
        // SetPatchVertexCount can validate without a per-call driver round-trip.
        GLint m_MaxPatchVertices = 0;
        // Cached in Init(): GL_ARB_gpu_shader_int64 && GL_NV_shader_atomic_int64 (issue #629).
        bool m_SupportsInt64Atomics = false;
    };
} // namespace OloEngine
