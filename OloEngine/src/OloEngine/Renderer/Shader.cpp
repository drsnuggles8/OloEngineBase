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
				return CreateRef<OpenGLShader>(filepath);
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
				return CreateRef<OpenGLShader>(name, vertexSrc, fragmentSrc);
			}
		}

		OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
	{
		OLO_CORE_ASSERT(!Exists(name), "Shader already exists!");
		m_Shaders[name] = shader;
	}

	void ShaderLibrary::Add(const Ref<Shader>& shader)
	{
		auto& name = shader->GetName();
		Add(name, shader);
	}

	Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
	{
		auto shader = Shader::Create(filepath);
		Add(shader);
		return shader;
	}

	Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
	{
		auto shader = Shader::Create(filepath);
		Add(name, shader);
		return shader;
	}

	Ref<Shader> ShaderLibrary::Get(const std::string& name)
	{
		OLO_CORE_ASSERT(Exists(name), "Shader not found!");
		return m_Shaders[name];
	}

	void ShaderLibrary::ReloadShaders()
	{
		for (auto& [name, shader] : m_Shaders)
		{
			shader->Reload();
		}
	}

	bool ShaderLibrary::Exists(const std::string& name) const
	{
		return m_Shaders.contains(name);
	}
}
