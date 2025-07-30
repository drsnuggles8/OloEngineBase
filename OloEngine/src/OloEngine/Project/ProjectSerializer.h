#pragma once

#include "OloEngine/Project/Project.h"

namespace OloEngine
{

	class ProjectSerializer
	{
	public:
		ProjectSerializer(AssetRef<Project> project);

		bool Serialize(const std::filesystem::path& filepath);
		bool Deserialize(const std::filesystem::path& filepath);
	private:
		AssetRef<Project> m_Project;
	};

}
