#pragma once

#include "OloEngine/Scene/Scene.h"

namespace OloEngine {

	class SceneSerializer
	{
	public:
		explicit SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::string& filepath) const;

		[[maybe_unused]] void SerializeRuntime(const std::string& filepath) const;

		bool Deserialize(const std::string& filepath) const;

		[[maybe_unused]] bool DeserializeRuntime(const std::string& filepath) const;
	private:
		Ref<Scene> m_Scene;
	};

}
