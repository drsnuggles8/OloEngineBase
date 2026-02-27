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
        Viewport GetViewport() const override;

        void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0) override;
        void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount = 0, u32 instanceCount = 1) override;
        void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
        void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices) override;

        void DrawIndexedRaw(u32 vaoID, u32 indexCount) override;
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices) override;

        void SetLineWidth(f32 width) override;

        void EnableCulling() override;
        void DisableCulling() override;
        void SetCullFace(GLenum face) override;
        void FrontCull() override;
        void BackCull() override;
        void SetDepthMask(bool value) override;
        void SetDepthTest(bool value) override;
        void SetBlendState(bool value) override;
        void SetBlendFunc(GLenum sfactor, GLenum dfactor) override;
        void SetBlendEquation(GLenum mode) override;
        void SetDepthFunc(GLenum func) override;
        void EnableStencilTest() override;
        void DisableStencilTest() override;
        void SetStencilFunc(GLenum func, GLint ref, GLuint mask) override;
        void SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) override;
        void SetStencilMask(GLuint mask) override;
        void ClearStencil() override;

        void SetPolygonMode(GLenum face, GLenum mode) override;
        void SetPolygonOffset(f32 factor, f32 units) override;
        void EnableMultisampling() override;
        void DisableMultisampling() override;
        void SetColorMask(bool red, bool green, bool blue, bool alpha) override;

        void EnableScissorTest() override;
        void DisableScissorTest() override;
        void SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height) override;

        void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) override;
        void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) override;

        void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) override;
        void MemoryBarrier(MemoryBarrierFlags flags) override;

        void BindDefaultFramebuffer() override;
        void BindTexture(u32 slot, u32 textureID) override;
        void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer, GLenum access, GLenum format) override;

        void SetBlendStateForAttachment(u32 attachment, bool enabled) override;
        void CopyImageSubData(u32 srcID, u32 srcTarget, u32 dstID, u32 dstTarget,
                              u32 width, u32 height) override;
        void SetDrawBuffers(std::span<const u32> attachments) override;
        void RestoreAllDrawBuffers(u32 colorAttachmentCount) override;
        u32 CreateTexture2D(u32 width, u32 height, GLenum internalFormat) override;
        void SetTextureParameter(u32 textureID, GLenum pname, GLint value) override;
        void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                     GLenum format, GLenum type, const void* data) override;
        void DeleteTexture(u32 textureID) override;

      private:
        bool m_DepthTestEnabled = false;
        bool m_DepthMaskEnabled = true;
        bool m_StencilTestEnabled = false;
    };
} // namespace OloEngine
