#pragma once

#include "OloEngine/Scene/Scene.h"

#include <filesystem>

namespace OloEngine {

	class SceneSerializer
	{
	public:
		explicit SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::filesystem::path& filepath) const;

		[[maybe_unused]] void SerializeRuntime(const std::filesystem::path& filepath) const;

		bool Deserialize(const std::filesystem::path& filepath) const;

		[[maybe_unused]] bool DeserializeRuntime(const std::filesystem::path& filepath) const;
	private:
		Ref<Scene> m_Scene;
	};

}
