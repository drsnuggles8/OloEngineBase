// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
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

		glEnable(GL_DEPTH_TEST);
		glEnable(GL_LINE_SMOOTH);
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

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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

		if (value)
		{
			glEnable(GL_DEPTH_TEST);
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}
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
}
