// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

#include <glad/gl.h>

namespace OloEngine
{
	void OpenGLMessageCallback(
		const unsigned int source,
		const unsigned int type,
		const unsigned int id,
		const unsigned int severity,
		const int,
		const char* const message,
		const void* const)
	{
		std::string sourceStr;
		switch (source)
		{
			case GL_DEBUG_SOURCE_API:             sourceStr = "API"; break;
			case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   sourceStr = "WINDOW_SYSTEM"; break;
			case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceStr = "SHADER_COMPILER"; break;
			case GL_DEBUG_SOURCE_THIRD_PARTY:     sourceStr = "THIRD_PARTY"; break;
			case GL_DEBUG_SOURCE_APPLICATION:     sourceStr = "APPLICATION"; break;
			case GL_DEBUG_SOURCE_OTHER:           sourceStr = "OTHER"; break;
		}
		std::string typeStr;
		switch (type)
		{
			case GL_DEBUG_TYPE_ERROR:               typeStr = "ERROR"; break;
			case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeStr = "DEPRECATED_BEHAVIOR"; break;
			case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  typeStr = "UNDEFINED_BEHAVIOR"; break;
			case GL_DEBUG_TYPE_PORTABILITY:         typeStr = "PORTABILITY"; break;
			case GL_DEBUG_TYPE_PERFORMANCE:         typeStr = "PERFORMANCE"; break;
			case GL_DEBUG_TYPE_OTHER:               typeStr = "OTHER"; break;
			case GL_DEBUG_TYPE_MARKER:              typeStr = "MARKER"; break;
		}
		switch (severity)
		{
			case GL_DEBUG_SEVERITY_HIGH:         OLO_CORE_CRITICAL("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message); return;
			case GL_DEBUG_SEVERITY_MEDIUM:       OLO_CORE_ERROR("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message); return;
			case GL_DEBUG_SEVERITY_LOW:          OLO_CORE_WARN("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message); return;
			case GL_DEBUG_SEVERITY_NOTIFICATION: OLO_CORE_TRACE("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message); return;
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

				glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
		#endif

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glEnable(GL_DEPTH_TEST);
		glEnable(GL_LINE_SMOOTH);
	}

	void OpenGLRendererAPI::SetViewport(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height)
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

	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, const uint32_t indexCount)
	{
		OLO_PROFILE_FUNCTION();

		vertexArray->Bind();
		const uint32_t count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);
	}

	void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, const uint32_t vertexCount)
	{
		OLO_PROFILE_FUNCTION();

		vertexArray->Bind();
		glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));
	}

	void OpenGLRendererAPI::SetLineWidth(const float width)
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
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);
	}

	void OpenGLRendererAPI::SetBlendState(bool value)
	{
		OLO_PROFILE_FUNCTION();

		if (value)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);
	}
}
