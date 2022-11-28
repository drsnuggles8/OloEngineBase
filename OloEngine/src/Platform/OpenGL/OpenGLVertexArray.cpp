// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

#include <glad/gl.h>

namespace OloEngine
{
	[[nodiscard("Store this!")]] static GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType const type)
	{
		switch (type)
		{
			using enum OloEngine::ShaderDataType;
			case Float:    return GL_FLOAT;
			case Float2:   return GL_FLOAT;
			case Float3:   return GL_FLOAT;
			case Float4:   return GL_FLOAT;
			case Mat3:     return GL_FLOAT;
			case Mat4:     return GL_FLOAT;
			case Int:      return GL_INT;
			case Int2:     return GL_INT;
			case Int3:     return GL_INT;
			case Int4:     return GL_INT;
			case Bool:     return GL_BOOL;
			case None:     break;
		}

		OLO_CORE_ASSERT(false, "Unknown ShaderDataType!");
		return 0;
	}

	OpenGLVertexArray::OpenGLVertexArray()
	{
		OLO_PROFILE_FUNCTION();

		glCreateVertexArrays(1, &m_RendererID);
	}

	OpenGLVertexArray::~OpenGLVertexArray()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteVertexArrays(1, &m_RendererID);
	}

	void OpenGLVertexArray::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindVertexArray(m_RendererID);
	}

	void OpenGLVertexArray::Unbind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindVertexArray(0);
	}

	void OpenGLVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
	{
		OLO_PROFILE_FUNCTION();

		OLO_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size(), "Vertex Buffer has no layout!");

		glBindVertexArray(m_RendererID);
		vertexBuffer->Bind();

		for (const auto& layout = vertexBuffer->GetLayout(); const auto& element : layout)
		{
			glEnableVertexArrayAttrib(m_RendererID, m_VertexBufferIndex);
			glVertexArrayVertexBuffer(m_RendererID, m_VertexBufferIndex, vertexBuffer->GetBufferHandle(), element.Offset, layout.GetStride());
			glVertexArrayAttribFormat(m_RendererID, m_VertexBufferIndex, element.GetComponentCount(), ShaderDataTypeToOpenGLBaseType(element.Type), element.Normalized ? GL_TRUE : GL_FALSE, 0);
			glVertexArrayAttribBinding(m_RendererID, m_VertexBufferIndex, m_VertexBufferIndex);
			m_VertexBufferIndex++;
		}

		m_VertexBuffers.push_back(vertexBuffer);
	}

	void OpenGLVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
	{
		OLO_PROFILE_FUNCTION();

		glVertexArrayElementBuffer(m_RendererID, indexBuffer->GetBufferHandle());

		m_IndexBuffer = indexBuffer;
	}

}
