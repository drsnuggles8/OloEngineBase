#include "OloEnginePCH.h"
#include "Prefab.h"

#include "Scene.h"
#include "Components.h"
#include "OloEngine/Asset/AssetManager.h"

namespace OloEngine
{
    // ─────────────────────────────────────────────────────────────────────────
    // Helper: copy a single component type from src to dst via AddOrReplaceComponent
    // ─────────────────────────────────────────────────────────────────────────
#define COPY_COMPONENT(CompType, Name)                \
    if (sourceEntity.HasComponent<CompType>())        \
    {                                                 \
        targetEntity.AddOrReplaceComponent<CompType>( \
            sourceEntity.GetComponent<CompType>());   \
    }

#define CHECK_COMPONENT_PRESENCE(CompType, Name)                                \
    {                                                                           \
        bool inPrefab = prefabRoot.HasComponent<CompType>();                    \
        bool inInstance = instanceEntity.HasComponent<CompType>();              \
        if (inPrefab && !inInstance)                                            \
            outRemoved.insert(Name);                                            \
        else if (!inPrefab && inInstance)                                       \
            outAdded.insert(Name);                                              \
        else if (inPrefab && inInstance)                                        \
        {                                                                       \
            /* Component exists in both; caller must compare for value diffs */ \
        }                                                                       \
    }

#define REVERT_COMPONENT(CompType, Name)                    \
    if (componentName == Name)                              \
    {                                                       \
        if (prefabRoot.HasComponent<CompType>())            \
        {                                                   \
            instanceEntity.AddOrReplaceComponent<CompType>( \
                prefabRoot.GetComponent<CompType>());       \
        }                                                   \
        else                                                \
        {                                                   \
            if (instanceEntity.HasComponent<CompType>())    \
                instanceEntity.RemoveComponent<CompType>(); \
        }                                                   \
        return true;                                        \
    }

#define APPLY_COMPONENT(CompType, Name)                   \
    if (componentName == Name)                            \
    {                                                     \
        if (instanceEntity.HasComponent<CompType>())      \
        {                                                 \
            prefabRoot.AddOrReplaceComponent<CompType>(   \
                instanceEntity.GetComponent<CompType>()); \
        }                                                 \
        return true;                                      \
    }

#define UPDATE_COMPONENT(CompType, Name)                               \
    if (!pc.IsComponentOverridden(Name) && !pc.IsComponentAdded(Name)) \
    {                                                                  \
        if (prefabRoot.HasComponent<CompType>())                       \
        {                                                              \
            instanceEntity.AddOrReplaceComponent<CompType>(            \
                prefabRoot.GetComponent<CompType>());                  \
        }                                                              \
    }

    // ─────────────────────────────────────────────────────────────────────────
    // All copyable component names + macros applied in bulk
    // ─────────────────────────────────────────────────────────────────────────
    // NOTE: IDComponent, TagComponent, PrefabComponent, and RelationshipComponent
    // are handled specially and excluded from the generic copy list.
#define FOR_EACH_COPYABLE_COMPONENT(MACRO)                                    \
    MACRO(TransformComponent, "TransformComponent")                           \
    MACRO(CameraComponent, "CameraComponent")                                 \
    MACRO(ScriptComponent, "ScriptComponent")                                 \
    MACRO(SpriteRendererComponent, "SpriteRendererComponent")                 \
    MACRO(CircleRendererComponent, "CircleRendererComponent")                 \
    MACRO(MeshComponent, "MeshComponent")                                     \
    MACRO(SubmeshComponent, "SubmeshComponent")                               \
    MACRO(ModelComponent, "ModelComponent")                                   \
    MACRO(SkeletonComponent, "SkeletonComponent")                             \
    MACRO(AnimationStateComponent, "AnimationStateComponent")                 \
    MACRO(MaterialComponent, "MaterialComponent")                             \
    MACRO(Rigidbody2DComponent, "Rigidbody2DComponent")                       \
    MACRO(BoxCollider2DComponent, "BoxCollider2DComponent")                   \
    MACRO(CircleCollider2DComponent, "CircleCollider2DComponent")             \
    MACRO(Rigidbody3DComponent, "Rigidbody3DComponent")                       \
    MACRO(BoxCollider3DComponent, "BoxCollider3DComponent")                   \
    MACRO(SphereCollider3DComponent, "SphereCollider3DComponent")             \
    MACRO(CapsuleCollider3DComponent, "CapsuleCollider3DComponent")           \
    MACRO(MeshCollider3DComponent, "MeshCollider3DComponent")                 \
    MACRO(ConvexMeshCollider3DComponent, "ConvexMeshCollider3DComponent")     \
    MACRO(TriangleMeshCollider3DComponent, "TriangleMeshCollider3DComponent") \
    MACRO(CharacterController3DComponent, "CharacterController3DComponent")   \
    MACRO(TextComponent, "TextComponent")                                     \
    MACRO(AudioSourceComponent, "AudioSourceComponent")                       \
    MACRO(AudioListenerComponent, "AudioListenerComponent")                   \
    MACRO(DirectionalLightComponent, "DirectionalLightComponent")             \
    MACRO(PointLightComponent, "PointLightComponent")                         \
    MACRO(SpotLightComponent, "SpotLightComponent")                           \
    MACRO(EnvironmentMapComponent, "EnvironmentMapComponent")                 \
    MACRO(LightProbeComponent, "LightProbeComponent")                         \
    MACRO(LightProbeVolumeComponent, "LightProbeVolumeComponent")             \
    MACRO(UICanvasComponent, "UICanvasComponent")                             \
    MACRO(UIRectTransformComponent, "UIRectTransformComponent")               \
    MACRO(UIImageComponent, "UIImageComponent")                               \
    MACRO(UIPanelComponent, "UIPanelComponent")                               \
    MACRO(UITextComponent, "UITextComponent")                                 \
    MACRO(UIButtonComponent, "UIButtonComponent")                             \
    MACRO(UISliderComponent, "UISliderComponent")                             \
    MACRO(UICheckboxComponent, "UICheckboxComponent")                         \
    MACRO(UIProgressBarComponent, "UIProgressBarComponent")                   \
    MACRO(UIInputFieldComponent, "UIInputFieldComponent")                     \
    MACRO(UIScrollViewComponent, "UIScrollViewComponent")                     \
    MACRO(UIDropdownComponent, "UIDropdownComponent")                         \
    MACRO(UIGridLayoutComponent, "UIGridLayoutComponent")                     \
    MACRO(UIToggleComponent, "UIToggleComponent")                             \
    MACRO(ParticleSystemComponent, "ParticleSystemComponent")                 \
    MACRO(TerrainComponent, "TerrainComponent")                               \
    MACRO(FoliageComponent, "FoliageComponent")                               \
    MACRO(WaterComponent, "WaterComponent")                                   \
    MACRO(SnowDeformerComponent, "SnowDeformerComponent")                     \
    MACRO(FogVolumeComponent, "FogVolumeComponent")                           \
    MACRO(DecalComponent, "DecalComponent")                                   \
    MACRO(LODGroupComponent, "LODGroupComponent")                             \
    MACRO(NetworkIdentityComponent, "NetworkIdentityComponent")               \
    MACRO(NetworkInterestComponent, "NetworkInterestComponent")               \
    MACRO(PhaseComponent, "PhaseComponent")                                   \
    MACRO(InstancePortalComponent, "InstancePortalComponent")                 \
    MACRO(NetworkLODComponent, "NetworkLODComponent")                         \
    MACRO(DialogueComponent, "DialogueComponent")

    // ─────────────────────────────────────────────────────────────────────────
    // Construction / Destruction
    // ─────────────────────────────────────────────────────────────────────────

    Prefab::Prefab()
        : m_Scene(nullptr), m_Entity{}
    {
        m_Scene = Scene::Create();
    }

    Prefab::~Prefab() = default;

    // ─────────────────────────────────────────────────────────────────────────
    // Create - build a prefab from a scene entity (with full hierarchy)
    // ─────────────────────────────────────────────────────────────────────────

    void Prefab::Create(Entity entity, bool serialize)
    {
        m_Scene = Scene::Create();
        m_Entity = CreatePrefabFromEntity(entity);

        // Recursively copy children
        CreatePrefabChildrenRecursive(entity, m_Entity);

        (void)serialize;
    }

    Entity Prefab::CreatePrefabFromEntity(Entity entity)
    {
        OLO_CORE_ASSERT(GetHandle() != 0, "Prefab handle must be set before creating prefab from entity");

        Entity newEntity = m_Scene->CreateEntity();

        // Mark as belonging to this prefab
        newEntity.AddComponent<PrefabComponent>(GetHandle(), newEntity.GetComponent<IDComponent>().ID);

        // Copy tag
        if (entity.HasComponent<TagComponent>())
        {
            newEntity.AddOrReplaceComponent<TagComponent>(entity.GetComponent<TagComponent>());
        }

        // Copy all serializable components
        CopyEntityComponents(entity, newEntity);

        return newEntity;
    }

    void Prefab::CreatePrefabChildrenRecursive(Entity sourceEntity, Entity prefabParent)
    {
        const auto& children = sourceEntity.Children();
        if (children.empty())
            return;

        Scene* sourceScene = sourceEntity.GetScene();
        for (const UUID& childUUID : children)
        {
            auto childOpt = sourceScene->TryGetEntityWithUUID(childUUID);
            if (!childOpt)
                continue;

            Entity sourceChild = *childOpt;

            // Create child in prefab scene
            Entity prefabChild = m_Scene->CreateEntity();
            prefabChild.AddComponent<PrefabComponent>(GetHandle(), prefabChild.GetComponent<IDComponent>().ID);

            if (sourceChild.HasComponent<TagComponent>())
            {
                prefabChild.AddOrReplaceComponent<TagComponent>(sourceChild.GetComponent<TagComponent>());
            }

            CopyEntityComponents(sourceChild, prefabChild);

            // Wire up parent-child relationship in the prefab scene
            prefabChild.SetParent(prefabParent);

            // Recurse into grandchildren
            CreatePrefabChildrenRecursive(sourceChild, prefabChild);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Copy all serializable components (excluding ID, Tag, Prefab, Relationship)
    // ─────────────────────────────────────────────────────────────────────────

    void Prefab::CopyEntityComponents(Entity sourceEntity, Entity targetEntity) const {
        FOR_EACH_COPYABLE_COMPONENT(COPY_COMPONENT)
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Asset dependency scanning
    // ─────────────────────────────────────────────────────────────────────────

    std::pair<std::unordered_set<AssetHandle>, std::unordered_set<AssetHandle>> Prefab::GetAssetList(bool recursive)
    {
        std::unordered_set<AssetHandle> assets;
        std::unordered_set<AssetHandle> missingAssets;

        if (!m_Scene)
            return { assets, missingAssets };

        // Scan for nested prefabs
        if (recursive)
        {
            auto prefabView = m_Scene->GetAllEntitiesWith<PrefabComponent>();
            for (auto e : prefabView)
            {
                Entity entity{ e, *m_Scene };
                auto& pc = entity.GetComponent<PrefabComponent>();
                if (pc.m_PrefabID != GetHandle() && static_cast<u64>(pc.m_PrefabID) != 0)
                {
                    if (AssetManager::IsAssetValid(pc.m_PrefabID))
                        assets.insert(pc.m_PrefabID);
                    else
                        missingAssets.insert(pc.m_PrefabID);
                }
            }
        }

        return { assets, missingAssets };
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Instantiate - create entity hierarchy in target scene
    // ─────────────────────────────────────────────────────────────────────────

    Entity Prefab::Instantiate(Scene& targetScene, UUID uuid) const
    {
        if (!m_Scene || !m_Entity)
        {
            OLO_CORE_ERROR("Prefab::Instantiate - Prefab has no valid scene or entity");
            return {};
        }

        if (!uuid)
            uuid = UUID();

        std::string entityName = "Prefab Instance";
        if (m_Entity.HasComponent<TagComponent>())
        {
            entityName = m_Entity.GetComponent<TagComponent>().Tag;
        }

        Entity targetEntity = targetScene.CreateEntityWithUUID(uuid, entityName);
        if (!targetEntity)
        {
            OLO_CORE_ERROR("Prefab::Instantiate - Failed to create entity in target scene");
            return {};
        }

        CopyEntityComponents(m_Entity, targetEntity);

        // Recursively instantiate children
        InstantiateChildrenRecursive(m_Entity, targetEntity, targetScene);

        return targetEntity;
    }

    void Prefab::InstantiateChildrenRecursive(Entity prefabParent, Entity targetParent, Scene& targetScene) const
    {
        const auto& children = prefabParent.Children();
        if (children.empty())
            return;

        for (const UUID& childUUID : children)
        {
            auto childOpt = m_Scene->TryGetEntityWithUUID(childUUID);
            if (!childOpt)
                continue;

            Entity prefabChild = *childOpt;

            std::string childName = "Child";
            if (prefabChild.HasComponent<TagComponent>())
            {
                childName = prefabChild.GetComponent<TagComponent>().Tag;
            }

            Entity targetChild = targetScene.CreateEntity(childName);
            CopyEntityComponents(prefabChild, targetChild);

            // Preserve the prefab link if the child was a nested prefab instance
            if (prefabChild.HasComponent<PrefabComponent>())
            {
                auto& srcPc = prefabChild.GetComponent<PrefabComponent>();
                auto& dstPc = targetChild.AddOrReplaceComponent<PrefabComponent>();
                dstPc.m_PrefabID = srcPc.m_PrefabID;
                dstPc.m_PrefabEntityID = targetChild.GetUUID();
                dstPc.OverriddenComponents = srcPc.OverriddenComponents;
                dstPc.AddedComponents = srcPc.AddedComponents;
                dstPc.RemovedComponents = srcPc.RemovedComponents;
            }

            targetChild.SetParent(targetParent);

            // Recurse
            InstantiateChildrenRecursive(prefabChild, targetChild, targetScene);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Override detection
    // ─────────────────────────────────────────────────────────────────────────

    void Prefab::DetectOverrides(Entity instanceEntity,
                                 std::unordered_set<std::string>& outOverridden,
                                 std::unordered_set<std::string>& outAdded,
                                 std::unordered_set<std::string>& outRemoved) const
    {
        if (!m_Entity)
            return;

        Entity prefabRoot = m_Entity;

        // Check each copyable component for presence differences
        FOR_EACH_COPYABLE_COMPONENT(CHECK_COMPONENT_PRESENCE)

        // For components present in both, compare serialized YAML to detect value overrides.
        // This is done by serializing both entities and comparing component blocks.
        // For now, we mark as overridden any component the user has explicitly flagged.
        // The editor will manage the OverriddenComponents set as the user modifies properties.
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Revert a single component on an instance back to prefab defaults
    // ─────────────────────────────────────────────────────────────────────────

    bool Prefab::RevertComponent(Entity instanceEntity, const std::string& componentName) const
    {
        if (!m_Entity)
            return false;

        Entity prefabRoot = m_Entity;

        FOR_EACH_COPYABLE_COMPONENT(REVERT_COMPONENT)

        OLO_CORE_WARN("Prefab::RevertComponent - Unknown component name: {}", componentName);
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Apply instance override back to the source prefab
    // ─────────────────────────────────────────────────────────────────────────

    bool Prefab::ApplyComponentToPrefab(Entity instanceEntity, const std::string& componentName)
    {
        if (!m_Entity)
            return false;

        Entity prefabRoot = m_Entity;

        FOR_EACH_COPYABLE_COMPONENT(APPLY_COMPONENT)

        OLO_CORE_WARN("Prefab::ApplyComponentToPrefab - Unknown component name: {}", componentName);
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Update all non-overridden components on an instance from the prefab
    // ─────────────────────────────────────────────────────────────────────────

    void Prefab::UpdateInstanceFromPrefab(Entity instanceEntity) const
    {
        if (!m_Entity)
            return;

        if (!instanceEntity.HasComponent<PrefabComponent>())
            return;

        const auto& pc = instanceEntity.GetComponent<PrefabComponent>();
        Entity prefabRoot = m_Entity;

        FOR_EACH_COPYABLE_COMPONENT(UPDATE_COMPONENT)
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Nested prefab utilities
    // ─────────────────────────────────────────────────────────────────────────

    bool Prefab::HasNestedPrefabs() const
    {
        if (!m_Scene)
            return false;

        auto view = m_Scene->GetAllEntitiesWith<PrefabComponent>();
        for (auto e : view)
        {
            Entity entity{ e, *m_Scene };
            auto& pc = entity.GetComponent<PrefabComponent>();
            // If the PrefabComponent references a different prefab, it's nested
            if (pc.m_PrefabID != GetHandle() && static_cast<u64>(pc.m_PrefabID) != 0)
                return true;
        }
        return false;
    }

    bool Prefab::WouldCreateCycle(AssetHandle rootHandle, AssetHandle prefabHandle,
                                  std::unordered_set<AssetHandle>& visited)
    {
        if (rootHandle == prefabHandle)
            return true;

        if (visited.contains(prefabHandle))
            return false; // Already checked this path, no cycle found

        visited.insert(prefabHandle);

        if (!AssetManager::IsAssetValid(prefabHandle))
            return false;

        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(prefabHandle);
        if (!prefab || !prefab->GetScene())
            return false;

        // Check all PrefabComponents in the nested prefab's scene
        auto view = prefab->GetScene()->GetAllEntitiesWith<PrefabComponent>();
        for (auto e : view)
        {
            Entity entity{ e, *prefab->GetScene() };
            auto& pc = entity.GetComponent<PrefabComponent>();
            if (static_cast<u64>(pc.m_PrefabID) == 0 || pc.m_PrefabID == prefabHandle)
                continue;

            if (pc.m_PrefabID == rootHandle)
                return true;

            if (WouldCreateCycle(rootHandle, pc.m_PrefabID, visited))
                return true;
        }

        return false;
    }

    // Clean up macros
#undef COPY_COMPONENT
#undef CHECK_COMPONENT_PRESENCE
#undef REVERT_COMPONENT
#undef APPLY_COMPONENT
#undef UPDATE_COMPONENT
#undef FOR_EACH_COPYABLE_COMPONENT

} // namespace OloEngine
