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

		void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;
		void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount = 0) override;
		void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) override;

		void SetLineWidth(f32 width) override;

		void EnableCulling() override;
		void DisableCulling() override;
		void FrontCull() override;
		void BackCull() override;
		void SetDepthMask(bool value) override;
		void SetDepthTest(bool value) override;
		void SetBlendState(bool value) override;
		void EnableStencilTest();
		void DisableStencilTest();
		void SetStencilFunc(GLenum func, GLint ref, GLuint mask);
		void SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass);

		void SetPolygonMode(GLenum face, GLenum mode);

		void EnableScissorTest();
		void DisableScissorTest();
		void SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height);
	};
}
