#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace OloEngine
{
    // @brief Prefab asset containing a reusable entity hierarchy
    //
    // Prefabs allow creating reusable entity templates that can be instantiated
    // multiple times in scenes. They store a complete entity hierarchy with all
    // components and can be nested within other prefabs.
    //
    // Features:
    // - Complete entity hierarchy serialization
    // - Nested prefab support with cycle detection
    // - Per-instance component-level overrides
    // - Asset dependency tracking
    // - Runtime instantiation with transform overrides
    class Prefab : public Asset
    {
      public:
        Prefab();
        virtual ~Prefab();

        // @brief Create prefab from an existing entity (including children)
        // @param entity Source entity to create prefab from
        // @param serialize Whether to immediately serialize the prefab to disk
        void Create(Entity entity, bool serialize = true);

        // @brief Get static asset type
        // @return AssetType::Prefab
        static AssetType GetStaticType()
        {
            return AssetType::Prefab;
        }

        // @brief Get asset type of this instance
        // @return AssetType::Prefab
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // @brief Get list of all assets referenced by this prefab
        // @param recursive Whether to recursively include assets from nested prefabs
        // @return Pair of (asset list, missing asset list)
        std::pair<std::unordered_set<AssetHandle>, std::unordered_set<AssetHandle>>
        GetAssetList(bool recursive = true);

        // @brief Get the prefab's scene containing the entity hierarchy
        // @return Reference to the prefab scene
        Ref<Scene> GetScene() const
        {
            return m_Scene;
        }

        // @brief Get the root entity of the prefab
        // @return Root entity of the prefab
        Entity GetRootEntity() const
        {
            return m_Entity;
        }

        // @brief Instantiate prefab into a target scene (including nested children)
        // @param targetScene Scene to instantiate the prefab into
        // @param uuid Optional UUID for the root entity (auto-generated if not provided)
        // @return Instantiated root entity in the target scene
        Entity Instantiate(Scene& targetScene, UUID uuid = UUID()) const;

        // --- Override detection ---

        // @brief Detect which components differ between a prefab source entity and a scene instance.
        // @param instanceEntity The in-scene instance whose components are compared against the prefab.
        // @param outOverridden Components present in both but with different values.
        // @param outAdded Components present on the instance but not in the prefab.
        // @param outRemoved Components present in the prefab but not on the instance.
        void DetectOverrides(Entity instanceEntity,
                             std::unordered_set<std::string>& outOverridden,
                             std::unordered_set<std::string>& outAdded,
                             std::unordered_set<std::string>& outRemoved) const;

        // @brief Revert a single component on an instance back to prefab defaults.
        // @param instanceEntity The in-scene instance to revert.
        // @param componentName Name of the component to revert (e.g. "TransformComponent").
        // @return true if the component was successfully reverted.
        bool RevertComponent(Entity instanceEntity, const std::string& componentName) const;

        // @brief Apply a component override from an instance back to the source prefab.
        // @param instanceEntity The in-scene instance to read the component value from.
        // @param componentName Name of the component to apply.
        // @return true if the component was successfully applied to the prefab.
        bool ApplyComponentToPrefab(Entity instanceEntity, const std::string& componentName);

        // @brief Update all non-overridden components on an instance from the prefab.
        // @param instanceEntity The in-scene instance to update.
        void UpdateInstanceFromPrefab(Entity instanceEntity) const;

        // --- Nested prefab utilities ---

        // @brief Check if this prefab contains nested prefab instances.
        [[nodiscard]] bool HasNestedPrefabs() const;

        // @brief Check for cycles: would adding prefabHandle as a nested instance create a cycle?
        // @param prefabHandle The prefab asset handle to test.
        // @param visited Accumulator for cycle detection (empty on first call).
        [[nodiscard]] static bool WouldCreateCycle(AssetHandle rootHandle, AssetHandle prefabHandle,
                                                   std::unordered_set<AssetHandle>& visited);

      private:
        // @brief Create prefab entity by copying from source entity (recursive for children)
        // @param entity Source entity to copy from
        // @return New prefab entity (root)
        Entity CreatePrefabFromEntity(Entity entity);

        // @brief Recursively copy child entities from source into the prefab scene.
        void CreatePrefabChildrenRecursive(Entity sourceEntity, Entity prefabParent);

        // @brief Copy all components from source entity to target entity
        // @param sourceEntity Entity to copy components from
        // @param targetEntity Entity to copy components to
        void CopyEntityComponents(Entity sourceEntity, Entity targetEntity) const;

        // @brief Recursively instantiate children of a prefab entity into the target scene.
        void InstantiateChildrenRecursive(Entity prefabParent, Entity targetParent, Scene& targetScene) const;

      private:
        Ref<Scene> m_Scene;
        Entity m_Entity;

        friend class Scene;
        friend class PrefabSerializer;
        friend class PrefabEditor;
    };
} // namespace OloEngine
