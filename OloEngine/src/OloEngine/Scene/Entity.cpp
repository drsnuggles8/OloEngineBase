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

	Entity::Entity(entt::entity handle, const Scene& scene)
		: m_EntityHandle(handle), m_Scene(const_cast<Scene*>(&scene))
	{
	}

	Entity::Entity(entt::entity const handle, const Scene* const scene)
		: m_EntityHandle(handle), m_Scene(const_cast<Scene*>(scene))
	{
	}

}
