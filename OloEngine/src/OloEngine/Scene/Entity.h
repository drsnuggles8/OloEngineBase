#pragma once

#include "Scene.h"

#include "entt.hpp"

namespace OloEngine {

	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity const handle, Scene* const scene)
			: m_EntityHandle(handle), m_Scene(scene) {}
		~Entity() = default;

		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			OLO_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!")
			T& component = m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		template<typename T>
		T& GetComponent()
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!")
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		const T& GetComponent() const
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity doesn't have component!")
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent()
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		[[nodiscard("Store this, you probably wanted another function!")]] bool HasComponent() const
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename...T>
		bool HasAny()
		{
			return m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename...T>
		[[nodiscard("Store this, you probably wanted another function!")]] bool HasAny() const
		{
			return m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename T>
		void RemoveComponent()
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!")
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		explicit operator bool() const { return m_EntityHandle != entt::null; }
		// TODO(olbu):: Check if we can make the below operator explicit
		explicit(false) operator entt::entity() const { return m_EntityHandle; }
		explicit operator uint32_t() const { return static_cast<uint32_t>(m_EntityHandle); }

		bool operator==(const Entity& other) const
		{
			return (m_EntityHandle == other.m_EntityHandle) && (m_Scene == other.m_Scene);
		}

		bool operator!=(const Entity& other) const
		{
			return !(*this == other);
		}
	private:
		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};

}
