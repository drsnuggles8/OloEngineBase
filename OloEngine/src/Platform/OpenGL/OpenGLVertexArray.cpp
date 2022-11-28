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
			switch (element.Type)
			{
				using enum OloEngine::ShaderDataType;
				case Float:
				case Float2:
				case Float3:
				case Float4:
				{
					glEnableVertexAttribArray(m_VertexBufferIndex);
					glVertexAttribPointer(m_VertexBufferIndex,
						element.GetComponentCount(),
						ShaderDataTypeToOpenGLBaseType(element.Type),
						element.Normalized ? GL_TRUE : GL_FALSE,
						layout.GetStride(),
						reinterpret_cast<const void*>(element.Offset));
					++m_VertexBufferIndex;
					break;
				}
				case Int:
				case Int2:
				case Int3:
				case Int4:
				case Bool:
				{
					glEnableVertexAttribArray(m_VertexBufferIndex);
					glVertexAttribIPointer(m_VertexBufferIndex,
						element.GetComponentCount(),
						ShaderDataTypeToOpenGLBaseType(element.Type),
						layout.GetStride(),
						reinterpret_cast<const void*>(element.Offset));
					++m_VertexBufferIndex;
					break;
				}
				case Mat3:
				case Mat4:
				{
					auto const count = static_cast<uint8_t>(element.GetComponentCount());
					for (uint8_t i = 0; i < count; ++i)
					{
						glEnableVertexAttribArray(m_VertexBufferIndex);
						glVertexAttribPointer(m_VertexBufferIndex,
							count,
							ShaderDataTypeToOpenGLBaseType(element.Type),
							element.Normalized ? GL_TRUE : GL_FALSE,
							layout.GetStride(),
							reinterpret_cast<const void*>(element.Offset + (sizeof(float) * count * i)));
						glVertexAttribDivisor(m_VertexBufferIndex, 1);
						++m_VertexBufferIndex;
					}
					break;
				}
				default:
				{
					OLO_CORE_ASSERT(false, "Unknown ShaderDataType!");
				}
			}
		}

		m_VertexBuffers.push_back(vertexBuffer);
	}

	void OpenGLVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
	{
		OLO_PROFILE_FUNCTION();

		glBindVertexArray(m_RendererID);
		indexBuffer->Bind();

		m_IndexBuffer = indexBuffer;
	}

}
