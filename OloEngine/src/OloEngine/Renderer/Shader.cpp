#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shader.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
	Ref<Shader> Shader::Create(const std::string& filepath)
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
				auto shader = Ref<OpenGLShader>::Create(filepath);
				// Initialize the resource registry after construction
				static_cast<OpenGLShader*>(shader.get())->InitializeResourceRegistry(shader);
				return shader;
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<Shader> Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
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
				auto shader = Ref<OpenGLShader>::Create(name, vertexSrc, fragmentSrc);
				// Initialize the resource registry after construction
				static_cast<OpenGLShader*>(shader.get())->InitializeResourceRegistry(shader);
				return shader;
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
