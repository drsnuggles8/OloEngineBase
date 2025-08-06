#pragma once

#include "OloEngine/Scene/Scene.h"

#include <filesystem>

namespace OloEngine
{
	class SceneSerializer
	{
	public:
		explicit SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::filesystem::path& filepath) const;
		[[maybe_unused]] void SerializeRuntime(const std::filesystem::path& filepath) const;

        [[nodiscard("Store this!")]] bool Deserialize(const std::filesystem::path& filepath);
        [[nodiscard("Store this!")]][[maybe_unused]] bool DeserializeRuntime(const std::filesystem::path& filepath);
        
        // String-based serialization methods for asset pack support
        std::string SerializeToYAML() const;
        bool DeserializeFromYAML(const std::string& yamlString);
	private:
		Ref<Scene> m_Scene;
	};
}
