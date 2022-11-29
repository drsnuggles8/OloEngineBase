// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GraphicsContext.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"

namespace OloEngine
{
	Scope<GraphicsContext> GraphicsContext::Create(void* const window)
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
				return CreateScope<OpenGLContext>(static_cast<GLFWwindow*>(window));
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
