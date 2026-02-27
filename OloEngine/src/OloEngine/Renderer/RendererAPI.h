#pragma once

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"

#include <glm/glm.hpp>
#include <glad/gl.h>
#include <span>

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
            TextureCubeMap
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
        virtual void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices) = 0;

        virtual void SetLineWidth(f32 width) = 0;

        virtual void EnableCulling() = 0;
        virtual void DisableCulling() = 0;
        virtual void FrontCull() = 0;
        virtual void BackCull() = 0;
        virtual void SetCullFace(GLenum face) = 0;
        virtual void SetDepthMask(bool value) = 0;
        virtual void SetDepthTest(bool value) = 0;
        virtual void SetDepthFunc(GLenum func) = 0;
        virtual void SetBlendState(bool value) = 0;
        virtual void SetBlendFunc(GLenum sfactor, GLenum dfactor) = 0;
        virtual void SetBlendEquation(GLenum mode) = 0;

        virtual void EnableStencilTest() = 0;
        virtual void DisableStencilTest() = 0;
        virtual bool IsStencilTestEnabled() const = 0;
        virtual void SetStencilFunc(GLenum func, GLint ref, GLuint mask) = 0;
        virtual void SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) = 0;
        virtual void SetStencilMask(GLuint mask) = 0;
        virtual void ClearStencil() = 0;

        virtual void SetPolygonMode(GLenum face, GLenum mode) = 0;

        virtual void EnableScissorTest() = 0;
        virtual void DisableScissorTest() = 0;
        virtual void SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height) = 0;

        // Indirect draw calls (GPU-driven rendering)
        virtual void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) = 0;
        virtual void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID) = 0;

        // Compute shader dispatch
        virtual void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;
        virtual void MemoryBarrier(MemoryBarrierFlags flags) = 0;

        // New methods for render graph
        virtual void BindDefaultFramebuffer() = 0;
        virtual void BindTexture(u32 slot, u32 textureID) = 0;
        virtual void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer, GLenum access, GLenum format) = 0;

        virtual void SetPolygonOffset(f32 factor, f32 units) = 0;
        virtual void EnableMultisampling() = 0;
        virtual void DisableMultisampling() = 0;
        virtual void SetColorMask(bool red, bool green, bool blue, bool alpha) = 0;

        // Per-attachment blend control (needed for mixed integer/float framebuffer attachments)
        virtual void SetBlendStateForAttachment(u32 attachment, bool enabled) = 0;

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
        virtual u32 CreateTexture2D(u32 width, u32 height, GLenum internalFormat) = 0;
        virtual u32 CreateTextureCubemap(u32 width, u32 height, GLenum internalFormat) = 0;
        virtual void SetTextureParameter(u32 textureID, GLenum pname, GLint value) = 0;
        virtual void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                             GLenum format, GLenum type, const void* data) = 0;
        virtual void DeleteTexture(u32 textureID) = 0;

        [[nodiscard("Store this!")]] static API GetAPI()
        {
            return s_API;
        }
        static Scope<RendererAPI> Create();

      private:
        static API s_API;
    };

} // namespace OloEngine
