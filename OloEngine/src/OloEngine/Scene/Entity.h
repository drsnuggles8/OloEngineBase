#pragma once

#include "OloEngine/Core/UUID.h"
#include "Scene.h"
#include "Components.h"

#pragma warning( push )
#pragma warning( disable : 4996)
#include <entt.hpp>
#pragma warning( pop )

namespace OloEngine
{
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene& scene);
		Entity(entt::entity handle, Scene* scene);
		~Entity() = default;

		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			OLO_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
			T& component = m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			T& component = m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		template<typename T>
		T& GetComponent()
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		const T& GetComponent() const
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity doesn't have component!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent()
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		[[nodiscard("Store this!")]] bool HasComponent() const
		{
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename...T>
		bool HasAny()
		{
			return m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename...T>
		[[nodiscard("Store this!")]] bool HasAny() const
		{
			return m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename T>
		void RemoveComponent()
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		explicit operator bool() const { return m_EntityHandle != entt::null; }
		// TODO(olbu):: Check if we can make the below operator explicit
		explicit(false) operator entt::entity() const { return m_EntityHandle; }
		explicit operator uint32_t() const { return static_cast<uint32_t>(m_EntityHandle); }

		TransformComponent GetTransform() const { return GetComponent<TransformComponent>(); }

		glm::mat4 GetLocalTransform() const
		{
			const auto& transform = GetTransform();
			return glm::translate(glm::mat4(1.0f), transform.Translation) * glm::toMat4(glm::quat(transform.Rotation)) * glm::scale(glm::mat4(1.0f), transform.Scale);
		}

		UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() { return GetComponent<TagComponent>().Tag; }

		bool operator==(const Entity& other) const
		{
			return (m_EntityHandle == other.m_EntityHandle) && (m_Scene == other.m_Scene);
		}

	private:
		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};

}
