#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <unordered_map>

namespace OloEngine
{
    class FArchive;

    // Forward-declare all component types
    // NOTE: When adding a new component, update FIVE places:
    //   1. Forward declaration here
    //   2. Serialize() overload declaration below
    //   3. RegisterAll() in SaveGameComponentSerializer.cpp
    //   4. ScriptCore (C#) bindings in OloEngine-ScriptCore
    //   5. LuaScriptCore (Lua) bindings in OloEngine-LuaScriptCore
    struct IDComponent;
    struct TagComponent;
    struct PrefabComponent;
    struct TransformComponent;
    struct RelationshipComponent;
    struct SpriteRendererComponent;
    struct CircleRendererComponent;
    struct CameraComponent;
    struct Rigidbody2DComponent;
    struct BoxCollider2DComponent;
    struct CircleCollider2DComponent;
    struct Rigidbody3DComponent;
    struct BoxCollider3DComponent;
    struct SphereCollider3DComponent;
    struct CapsuleCollider3DComponent;
    struct MeshCollider3DComponent;
    struct ConvexMeshCollider3DComponent;
    struct TriangleMeshCollider3DComponent;
    struct CharacterController3DComponent;
    struct PhysicsJoint3DComponent;
    struct VehicleComponent;
    struct RagdollComponent;
    struct ClothComponent;
    struct TextComponent;
    struct ScriptComponent;
    struct AudioSourceComponent;
    struct AudioListenerComponent;
    struct VideoOverlayComponent;
    struct VideoSurfaceComponent;
    struct MaterialComponent;
    struct DirectionalLightComponent;
    struct PointLightComponent;
    struct SpotLightComponent;
    struct SphereAreaLightComponent;
    struct EnvironmentMapComponent;
    struct ProceduralSkyComponent;
    struct StarNestSkyComponent;
    struct LightProbeComponent;
    struct LightProbeVolumeComponent;
    struct ReflectionProbeComponent;
    struct UICanvasComponent;
    struct UIRectTransformComponent;
    struct UIImageComponent;
    struct UIPanelComponent;
    struct UITextComponent;
    struct UIButtonComponent;
    struct UISliderComponent;
    struct UICheckboxComponent;
    struct UIProgressBarComponent;
    struct UIInputFieldComponent;
    struct UIScrollViewComponent;
    struct UIDropdownComponent;
    struct UIGridLayoutComponent;
    struct UIToggleComponent;
    struct ParticleSystemComponent;
    struct TerrainComponent;
    struct FoliageComponent;
    struct WaterComponent;
    struct BuoyancyComponent;
    struct SnowDeformerComponent;
    struct FluidComponent;
    struct FluidEmitterComponent;
    struct FluidKillVolumeComponent;
    struct FogVolumeComponent;
    struct DecalComponent;
    struct LODGroupComponent;
    struct NetworkIdentityComponent;
    struct NetworkInterestComponent;
    struct PhaseComponent;
    struct InstancePortalComponent;
    struct NetworkLODComponent;
    struct SubmeshComponent;
    struct MeshComponent;
    struct ModelComponent;
    struct AnimationStateComponent;
    struct SkeletonComponent;
    struct StreamingVolumeComponent;
    struct LuaScriptComponent;
    struct TileRendererComponent;
    struct DialogueComponent;
    struct NavMeshBoundsComponent;
    struct NavAgentComponent;
    struct NameplateComponent;
    struct IKTargetComponent;
    struct SpringBoneComponent;
    struct RetargetingComponent;
    struct FootIKComponent;
    struct LocomotionComponent;
    struct NoiseAnimationComponent;
    struct UIWorldAnchorComponent;
    struct MorphTargetComponent;
    struct InstancedMeshComponent;
    struct AnimationGraphComponent;
    struct CinematicComponent;
    struct BehaviorTreeComponent;
    struct StateMachineComponent;
    struct GoapAgentComponent;
    struct PerceptibleComponent;
    struct PerceptionComponent;
    struct InventoryComponent;
    struct ItemPickupComponent;
    struct ItemContainerComponent;
    struct QuestJournalComponent;
    struct QuestGiverComponent;
    struct AbilityComponent;

    // Type-erased save-game serialization function (raw pointer: no heap allocation)
    using SaveGameSerializeFn = void (*)(FArchive&, void*);

    // Save-game component serializer — covers ALL serializable components.
    // Separate from ComponentReplicator which only handles network-replicated subset.
    class SaveGameComponentSerializer
    {
      public:
        // Register all component serializers
        static void RegisterAll();

        // Typed convenience overloads (one per component)
        static void Serialize(FArchive& ar, IDComponent& c);
        static void Serialize(FArchive& ar, TagComponent& c);
        static void Serialize(FArchive& ar, PrefabComponent& c);
        static void Serialize(FArchive& ar, TransformComponent& c);
        static void Serialize(FArchive& ar, RelationshipComponent& c);
        static void Serialize(FArchive& ar, SpriteRendererComponent& c);
        static void Serialize(FArchive& ar, CircleRendererComponent& c);
        static void Serialize(FArchive& ar, CameraComponent& c);
        static void Serialize(FArchive& ar, Rigidbody2DComponent& c);
        static void Serialize(FArchive& ar, BoxCollider2DComponent& c);
        static void Serialize(FArchive& ar, CircleCollider2DComponent& c);
        static void Serialize(FArchive& ar, Rigidbody3DComponent& c);
        static void Serialize(FArchive& ar, BoxCollider3DComponent& c);
        static void Serialize(FArchive& ar, SphereCollider3DComponent& c);
        static void Serialize(FArchive& ar, CapsuleCollider3DComponent& c);
        static void Serialize(FArchive& ar, MeshCollider3DComponent& c);
        static void Serialize(FArchive& ar, ConvexMeshCollider3DComponent& c);
        static void Serialize(FArchive& ar, TriangleMeshCollider3DComponent& c);
        static void Serialize(FArchive& ar, CharacterController3DComponent& c);
        static void Serialize(FArchive& ar, PhysicsJoint3DComponent& c);
        static void Serialize(FArchive& ar, VehicleComponent& c);
        static void Serialize(FArchive& ar, RagdollComponent& c);
        static void Serialize(FArchive& ar, ClothComponent& c);
        static void Serialize(FArchive& ar, TextComponent& c);
        static void Serialize(FArchive& ar, ScriptComponent& c);
        static void Serialize(FArchive& ar, AudioSourceComponent& c);
        static void Serialize(FArchive& ar, AudioListenerComponent& c);
        static void Serialize(FArchive& ar, VideoOverlayComponent& c);
        static void Serialize(FArchive& ar, VideoSurfaceComponent& c);
        static void Serialize(FArchive& ar, MaterialComponent& c);
        static void Serialize(FArchive& ar, DirectionalLightComponent& c);
        static void Serialize(FArchive& ar, PointLightComponent& c);
        static void Serialize(FArchive& ar, SpotLightComponent& c);
        static void Serialize(FArchive& ar, SphereAreaLightComponent& c);
        static void Serialize(FArchive& ar, EnvironmentMapComponent& c);
        static void Serialize(FArchive& ar, ProceduralSkyComponent& c);
        static void Serialize(FArchive& ar, StarNestSkyComponent& c);
        static void Serialize(FArchive& ar, LightProbeComponent& c);
        static void Serialize(FArchive& ar, LightProbeVolumeComponent& c);
        static void Serialize(FArchive& ar, ReflectionProbeComponent& c);
        static void Serialize(FArchive& ar, UICanvasComponent& c);
        static void Serialize(FArchive& ar, UIRectTransformComponent& c);
        static void Serialize(FArchive& ar, UIImageComponent& c);
        static void Serialize(FArchive& ar, UIPanelComponent& c);
        static void Serialize(FArchive& ar, UITextComponent& c);
        static void Serialize(FArchive& ar, UIButtonComponent& c);
        static void Serialize(FArchive& ar, UISliderComponent& c);
        static void Serialize(FArchive& ar, UICheckboxComponent& c);
        static void Serialize(FArchive& ar, UIProgressBarComponent& c);
        static void Serialize(FArchive& ar, UIInputFieldComponent& c);
        static void Serialize(FArchive& ar, UIScrollViewComponent& c);
        static void Serialize(FArchive& ar, UIDropdownComponent& c);
        static void Serialize(FArchive& ar, UIGridLayoutComponent& c);
        static void Serialize(FArchive& ar, UIToggleComponent& c);
        static void Serialize(FArchive& ar, ParticleSystemComponent& c);
        static void Serialize(FArchive& ar, TerrainComponent& c);
        static void Serialize(FArchive& ar, FoliageComponent& c);
        static void Serialize(FArchive& ar, WaterComponent& c);
        static void Serialize(FArchive& ar, BuoyancyComponent& c);
        static void Serialize(FArchive& ar, SnowDeformerComponent& c);
        static void Serialize(FArchive& ar, FluidComponent& c);
        static void Serialize(FArchive& ar, FluidEmitterComponent& c);
        static void Serialize(FArchive& ar, FluidKillVolumeComponent& c);
        static void Serialize(FArchive& ar, FogVolumeComponent& c);
        static void Serialize(FArchive& ar, DecalComponent& c);
        static void Serialize(FArchive& ar, LODGroupComponent& c);
        static void Serialize(FArchive& ar, NetworkIdentityComponent& c);
        static void Serialize(FArchive& ar, NetworkInterestComponent& c);
        static void Serialize(FArchive& ar, PhaseComponent& c);
        static void Serialize(FArchive& ar, InstancePortalComponent& c);
        static void Serialize(FArchive& ar, NetworkLODComponent& c);
        static void Serialize(FArchive& ar, SubmeshComponent& c);
        static void Serialize(FArchive& ar, MeshComponent& c);
        static void Serialize(FArchive& ar, ModelComponent& c);
        static void Serialize(FArchive& ar, AnimationStateComponent& c);
        static void Serialize(FArchive& ar, SkeletonComponent& c);
        static void Serialize(FArchive& ar, StreamingVolumeComponent& c);
        static void Serialize(FArchive& ar, LuaScriptComponent& c);
        static void Serialize(FArchive& ar, TileRendererComponent& c);
        static void Serialize(FArchive& ar, DialogueComponent& c);
        static void Serialize(FArchive& ar, NavMeshBoundsComponent& c);
        static void Serialize(FArchive& ar, NavAgentComponent& c);
        static void Serialize(FArchive& ar, NameplateComponent& c);
        static void Serialize(FArchive& ar, IKTargetComponent& c);
        static void Serialize(FArchive& ar, SpringBoneComponent& c);
        static void Serialize(FArchive& ar, RetargetingComponent& c);
        static void Serialize(FArchive& ar, FootIKComponent& c);
        static void Serialize(FArchive& ar, LocomotionComponent& c);
        static void Serialize(FArchive& ar, NoiseAnimationComponent& c);
        static void Serialize(FArchive& ar, UIWorldAnchorComponent& c);
        static void Serialize(FArchive& ar, MorphTargetComponent& c);
        static void Serialize(FArchive& ar, InstancedMeshComponent& c);
        static void Serialize(FArchive& ar, AnimationGraphComponent& c);
        static void Serialize(FArchive& ar, CinematicComponent& c);
        static void Serialize(FArchive& ar, BehaviorTreeComponent& c);
        static void Serialize(FArchive& ar, StateMachineComponent& c);
        static void Serialize(FArchive& ar, GoapAgentComponent& c);
        static void Serialize(FArchive& ar, PerceptibleComponent& c);
        static void Serialize(FArchive& ar, PerceptionComponent& c);
        static void Serialize(FArchive& ar, InventoryComponent& c);
        static void Serialize(FArchive& ar, ItemPickupComponent& c);
        static void Serialize(FArchive& ar, ItemContainerComponent& c);
        static void Serialize(FArchive& ar, QuestJournalComponent& c);
        static void Serialize(FArchive& ar, QuestGiverComponent& c);
        static void Serialize(FArchive& ar, AbilityComponent& c);

        // Registry lookup by type hash
        static void Register(u32 typeHash, SaveGameSerializeFn serializer);
        [[nodiscard]] static const SaveGameSerializeFn* GetSerializer(u32 typeHash);
        [[nodiscard]] static const std::unordered_map<u32, SaveGameSerializeFn>& GetRegistry();
        static void ClearRegistry();

      private:
        // Type-safe registration helper used by RegisterAll()
        template<typename ComponentType>
        static void RegisterSaveComponent(const char* name);

        static std::unordered_map<u32, SaveGameSerializeFn> s_Registry;
    };

} // namespace OloEngine
