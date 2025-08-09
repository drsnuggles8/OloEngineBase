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
		Entity(entt::entity handle, const Scene& scene);
		Entity(entt::entity handle, const Scene* scene);
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
		[[nodiscard]] const T& GetComponent() const
		{
			OLO_CORE_ASSERT(HasComponent<T>(), "Entity doesn't have component!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent()
		{
			return m_Scene && m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		[[nodiscard("Store this!")]] bool HasComponent() const
		{
			return m_Scene && m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename...T>
		bool HasAny()
		{
			return m_Scene && m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename...T>
		[[nodiscard("Store this!")]] bool HasAny() const
		{
			return m_Scene && m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
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
		explicit operator u32() const { return static_cast<u32>(m_EntityHandle); }

		[[nodiscard("Store this!")]] TransformComponent GetTransform() const { return GetComponent<TransformComponent>(); }

		[[nodiscard("Store this!")]] glm::mat4 GetLocalTransform() const
		{
			const auto& transform = GetTransform();
			return glm::translate(glm::mat4(1.0f), transform.Translation) * glm::toMat4(glm::quat(transform.Rotation)) * glm::scale(glm::mat4(1.0f), transform.Scale);
		}

		[[nodiscard("Store this!")]] UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() { return GetComponent<TagComponent>().Tag; }

		// Parent-child hierarchy methods (Hazel-style)
		Entity GetParent() const
		{
			if (!HasComponent<RelationshipComponent>())
				return {};
			
			UUID parentID = GetComponent<RelationshipComponent>().m_ParentHandle;
			if (parentID == 0) // null UUID
				return {};
			
			return m_Scene->TryGetEntityWithUUID(parentID);
		}

		void SetParent(Entity parent)
		{
			Entity currentParent = GetParent();
			if (currentParent == parent)
				return;

			// If changing parent, remove child from existing parent
			if (currentParent)
				currentParent.RemoveChild(*this);

			// Setting to null is okay
			SetParentUUID(parent.GetUUID());

			if (parent)
			{
				auto& parentChildren = parent.Children();
				UUID uuid = GetUUID();
				if (std::find(parentChildren.begin(), parentChildren.end(), uuid) == parentChildren.end())
					parentChildren.emplace_back(uuid);
			}
		}

		void SetParentUUID(UUID parent) 
		{ 
			if (!HasComponent<RelationshipComponent>())
				AddComponent<RelationshipComponent>();
			GetComponent<RelationshipComponent>().m_ParentHandle = parent; 
		}

		UUID GetParentUUID() const 
		{ 
			if (!HasComponent<RelationshipComponent>())
				return UUID(0);
			return GetComponent<RelationshipComponent>().m_ParentHandle; 
		}

		std::vector<UUID>& Children() 
		{ 
			if (!HasComponent<RelationshipComponent>())
				AddComponent<RelationshipComponent>();
			return GetComponent<RelationshipComponent>().m_Children; 
		}

		const std::vector<UUID>& Children() const 
		{ 
			if (!HasComponent<RelationshipComponent>())
			{
				static std::vector<UUID> emptyChildren;
				return emptyChildren;
			}
			return GetComponent<RelationshipComponent>().m_Children; 
		}

		bool RemoveChild(Entity child)
		{
			if (!HasComponent<RelationshipComponent>())
				return false;
				
			UUID childId = child.GetUUID();
			std::vector<UUID>& children = Children();
			auto it = std::find(children.begin(), children.end(), childId);
			if (it != children.end())
			{
				children.erase(it);
				return true;
			}
			return false;
		}

		bool operator==(const Entity& other) const
		{
			return (m_EntityHandle == other.m_EntityHandle) && (m_Scene == other.m_Scene);
		}

	private:
		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};
}
