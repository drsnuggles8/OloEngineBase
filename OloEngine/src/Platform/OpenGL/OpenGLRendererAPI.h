#pragma once
#include "OloEngine/Renderer/RendererAPI.h"

#include <glad/gl.h>

namespace OloEngine
{

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

        void DrawIndexedRaw(u32 vaoID, u32 indexCount) override;
        void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex) override;
        void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount) override;
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices) override;

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

        void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) override;
        void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) override;
        void DrawElementsIndirectRaw(u32 vaoID, u32 indirectBufferID) override;
        void MultiDrawElementsIndirectCountRaw(u32 vaoID, u32 indirectBufferID, u32 indirectOffsetBytes,
                                               u32 parameterBufferID, u32 parameterOffsetBytes,
                                               u32 maxDrawCount, u32 strideBytes) override;

        void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier(MemoryBarrierFlags flags) override;

        void BindDefaultFramebuffer() override;
        void BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height) override;
        void BindTexture(u32 slot, u32 textureID) override;
        void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer,
                              RHI::Access access, RHI::Format format) override;

        void SetBlendStateForAttachment(u32 attachment, bool enabled) override;
        void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) override;
        void CopyImageSubData(u32 srcID, TextureTargetType srcTarget, u32 dstID, TextureTargetType dstTarget,
                              u32 width, u32 height) override;
        void CopyImageSubDataFull(u32 srcID, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                  u32 dstID, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                  u32 width, u32 height) override;
        void CopyFramebufferToTexture(u32 textureID, u32 width, u32 height) override;
        void SetDrawBuffers(std::span<const u32> attachments) override;
        void RestoreAllDrawBuffers(u32 colorAttachmentCount) override;
        u32 CreateTexture2D(u32 width, u32 height, RHI::Format internalFormat) override;
        u32 CreateTextureCubemap(u32 width, u32 height, RHI::Format internalFormat) override;
        u32 CreateDepthArrayCompareOffView(u32 srcTextureID, u32 numLayers) override;
        void SetTextureFilter(u32 textureID, RHI::Filter minFilter, RHI::Filter magFilter) override;
        void SetTextureWrap(u32 textureID, RHI::AddressMode wrap) override;
        void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                     RHI::Format sourceFormat, const void* data) override;
        void DeleteTexture(u32 textureID) override;

        void BeginConditionalRender(u32 queryID) override;
        void EndConditionalRender() override;

        [[nodiscard("Store this!")]] bool IsDeviceAvailable() const override;
        [[nodiscard("Store this!")]] u32 GetMaxUniformBlockSize() const override;
        [[nodiscard("Store this!")]] bool SupportsInt64ShaderAtomics() const override
        {
            return m_SupportsInt64Atomics;
        }

      private:
        bool m_DepthTestEnabled = false;
        bool m_DepthMaskEnabled = true;
        bool m_StencilTestEnabled = false;
        GLint m_MaxDrawBuffers = 0; // Cached from glGetIntegerv(GL_MAX_DRAW_BUFFERS) in Init().
        // Cached in Init(): GL_ARB_gpu_shader_int64 && GL_NV_shader_atomic_int64 (issue #629).
        bool m_SupportsInt64Atomics = false;
    };
} // namespace OloEngine
