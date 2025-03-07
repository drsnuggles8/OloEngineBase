#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/OpenGL/OpenGLDebug.h"

#include <glad/gl.h>

namespace OloEngine
{
	void OpenGLRendererAPI::Init()
	{
		OLO_PROFILE_FUNCTION();

#ifdef OLO_DEBUG
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(OpenGLMessageCallback, nullptr);

		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
#endif

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		SetDepthTest(true);
		SetDepthFunc(GL_LESS);
		glEnable(GL_LINE_SMOOTH);

		EnableStencilTest();
		SetStencilFunc(GL_ALWAYS, 1, 0xFF);
		SetStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	}

	void OpenGLRendererAPI::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
	{
		OLO_PROFILE_FUNCTION();

		glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
	}

	void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
	{
		OLO_PROFILE_FUNCTION();

		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRendererAPI::Clear()
	{
		OLO_PROFILE_FUNCTION();

		GLbitfield clearFlags = GL_COLOR_BUFFER_BIT;
		if (m_DepthTestEnabled)
		{
			clearFlags |= GL_DEPTH_BUFFER_BIT;
		}
		if (m_StencilTestEnabled)
		{
			clearFlags |= GL_STENCIL_BUFFER_BIT;
		}

		glClear(clearFlags);
	}

	void OpenGLRendererAPI::DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
	{
		OLO_PROFILE_FUNCTION();

		vertexArray->Bind();
		glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(vertexCount));
	}

	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount)
	{
		OLO_PROFILE_FUNCTION();

		vertexArray->Bind();
		const u32 count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);
	}

	void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, const u32 vertexCount)
	{
		OLO_PROFILE_FUNCTION();

		vertexArray->Bind();
		glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));
	}

	void OpenGLRendererAPI::SetLineWidth(const f32 width)
	{
		OLO_PROFILE_FUNCTION();

		glLineWidth(width);
	}

	void OpenGLRendererAPI::EnableCulling()
	{
		OLO_PROFILE_FUNCTION();

		glEnable(GL_CULL_FACE);
	}

	void OpenGLRendererAPI::DisableCulling()
	{
		OLO_PROFILE_FUNCTION();

		glDisable(GL_CULL_FACE);
	}

	void OpenGLRendererAPI::FrontCull()
	{
		OLO_PROFILE_FUNCTION();

		glCullFace(GL_FRONT);
	}

	void OpenGLRendererAPI::BackCull()
	{
		OLO_PROFILE_FUNCTION();

		glCullFace(GL_BACK);
	}

	void OpenGLRendererAPI::SetDepthMask(bool value)
	{
		OLO_PROFILE_FUNCTION();

		glDepthMask(value);
	}

	void OpenGLRendererAPI::SetDepthTest(bool value)
	{
		OLO_PROFILE_FUNCTION();

		m_DepthTestEnabled = value;

		if (value)
		{
			glEnable(GL_DEPTH_TEST);
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}
	}

	void OpenGLRendererAPI::SetDepthFunc(GLenum func)
	{
		OLO_PROFILE_FUNCTION();

		glDepthFunc(func);
	}

	void OpenGLRendererAPI::SetStencilMask(GLuint mask)
	{
		OLO_PROFILE_FUNCTION();

		glStencilMask(mask);
	}

	void OpenGLRendererAPI::ClearStencil()
	{
		OLO_PROFILE_FUNCTION();

		glClear(GL_STENCIL_BUFFER_BIT);
	}

	void OpenGLRendererAPI::SetBlendState(bool value)
	{
		OLO_PROFILE_FUNCTION();

		if (value)
		{
			glEnable(GL_BLEND);
		}
		else
		{
			glDisable(GL_BLEND);
		}
	}

	void OpenGLRendererAPI::EnableStencilTest()
	{
		OLO_PROFILE_FUNCTION();

		m_StencilTestEnabled = true;
		glEnable(GL_STENCIL_TEST);
	}

	void OpenGLRendererAPI::DisableStencilTest()
	{
		OLO_PROFILE_FUNCTION();

		m_StencilTestEnabled = false;
		glDisable(GL_STENCIL_TEST);
	}

	void OpenGLRendererAPI::SetStencilFunc(GLenum func, GLint ref, GLuint mask)
	{
		OLO_PROFILE_FUNCTION();

		glStencilFunc(func, ref, mask);
	}

	void OpenGLRendererAPI::SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
	{
		OLO_PROFILE_FUNCTION();

		glStencilOp(sfail, dpfail, dppass);
	}

	void OpenGLRendererAPI::SetPolygonMode(GLenum face, GLenum mode)
	{
		OLO_PROFILE_FUNCTION();

		glPolygonMode(face, mode);
	}

	void OpenGLRendererAPI::EnableScissorTest()
	{
		OLO_PROFILE_FUNCTION();

		glEnable(GL_SCISSOR_TEST);
	}

	void OpenGLRendererAPI::DisableScissorTest()
	{
		OLO_PROFILE_FUNCTION();

		glDisable(GL_SCISSOR_TEST);
	}

	void OpenGLRendererAPI::SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height)
	{
		OLO_PROFILE_FUNCTION();

		glScissor(x, y, width, height);
	}
}
