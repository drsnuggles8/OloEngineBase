#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLIndexBuffer.h"

namespace OloEngine
{
	AssetRef<IndexBuffer> IndexBuffer::Create(u32* indices, u32 size)
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
				return AssetRef<IndexBuffer>(new OpenGLIndexBuffer(indices, size));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
