#pragma once

#include "OloEngine/Scene/Scene.h"

namespace OloEngine {

	class SceneSerializer
	{
	public:
		explicit SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::string& filepath) const;

		[[maybe_unused]] void SerializeRuntime(std::string_view filepath) const;

		bool Deserialize(const std::string& filepath) const;

		[[maybe_unused]] bool DeserializeRuntime(std::string_view const filepath) const;
	private:
		Ref<Scene> m_Scene;
	};

}
