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

		void DrawArrays(const AssetRef<VertexArray>& vertexArray, u32 vertexCount) override;
		void DrawIndexed(const AssetRef<VertexArray>& vertexArray, u32 indexCount = 0) override;
		void DrawIndexedInstanced(const AssetRef<VertexArray>& vertexArray, u32 indexCount = 0, u32 instanceCount = 1) override;
		void DrawLines(const AssetRef<VertexArray>& vertexArray, u32 vertexCount) override;

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
		
		void BindDefaultFramebuffer() override;
		void BindTexture(u32 slot, u32 textureID) override;
		
	private:
		bool m_DepthTestEnabled = false;
		bool m_StencilTestEnabled = false;
	};
}
