#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLVertexBuffer.h"

namespace OloEngine
{

	Ref<VertexBuffer> VertexBuffer::Create(u32 size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(size));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<VertexBuffer> VertexBuffer::Create(f32* vertices, u32 size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(vertices, size));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<VertexBuffer> VertexBuffer::Create(const f32* vertices, u32 size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(vertices, size));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<VertexBuffer> VertexBuffer::Create(const void* data, u32 size)
	{
		// Runtime check: if size > 0, data must not be null
		// For uninitialized buffer creation, use Create(u32 size) instead
		OLO_CORE_ASSERT(size == 0 || data != nullptr, 
			"VertexBuffer::Create(): data cannot be null when size > 0. Use Create(u32 size) for uninitialized buffer creation.");

		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None:
			{
				OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(data, size));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
