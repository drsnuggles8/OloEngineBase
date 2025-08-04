#pragma once

#include <string>
#include <unordered_map>
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
	// Forward declaration of Shader class
	class Shader;

	class ShaderLibrary
	{
	public:
		void Add(const std::string& name, const AssetRef<Shader>& shader);
		void Add(const AssetRef<Shader>& shader);
		AssetRef<Shader> Load(const std::string& filepath);
		AssetRef<Shader> Load(const std::string& name, const std::string& filepath);

		AssetRef<Shader> Get(const std::string& name);

		void ReloadShaders();

		[[nodiscard("Store this!")]] bool Exists(const std::string& name) const;
	private:
		std::unordered_map<std::string, AssetRef<Shader>> m_Shaders;
	};
}
