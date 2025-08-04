#include "OloEnginePCH.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/Project/ProjectSerializer.h"

namespace OloEngine
{

	Ref<Project> Project::New()
	{
		s_ActiveProject = Ref<Project>::Create();
		return s_ActiveProject;
	}

	Ref<Project> Project::Load(const std::filesystem::path& path)
	{
		Ref<Project> project = Ref<Project>::Create();

		if (ProjectSerializer serializer(project); serializer.Deserialize(path))
		{
			project->m_ProjectDirectory = path.parent_path();
			s_ActiveProject = project;
			return s_ActiveProject;
		}

		return nullptr;
	}

	bool Project::SaveActive(const std::filesystem::path& path)
	{
		if (ProjectSerializer serializer(s_ActiveProject); serializer.Serialize(path))
		{
			s_ActiveProject->m_ProjectDirectory = path.parent_path();
			return true;
		}

		return false;
	}
}
