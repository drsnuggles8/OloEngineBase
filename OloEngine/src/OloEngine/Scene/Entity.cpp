// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Entity.h"

namespace OloEngine
{
	Entity::Entity(entt::entity handle, Scene& scene)
		: m_EntityHandle(handle), m_Scene(&scene)
	{
	}

	Entity::Entity(entt::entity const handle, Scene* const scene)
		: m_EntityHandle(handle), m_Scene(scene)
	{
	}

}
