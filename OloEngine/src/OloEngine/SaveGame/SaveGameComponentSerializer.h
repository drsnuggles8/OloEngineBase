#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    class FArchive;

    // Forward-declare all component types
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
    struct TextComponent;
    struct ScriptComponent;
    struct AudioSourceComponent;
    struct AudioListenerComponent;
    struct MaterialComponent;
    struct DirectionalLightComponent;
    struct PointLightComponent;
    struct SpotLightComponent;
    struct EnvironmentMapComponent;
    struct LightProbeComponent;
    struct LightProbeVolumeComponent;
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
    struct SnowDeformerComponent;
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
    struct StreamingVolumeComponent;

    // Type-erased save-game serialization function
    using SaveGameSerializeFn = std::function<void(FArchive&, void*)>;

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
        static void Serialize(FArchive& ar, TextComponent& c);
        static void Serialize(FArchive& ar, ScriptComponent& c);
        static void Serialize(FArchive& ar, AudioSourceComponent& c);
        static void Serialize(FArchive& ar, AudioListenerComponent& c);
        static void Serialize(FArchive& ar, MaterialComponent& c);
        static void Serialize(FArchive& ar, DirectionalLightComponent& c);
        static void Serialize(FArchive& ar, PointLightComponent& c);
        static void Serialize(FArchive& ar, SpotLightComponent& c);
        static void Serialize(FArchive& ar, EnvironmentMapComponent& c);
        static void Serialize(FArchive& ar, LightProbeComponent& c);
        static void Serialize(FArchive& ar, LightProbeVolumeComponent& c);
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
        static void Serialize(FArchive& ar, SnowDeformerComponent& c);
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
        static void Serialize(FArchive& ar, StreamingVolumeComponent& c);

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
