// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

#include <glad/gl.h>

namespace OloEngine
{
	void OpenGLMessageCallback(
		const unsigned,
		const unsigned,
		const unsigned,
		const unsigned severity,
		const int,
		const char* const message,
		const void* const)
	{
		switch (severity)
		{
			case GL_DEBUG_SEVERITY_HIGH:         OLO_CORE_CRITICAL(message); return;
			case GL_DEBUG_SEVERITY_MEDIUM:       OLO_CORE_ERROR(message); return;
			case GL_DEBUG_SEVERITY_LOW:          OLO_CORE_WARN(message); return;
			case GL_DEBUG_SEVERITY_NOTIFICATION: OLO_CORE_TRACE(message); return;
		}

		OLO_CORE_ASSERT(false, "Unknown severity level!");
	}

	void OpenGLRendererAPI::Init()
	{
		OLO_PROFILE_FUNCTION();

		#ifdef OLO_DEBUG
				glEnable(GL_DEBUG_OUTPUT);
				glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
				glDebugMessageCallback(OpenGLMessageCallback, nullptr);

				glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_TRUE);
		#endif

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glEnable(GL_DEPTH_TEST);
		glEnable(GL_LINE_SMOOTH);
	}

	void OpenGLRendererAPI::SetViewport(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height)
	{
		glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
	}

	void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRendererAPI::Clear()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, const uint32_t indexCount)
	{
		vertexArray->Bind();
		const uint32_t count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);
	}

	void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, const uint32_t vertexCount)
	{
		vertexArray->Bind();
		glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));
	}

	void OpenGLRendererAPI::SetLineWidth(const float width)
	{
		glLineWidth(width);
	}
}
