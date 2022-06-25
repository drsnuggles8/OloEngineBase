#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/EditorCamera.h"

#include "entt.hpp"

class b2World;

namespace OloEngine {

	class Entity;

	class Scene
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Copy(Ref<Scene> const other);

		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(const UUID uuid, const std::string& name = std::string());
		void DestroyEntity(Entity const entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();

		void OnUpdateRuntime(Timestep const ts);
		void OnUpdateSimulation(const Timestep ts, EditorCamera const& camera);
		void OnUpdateEditor(Timestep const ts, EditorCamera& camera);
		void OnViewportResize(uint32_t width, uint32_t height);

		void DuplicateEntity(Entity entity);

		Entity GetPrimaryCameraEntity();

		void SetName(const std::string& name);
		const std::string& GetName() const { return m_Name; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}
	private:
		template<typename T>
		void OnComponentAdded(Entity entity, T& component);

		void OnPhysics2DStart();
		void OnPhysics2DStop();

		void RenderScene(EditorCamera const& camera);
	private:
		entt::registry m_Registry;
		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;

		b2World* m_PhysicsWorld = nullptr;

		std::string m_Name = "Untitled";

		friend class Entity;
		friend class SceneSerializer;
		friend class SceneHierarchyPanel;
	};

}
