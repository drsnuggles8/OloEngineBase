#pragma once

#include "OloEngine/Renderer/RendererAPI.h"

#include <glad/gl.h>

namespace OloEngine
{
	class RenderCommand
	{
	public:
		static void Init()
		{
			s_RendererAPI->Init();
		}

		static void SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		static void Clear()
		{
			s_RendererAPI->Clear();
		}

		static void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
		{
			s_RendererAPI->DrawArrays(vertexArray, vertexCount);
		}

		static void DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount);
		}

		static void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0, const u32 instanceCount = 1)
		{
			s_RendererAPI->DrawIndexedInstanced(vertexArray, indexCount, instanceCount);
		}

		static void DrawLines(const Ref<VertexArray>& vertexArray, const u32 vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		static void SetLineWidth(const f32 width)
		{
			s_RendererAPI->SetLineWidth(width);
		}

		static void EnableCulling()
		{
			s_RendererAPI->EnableCulling();
		}

		static void DisableCulling()
		{
			s_RendererAPI->DisableCulling();
		}

		static void FrontCull()
		{
			s_RendererAPI->FrontCull();
		}

		static void BackCull()
		{
			s_RendererAPI->BackCull();
		}

		static void SetDepthMask(bool value)
		{
			s_RendererAPI->SetDepthMask(value);
		}

		static void SetDepthTest(bool value)
		{
			s_RendererAPI->SetDepthTest(value);
		}

		static void SetBlendState(bool value)
		{
			s_RendererAPI->SetBlendState(value);
		}

		static void EnableBlending()
		{
			s_RendererAPI->SetBlendState(true);
		}

		static void DisableBlending()
		{
			s_RendererAPI->SetBlendState(false);
		}

		static void SetBlendFunc(GLenum sfactor, GLenum dfactor)
		{
			s_RendererAPI->SetBlendFunc(sfactor, dfactor);
		}

		static void EnableStencilTest()
		{
			s_RendererAPI->EnableStencilTest();
		}

		static void DisableStencilTest()
		{
			s_RendererAPI->DisableStencilTest();
		}

		static void SetStencilFunc(GLenum func, GLint ref, GLuint mask)
		{
			s_RendererAPI->SetStencilFunc(func, ref, mask);
		}

		static void SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
		{
			s_RendererAPI->SetStencilOp(sfail, dpfail, dppass);
		}

		static void SetPolygonMode(GLenum face, GLenum mode)
		{
			s_RendererAPI->SetPolygonMode(face, mode);
		}

		static void EnableScissorTest()
		{
			s_RendererAPI->EnableScissorTest();
		}

		static void DisableScissorTest()
		{
			s_RendererAPI->DisableScissorTest();
		}

		static void SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height)
		{
			s_RendererAPI->SetScissorBox(x, y, width, height);
		}

		static void SetStencilMask(GLuint mask)
		{
			s_RendererAPI->SetStencilMask(mask);
		}

		static void ClearStencil()
		{
			s_RendererAPI->ClearStencil();
		}
		
		// New methods for render graph
		static void BindDefaultFramebuffer()
		{
			s_RendererAPI->BindDefaultFramebuffer();
		}
		
		static void BindTexture(uint32_t slot, uint32_t textureID)
		{
			s_RendererAPI->BindTexture(slot, textureID);
		}

	private:
		static Scope<RendererAPI> s_RendererAPI;
	};
}
