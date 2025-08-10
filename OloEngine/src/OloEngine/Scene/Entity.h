#pragma once

#include "OloEngine/Core/UUID.h"
#include "Scene.h"
#include "Components.h"

#include <algorithm>

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
			
			auto parentOpt = m_Scene->TryGetEntityWithUUID(parentID);
			return parentOpt ? *parentOpt : Entity{};
		}

		void SetParent(Entity parent)
		{
			// Guard against self-parenting
			if (parent && parent == *this)
			{
				OLO_CORE_ASSERT(false, "Entity cannot be its own parent");
				return;
			}

			// Guard against cross-scene parenting
			if (parent && parent.m_Scene != m_Scene)
			{
				OLO_CORE_ASSERT(false, "Parent entity must belong to the same scene as child");
				return;
			}

			// Guard against cyclic relationships
			if (parent && WouldCreateCycle(parent))
			{
				OLO_CORE_ASSERT(false, "Setting parent would create a cyclic relationship");
				return;
			}

			Entity currentParent = GetParent();
			if (currentParent == parent)
				return;

			// If changing parent, remove child from existing parent
			if (currentParent)
				currentParent.RemoveChild(*this);

			// Setting to null is okay - check if parent is valid before calling GetUUID()
			SetParentUUID(parent ? parent.GetUUID() : UUID(0));

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
			
			// Ensure child belongs to the same scene to prevent cross-scene inconsistencies
			OLO_CORE_ASSERT(child.m_Scene == m_Scene, "Child entity must belong to the same scene as parent");
				
			UUID childId = child.GetUUID();
			std::vector<UUID>& children = Children();
			auto it = std::find(children.begin(), children.end(), childId);
			if (it != children.end())
			{
				children.erase(it);
				
				// Clear the child's parent UUID to maintain relationship consistency
				child.SetParentUUID(UUID(0));
				
				return true;
			}
			return false;
		}

		bool operator==(const Entity& other) const
		{
			return (m_EntityHandle == other.m_EntityHandle) && (m_Scene == other.m_Scene);
		}

	private:
		// Helper method to check if setting a parent would create a cycle
		bool WouldCreateCycle(Entity potentialParent) const
		{
			// Traverse up the hierarchy from the potential parent
			Entity current = potentialParent;
			while (current)
			{
				// If we encounter this entity while traversing up from the potential parent,
				// it means setting the potential parent would create a cycle
				if (current == *this)
					return true;
				
				current = current.GetParent();
			}
			return false;
		}

		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};
}
