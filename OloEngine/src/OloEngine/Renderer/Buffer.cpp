#include "OloEnginePCH.h"
#include "Buffer.h"

#include "Renderer.h"

#include "Platform/OpenGL/OpenGLBuffer.h"

namespace OloEngine {
	
	VertexBuffer* VertexBuffer::Create(float* vertices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::None:		OLO_CORE_ASSERT(false, "RenderererAPI::None is currently not supported!");  return nullptr;
			case RendererAPI::OpenGL:	return new OpenGLVertexBuffer(vertices, size);
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	IndexBuffer* IndexBuffer::Create(uint32_t* indices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::None:		OLO_CORE_ASSERT(false, "RenderererAPI::None is currently not supported!");  return nullptr;
		case RendererAPI::OpenGL:	return new OpenGLIndexBuffer(indices, size);
		}
		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}