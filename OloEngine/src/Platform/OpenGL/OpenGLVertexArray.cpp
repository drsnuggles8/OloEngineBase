#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <glad/gl.h>

namespace OloEngine
{
	[[nodiscard("Store this!")]] static constexpr GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType const type)
	{
		switch (type)
		{
			using enum OloEngine::ShaderDataType;
			case Float:
			case Float2:
			case Float3:
			case Float4:
			case Mat3:
			case Mat4:
			{
				return GL_FLOAT;
			}
			case Int:
			case Int2:
			case Int3:
			case Int4:
			{
				return GL_INT;
			}
			case Bool:
			{
				return GL_BOOL;
			}
			case None:
			{
				break;
			}
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
		RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
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
			switch (element.dataType)
			{
				using enum OloEngine::ShaderDataType;
				case Float:
				case Float2:
				case Float3:
				case Float4:
				case Mat3:
				case Mat4:
				{
					glEnableVertexArrayAttrib(m_RendererID, m_VertexBufferIndex);
					glVertexArrayVertexBuffer(m_RendererID, m_VertexBufferIndex, vertexBuffer->GetBufferHandle(), element.offset, layout.GetStride());
					glVertexArrayAttribFormat(m_RendererID, m_VertexBufferIndex, static_cast<GLint>(element.GetComponentCount()), ShaderDataTypeToOpenGLBaseType(element.dataType), element.normalized ? GL_TRUE : GL_FALSE, 0);

					glVertexArrayAttribBinding(m_RendererID, m_VertexBufferIndex, m_VertexBufferIndex);
					++m_VertexBufferIndex;
					break;
				}
				case Int:
				case Int2:
				case Int3:
				case Int4:
				case Bool:
				{
					glEnableVertexArrayAttrib(m_RendererID, m_VertexBufferIndex);
					// NOTE(olbu): If we uncomment these two and comment the one below, we lose hovered entity, not sure why
					//glVertexArrayVertexBuffer(m_RendererID, m_VertexBufferIndex, vertexBuffer->GetBufferHandle(), element.Offset, layout.GetStride());
					//glVertexArrayAttribFormat(m_RendererID, m_VertexBufferIndex, element.GetComponentCount(), ShaderDataTypeToOpenGLBaseType(element.Type), element.Normalized ? GL_TRUE : GL_FALSE, 0);
					glVertexAttribIPointer(m_VertexBufferIndex, element.GetComponentCount(), ShaderDataTypeToOpenGLBaseType(element.dataType), layout.GetStride(), reinterpret_cast<const void*>(element.offset));

					glVertexArrayAttribBinding(m_RendererID, m_VertexBufferIndex, m_VertexBufferIndex);
					++m_VertexBufferIndex;
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

		glVertexArrayElementBuffer(m_RendererID, indexBuffer->GetBufferHandle());
		m_IndexBuffer = indexBuffer;
	}

}
