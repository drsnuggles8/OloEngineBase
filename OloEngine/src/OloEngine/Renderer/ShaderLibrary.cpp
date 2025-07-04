#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{	void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
	{
		OLO_CORE_ASSERT(!Exists(name), "Shader already exists!");
		m_Shaders[name] = shader;
		
		// Register with shader debugger
		OLO_SHADER_REGISTER(shader);
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

	void ShaderLibrary::ReloadShaders() const
	{
		for (auto const& [name, shader] : m_Shaders)
		{
			shader->Reload();
		}
	}

	bool ShaderLibrary::Exists(const std::string& name) const
	{
		return m_Shaders.contains(name);
	}
}
