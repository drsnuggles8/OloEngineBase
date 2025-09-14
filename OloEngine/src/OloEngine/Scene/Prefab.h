#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include <unordered_set>
#include <utility>

namespace OloEngine
{
    /**
     * @brief Prefab asset containing a reusable entity hierarchy
     * 
     * Prefabs allow creating reusable entity templates that can be instantiated
     * multiple times in scenes. They store a complete entity hierarchy with all
     * components and can be nested within other prefabs.
     * 
     * Features:
     * - Complete entity hierarchy serialization
     * - Nested prefab support
     * - Asset dependency tracking
     * - Runtime instantiation with transform overrides
     */
    class Prefab : public Asset
    {
    public:
        Prefab();
        virtual ~Prefab();

        /**
         * @brief Create prefab from an existing entity
         * @param entity Source entity to create prefab from
         * @param serialize Whether to immediately serialize the prefab to disk
         */
        void Create(Entity entity, bool serialize = true);

        /**
         * @brief Get static asset type
         * @return AssetType::Prefab
         */
        static AssetType GetStaticType() { return AssetType::Prefab; }

        /**
         * @brief Get asset type of this instance
         * @return AssetType::Prefab
         */
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

        /**
         * @brief Get list of all assets referenced by this prefab
         * @param recursive Whether to recursively include assets from nested prefabs
         * @return Pair of (asset list, missing asset list)
         */
        std::pair<std::unordered_set<AssetHandle>, std::unordered_set<AssetHandle>> 
            GetAssetList(bool recursive = true);

        /**
         * @brief Get the prefab's scene containing the entity hierarchy
         * @return Reference to the prefab scene
         */
        Ref<Scene> GetScene() const { return m_Scene; }

        /**
         * @brief Get the root entity of the prefab
         * @return Root entity of the prefab
         */
        Entity GetRootEntity() const { return m_Entity; }

        /**
         * @brief Instantiate prefab into a target scene
         * @param targetScene Scene to instantiate the prefab into
         * @param uuid Optional UUID for the instantiated entity (auto-generated if not provided)
         * @return Instantiated entity in the target scene
         */
        Entity Instantiate(Scene& targetScene, UUID uuid = UUID()) const;

    private:
        /**
         * @brief Create prefab entity by copying from source entity
         * @param entity Source entity to copy from
         * @return New prefab entity
         */
        Entity CreatePrefabFromEntity(Entity entity);

        /**
         * @brief Copy all components from source entity to target entity
         * @param sourceEntity Entity to copy components from
         * @param targetEntity Entity to copy components to
         */
        void CopyEntityComponents(Entity sourceEntity, Entity targetEntity) const;

    private:
        Ref<Scene> m_Scene;
        Entity m_Entity;

        friend class Scene;
        friend class PrefabSerializer;
        friend class PrefabEditor;
    };
}
