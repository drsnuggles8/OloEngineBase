#pragma once

#include "OloEngine/Renderer/Framebuffer.h"
#include <glad/gl.h>

namespace OloEngine::Utils
{
		[[nodiscard("Store this!")]] constexpr GLenum TextureTarget(const bool multisampled) noexcept;
		void PrepareTexture(const u32 id, const int samples, const GLenum format, const int width, const int height);
		void CreateTextures(const bool multisampled, const int count, u32* const outID);
		void BindTexture(const u32 id);
		void BindTextures(const u32 firstID, const u32 count, const GLuint* id);
		void AttachColorTexture(const u32 fbo, const u32 id, const int samples, const GLenum internalFormat, const int width, const int height, const u32 index);
		void AttachDepthTexture(const u32 fbo, const u32 id, const int samples, const GLenum format, const GLenum attachmentType, const int width, const int height);
		[[nodiscard("Store this!")]] bool IsDepthFormat(const FramebufferTextureFormat format) noexcept;
		[[nodiscard("Store this!")]] GLenum OloFBTextureFormatToGL(const FramebufferTextureFormat format);
		[[nodiscard("Store this!")]] GLenum OloFBColorTextureFormatToGL(const FramebufferTextureFormat format);
		[[nodiscard("Store this!")]] GLenum OloFBDepthTextureFormatToGL(const FramebufferTextureFormat format);
}
