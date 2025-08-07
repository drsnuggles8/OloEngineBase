#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"

#include "box2d/box2d.h" // Include Box2D header

#pragma warning( push )
#pragma warning( disable : 4996)
#include "entt.hpp"
#pragma warning( pop )

class b2World;

namespace OloEngine
{

	class Entity;

	class Scene : public Asset
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Create();
		static Ref<Scene> Copy(Ref<Scene>& other);

		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();

		void OnUpdateRuntime(Timestep ts);
		void OnUpdateSimulation(Timestep ts, EditorCamera const& camera);
		void OnUpdateEditor(Timestep ts, EditorCamera const& camera);
		void OnViewportResize(u32 width, u32 height);

		void DuplicateEntity(Entity entity);

		Entity FindEntityByName(std::string_view name);
		Entity GetEntityByUUID(UUID uuid);

		Entity GetPrimaryCameraEntity();

		Entity FindEntityByName(std::string_view name) const;
		Entity GetEntityByUUID(UUID uuid) const;

		Entity GetPrimaryCameraEntity() const;

		// Bone entity management (Hazel-style)
		std::vector<glm::mat4> GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, Ref<class MeshSource> meshSource);
		std::vector<UUID> FindBoneEntityIds(Entity entity, Entity rootEntity, const class Skeleton* skeleton) const;
		glm::mat3 FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const;
		void BuildBoneEntityIds(Entity entity);
		void BuildMeshBoneEntityIds(Entity entity, Entity rootEntity);
		void BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity);

		// Entity lookup utilities
		Entity TryGetEntityWithUUID(UUID id) const;
		Entity GetEntityWithUUID(UUID id) const;

		[[nodiscard("Store this!")]] bool IsRunning() const { return m_IsRunning; }
        [[nodiscard("Store this!")]] bool IsPaused() const { return m_IsPaused; }

		void SetPaused(bool paused) { m_IsPaused = paused; }

		void Step(int frames = 1);

		void SetName(std::string_view name);
		[[nodiscard("Store this!")]] const std::string & GetName() const { return m_Name; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}

		template<typename... Components>
		auto GetAllEntitiesWith() const
		{
			return m_Registry.view<Components...>();
		}

		// Asset interface
		static AssetType GetStaticType() { return AssetType::Scene; }
		virtual AssetType GetAssetType() const override { return GetStaticType(); }

	private:
		template<typename T>
		void OnComponentAdded(Entity entity, T& component);

		void OnPhysics2DStart();
		void OnPhysics2DStop();

		void RenderScene(EditorCamera const& camera);
	private:
		entt::registry m_Registry;
		u32 m_ViewportWidth = 0;
		u32 m_ViewportHeight = 0;
		bool m_IsRunning = false;
		bool m_IsPaused = false;
		int m_StepFrames = 0;

		b2WorldId m_PhysicsWorld = b2_nullWorldId;

		std::unordered_map<UUID, entt::entity> m_EntityMap;

		std::string m_Name = "Untitled";

		friend class Entity;
		friend class SceneSerializer;
		friend class SceneHierarchyPanel;
	};
}
