#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"

#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include "box2d/box2d.h"

#pragma warning( push )
#pragma warning( disable : 4996)
#include "entt.hpp"
#pragma warning( pop )

class b2World;

namespace OloEngine
{
	class Entity;
	class MeshSource;
	class Skeleton;
	class Prefab;

	class Scene : public Asset
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Create();
		static Ref<Scene> Copy(Ref<Scene>& other);

		[[nodiscard]] Entity CreateEntity(const std::string& name = std::string());
		[[nodiscard]] Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		void DestroyEntity(Entity entity);

		// Prefab instantiation
		[[nodiscard]] Entity Instantiate(AssetHandle prefabHandle);
		[[nodiscard]] Entity InstantiateWithUUID(AssetHandle prefabHandle, UUID uuid);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();

		void OnUpdateRuntime(Timestep ts);
		void OnUpdateSimulation(Timestep ts, EditorCamera const& camera);
		void OnUpdateEditor(Timestep ts, EditorCamera const& camera);
		void OnViewportResize(u32 width, u32 height);

		void DuplicateEntity(Entity entity);

		[[nodiscard]] Entity FindEntityByName(std::string_view name);
		[[nodiscard]] Entity GetEntityByUUID(UUID uuid);

		[[nodiscard]] Entity GetPrimaryCameraEntity();

		[[nodiscard]] Entity FindEntityByName(std::string_view name) const;
		[[nodiscard]] Entity GetEntityByUUID(UUID uuid) const;

		[[nodiscard]] Entity GetPrimaryCameraEntity() const;

		// Bone entity management (Hazel-style)
		std::vector<glm::mat4> GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, const MeshSource& meshSource) const;
		std::vector<UUID> FindBoneEntityIds(Entity rootEntity, const Skeleton& skeleton) const;
		glm::mat4 FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const;
		void BuildBoneEntityIds(Entity entity);
		void BuildMeshBoneEntityIds(Entity entity, Entity rootEntity);
		void BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity);

		// Entity lookup utilities
		[[nodiscard]] std::optional<Entity> TryGetEntityWithUUID(UUID id) const;

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
