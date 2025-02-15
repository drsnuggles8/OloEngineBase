#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace OloEngine
{
	RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;

	[[nodiscard("Store this!")]] Scope<RendererAPI> RendererAPI::Create()
	{
		switch (s_API)
		{
			case RendererAPI::API::None:
			{
				OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			}
			case RendererAPI::API::OpenGL:
			{
				return CreateScope<OpenGLRendererAPI>();
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
