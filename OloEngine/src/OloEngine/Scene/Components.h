#pragma once
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/AudioEvents/CommandID.h"
#include "OloEngine/Video/VideoPlayer.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/StarNestSky.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainStreamer.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/MarchingCubes.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <box2d/id.h>

#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/norm.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine
{

    struct IDComponent
    {
        UUID ID;

        IDComponent() = default;
        IDComponent(const IDComponent&) = default;

        // Manual operator== — UUID's implicit operator u64() causes C2666
        // ambiguity with defaulted ==. Compare via explicit u64 conversion.
        auto operator==(const IDComponent& other) const -> bool
        {
            return static_cast<u64>(ID) == static_cast<u64>(other.ID);
        }
    };

    struct TagComponent
    {
        std::string Tag;
        bool renaming = false; // Transient editor flag — NOT authoring data.

        TagComponent() = default;
        TagComponent(const TagComponent& other) = default;
        explicit TagComponent(std::string tag)
            : Tag(std::move(tag)) {}

        explicit operator std::string&()
        {
            return Tag;
        }
        explicit operator const std::string&() const
        {
            return Tag;
        }

        // Authoring-only equality — `renaming` is a transient editor flag toggled
        // while the user is mid-rename. Including it would treat start/end of an
        // inline edit as a real change and pollute the undo stack.
        auto operator==(const TagComponent& other) const -> bool
        {
            return Tag == other.Tag;
        }
    };

    struct PrefabComponent
    {
        UUID m_PrefabID{};
        UUID m_PrefabEntityID{};

        // Component-level override tracking for prefab instances.
        // Components listed here have been intentionally modified on this instance
        // and will not be updated when the source prefab changes.
        std::unordered_set<std::string> m_OverriddenComponents;

        // Components added to this instance that do not exist in the source prefab.
        std::unordered_set<std::string> m_AddedComponents;

        // Components removed from this instance that exist in the source prefab.
        std::unordered_set<std::string> m_RemovedComponents;

        PrefabComponent() = default;
        PrefabComponent(const PrefabComponent&) = default;
        PrefabComponent(PrefabComponent&&) = default;
        PrefabComponent& operator=(const PrefabComponent&) = default;
        PrefabComponent& operator=(PrefabComponent&&) = default;
        PrefabComponent(UUID prefabID, UUID prefabEntityID)
            : m_PrefabID(prefabID), m_PrefabEntityID(prefabEntityID) {}

        [[nodiscard]] inline bool IsValid() const noexcept
        {
            return static_cast<u64>(m_PrefabID) != 0 && static_cast<u64>(m_PrefabEntityID) != 0;
        }

        [[nodiscard]] inline bool IsComponentOverridden(const std::string& componentName) const
        {
            return m_OverriddenComponents.contains(componentName);
        }

        [[nodiscard]] inline bool IsComponentAdded(const std::string& componentName) const
        {
            return m_AddedComponents.contains(componentName);
        }

        [[nodiscard]] inline bool IsComponentRemoved(const std::string& componentName) const
        {
            return m_RemovedComponents.contains(componentName);
        }

        [[nodiscard]] inline bool HasAnyOverrides() const noexcept
        {
            return !m_OverriddenComponents.empty() || !m_AddedComponents.empty() || !m_RemovedComponents.empty();
        }

        inline void MarkComponentOverridden(const std::string& componentName)
        {
            m_OverriddenComponents.insert(componentName);
        }

        inline void ClearComponentOverride(const std::string& componentName)
        {
            m_OverriddenComponents.erase(componentName);
            m_AddedComponents.erase(componentName);
            m_RemovedComponents.erase(componentName);
        }

        inline void ClearAllOverrides()
        {
            m_OverriddenComponents.clear();
            m_AddedComponents.clear();
            m_RemovedComponents.clear();
        }

        // Manual operator== — UUID members would trigger C2666 ambiguity with
        // defaulted ==. Compare UUIDs via u64.
        auto operator==(const PrefabComponent& other) const -> bool
        {
            return static_cast<u64>(m_PrefabID) == static_cast<u64>(other.m_PrefabID) && static_cast<u64>(m_PrefabEntityID) == static_cast<u64>(other.m_PrefabEntityID) && m_OverriddenComponents == other.m_OverriddenComponents && m_AddedComponents == other.m_AddedComponents && m_RemovedComponents == other.m_RemovedComponents;
        }
    };

    struct TransformComponent
    {
        OLO_PROPERTY()
        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

      private:
        OLO_PROPERTY(Name = "Rotation", Type = "vec3", Get = "comp.GetRotationEuler()", Set = "comp.SetRotationEuler({v})")
        glm::vec3 RotationEuler = { 0.0f, 0.0f, 0.0f };
        glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };

      public:
        TransformComponent() = default;
        TransformComponent(const TransformComponent& other) = default;
        explicit TransformComponent(const glm::vec3& translation)
            : Translation(translation) {}

        [[nodiscard("Store this!")]] glm::vec3 GetRotationEuler() const
        {
            return RotationEuler;
        }

        void SetRotationEuler(const glm::vec3& euler)
        {
            RotationEuler = euler;
            Rotation = glm::quat(euler);
        }

        [[nodiscard("Store this!")]] glm::quat GetRotation() const
        {
            return Rotation;
        }

        void SetRotation(const glm::quat& quat)
        {
            if (f32 const len2 = glm::dot(quat, quat); len2 < 1e-12f)
            {
                Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                RotationEuler = glm::vec3(0.0f);
                return;
            }

            auto wrapToPi = [](glm::vec3 v)
            {
                return glm::mod(v + glm::pi<float>(), 2.0f * glm::pi<float>()) - glm::pi<float>();
            };

            auto originalEuler = RotationEuler;
            Rotation = glm::normalize(quat);
            RotationEuler = glm::eulerAngles(Rotation);

            glm::vec3 alternate1 = { RotationEuler.x - glm::pi<float>(), glm::pi<float>() - RotationEuler.y, RotationEuler.z - glm::pi<float>() };
            glm::vec3 alternate2 = { RotationEuler.x + glm::pi<float>(), glm::pi<float>() - RotationEuler.y, RotationEuler.z - glm::pi<float>() };
            glm::vec3 alternate3 = { RotationEuler.x + glm::pi<float>(), glm::pi<float>() - RotationEuler.y, RotationEuler.z + glm::pi<float>() };
            glm::vec3 alternate4 = { RotationEuler.x - glm::pi<float>(), glm::pi<float>() - RotationEuler.y, RotationEuler.z + glm::pi<float>() };

            float distance0 = glm::length2(wrapToPi(RotationEuler - originalEuler));
            float distance1 = glm::length2(wrapToPi(alternate1 - originalEuler));
            float distance2 = glm::length2(wrapToPi(alternate2 - originalEuler));
            float distance3 = glm::length2(wrapToPi(alternate3 - originalEuler));
            float distance4 = glm::length2(wrapToPi(alternate4 - originalEuler));

            float best = distance0;
            if (distance1 < best)
            {
                best = distance1;
                RotationEuler = alternate1;
            }
            if (distance2 < best)
            {
                best = distance2;
                RotationEuler = alternate2;
            }
            if (distance3 < best)
            {
                best = distance3;
                RotationEuler = alternate3;
            }
            if (distance4 < best)
            {
                best = distance4;
                RotationEuler = alternate4;
            }

            // Unwrap each axis to be within ±π of the original Euler to maintain continuity
            auto constexpr twoPi = 2.0f * glm::pi<float>();
            for (int i = 0; i < 3; ++i)
            {
                float diff = RotationEuler[i] - originalEuler[i];
                if (diff > glm::pi<float>())
                    RotationEuler[i] -= twoPi;
                else if (diff < -glm::pi<float>())
                    RotationEuler[i] += twoPi;
                else
                {
                    // No additional handling required.
                }
            }
        }

        void SetTransform(const glm::mat4& transform);

        [[nodiscard("Store this!")]] glm::mat4 GetTransform() const
        {
            return glm::translate(glm::mat4(1.0f), Translation) * glm::toMat4(Rotation) * glm::scale(glm::mat4(1.0f), Scale);
        }

        auto operator==(const TransformComponent&) const -> bool = default;
    };

    struct SpriteRendererComponent
    {
        OLO_PROPERTY()
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        Ref<Texture2D> Texture = nullptr;
        OLO_PROPERTY()
        f32 TilingFactor = 1.0f;

        SpriteRendererComponent() = default;
        SpriteRendererComponent(const SpriteRendererComponent&) = default;
        explicit SpriteRendererComponent(const glm::vec4& color)
            : Color(color) {}

        auto operator==(const SpriteRendererComponent&) const -> bool = default;
    };

    struct CircleRendererComponent
    {
        OLO_PROPERTY()
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        f32 Thickness = 1.0f;
        OLO_PROPERTY()
        f32 Fade = 0.005f;

        CircleRendererComponent() = default;
        CircleRendererComponent(const CircleRendererComponent&) = default;

        auto operator==(const CircleRendererComponent&) const -> bool = default;
    };

    struct CameraComponent
    {
        // TODO(olbu): think about moving to Scene
        OLO_PROPERTY(Name = "ProjectionType", Type = "int", Get = "static_cast<int>(comp.Camera.GetProjectionType())", Set = "comp.Camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>({v}))")
        OLO_PROPERTY(Name = "PerspectiveFOV", Type = "float", Get = "comp.Camera.GetPerspectiveVerticalFOV()", Set = "comp.Camera.SetPerspectiveVerticalFOV({v})")
        OLO_PROPERTY(Name = "PerspectiveNearClip", Type = "float", Get = "comp.Camera.GetPerspectiveNearClip()", Set = "comp.Camera.SetPerspectiveNearClip({v})")
        OLO_PROPERTY(Name = "PerspectiveFarClip", Type = "float", Get = "comp.Camera.GetPerspectiveFarClip()", Set = "comp.Camera.SetPerspectiveFarClip({v})")
        OLO_PROPERTY(Name = "OrthographicSize", Type = "float", Get = "comp.Camera.GetOrthographicSize()", Set = "comp.Camera.SetOrthographicSize({v})")
        OLO_PROPERTY(Name = "OrthographicNearClip", Type = "float", Get = "comp.Camera.GetOrthographicNearClip()", Set = "comp.Camera.SetOrthographicNearClip({v})")
        OLO_PROPERTY(Name = "OrthographicFarClip", Type = "float", Get = "comp.Camera.GetOrthographicFarClip()", Set = "comp.Camera.SetOrthographicFarClip({v})")
        SceneCamera Camera;
        OLO_PROPERTY()
        bool Primary = true;
        OLO_PROPERTY()
        bool FixedAspectRatio = false;
        bool RuntimeControl = false;
        f32 FlySpeed = 5.0f;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;

        // Manual operator== — SceneCamera lacks defaulted ==. Compare the
        // user-visible projection state via getters, which captures every
        // serialized field. Float members go through Math::BitwiseEqual
        // (cpp-coding-quality §2a). Other fields are POD and trivially compared.
        auto operator==(const CameraComponent& other) const -> bool
        {
            if (Camera.GetProjectionType() != other.Camera.GetProjectionType())
                return false;
            if (const auto type = Camera.GetProjectionType(); type == SceneCamera::ProjectionType::Perspective)
            {
                if (!Math::BitwiseEqual(Camera.GetPerspectiveVerticalFOV(), other.Camera.GetPerspectiveVerticalFOV()))
                    return false;
                if (!Math::BitwiseEqual(Camera.GetPerspectiveNearClip(), other.Camera.GetPerspectiveNearClip()))
                    return false;
                if (!Math::BitwiseEqual(Camera.GetPerspectiveFarClip(), other.Camera.GetPerspectiveFarClip()))
                    return false;
            }
            else
            {
                if (!Math::BitwiseEqual(Camera.GetOrthographicSize(), other.Camera.GetOrthographicSize()))
                    return false;
                if (!Math::BitwiseEqual(Camera.GetOrthographicNearClip(), other.Camera.GetOrthographicNearClip()))
                    return false;
                if (!Math::BitwiseEqual(Camera.GetOrthographicFarClip(), other.Camera.GetOrthographicFarClip()))
                    return false;
            }
            return Primary == other.Primary && FixedAspectRatio == other.FixedAspectRatio && RuntimeControl == other.RuntimeControl && Math::BitwiseEqual(FlySpeed, other.FlySpeed);
        }
    };

    // Physics

    struct Rigidbody2DComponent
    {
        enum class BodyType
        {
            Static = 0,
            Dynamic,
            Kinematic
        };
        OLO_PROPERTY(Name = "Type", Type = "int", Get = "static_cast<int>(comp.Type)", Set = "comp.Type = static_cast<Rigidbody2DComponent::BodyType>({v})")
        BodyType Type = BodyType::Static;
        OLO_PROPERTY()
        bool FixedRotation = false;

        // Persisted velocity — snapshot from runtime before save, applied on body creation
        OLO_PROPERTY()
        glm::vec2 LinearVelocity = { 0.0f, 0.0f };
        OLO_PROPERTY()
        f32 AngularVelocity = 0.0f;

        // Storage for runtime
        b2BodyId RuntimeBody = b2_nullBodyId;

        Rigidbody2DComponent() = default;
        Rigidbody2DComponent(const Rigidbody2DComponent&) = default;

        // Compare only authored fields. RuntimeBody is a b2BodyId set by the
        // physics system when entering Play mode; including it in equality
        // would flag enter/exit-play as an authored change. Float fields use
        // Math::BitwiseEqual per cpp-coding-quality §2a.
        auto operator==(const Rigidbody2DComponent& other) const -> bool
        {
            return Type == other.Type && FixedRotation == other.FixedRotation && Math::BitwiseEqual(LinearVelocity, other.LinearVelocity) && Math::BitwiseEqual(AngularVelocity, other.AngularVelocity);
        }
    };

    struct BoxCollider2DComponent
    {
        OLO_PROPERTY()
        glm::vec2 Offset = { 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec2 Size = { 0.5f, 0.5f };

        // TODO(olbu): move into physics material in the future maybe
        OLO_PROPERTY()
        f32 Density = 1.0f;
        OLO_PROPERTY()
        f32 Friction = 0.5f;
        OLO_PROPERTY()
        f32 Restitution = 0.0f;
        f32 RestitutionThreshold = 0.5f;

        // Storage for runtime
        void* RuntimeFixture = nullptr;

        BoxCollider2DComponent() = default;
        BoxCollider2DComponent(const BoxCollider2DComponent&) = default;

        // RuntimeFixture (void*) is a Box2D handle populated on Play; excluded
        // from equality so play-mode enter/exit doesn't show as authored change.
        auto operator==(const BoxCollider2DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(Offset, other.Offset) && Math::BitwiseEqual(Size, other.Size) && Math::BitwiseEqual(Density, other.Density) && Math::BitwiseEqual(Friction, other.Friction) && Math::BitwiseEqual(Restitution, other.Restitution) && Math::BitwiseEqual(RestitutionThreshold, other.RestitutionThreshold);
        }
    };

    struct CircleCollider2DComponent
    {
        OLO_PROPERTY()
        glm::vec2 Offset = { 0.0f, 0.0f };
        OLO_PROPERTY()
        f32 Radius = 0.5f;

        // TODO(olbu): move into physics material in the future maybe
        OLO_PROPERTY()
        f32 Density = 1.0f;
        OLO_PROPERTY()
        f32 Friction = 0.5f;
        OLO_PROPERTY()
        f32 Restitution = 0.0f;
        f32 RestitutionThreshold = 0.5f;

        // Storage for runtime
        void* RuntimeFixture = nullptr;

        CircleCollider2DComponent() = default;
        CircleCollider2DComponent(const CircleCollider2DComponent&) = default;

        // RuntimeFixture (void*) is a Box2D handle populated on Play; excluded.
        auto operator==(const CircleCollider2DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(Offset, other.Offset) && Math::BitwiseEqual(Radius, other.Radius) && Math::BitwiseEqual(Density, other.Density) && Math::BitwiseEqual(Friction, other.Friction) && Math::BitwiseEqual(Restitution, other.Restitution) && Math::BitwiseEqual(RestitutionThreshold, other.RestitutionThreshold);
        }
    };

    // 3D Physics Components

    enum class BodyType3D
    {
        Static = 0,
        Dynamic,
        Kinematic
    };

    struct Rigidbody3DComponent
    {
        OLO_PROPERTY(Name = "BodyType", Type = "int", Get = "static_cast<int>(comp.m_Type)", Set = "comp.m_Type = static_cast<BodyType3D>({v})")
        BodyType3D m_Type = BodyType3D::Static;
        u32 m_LayerID = 0;
        OLO_PROPERTY()
        f32 m_Mass = 1.0f;
        OLO_PROPERTY()
        f32 m_LinearDrag = 0.01f;
        OLO_PROPERTY()
        f32 m_AngularDrag = 0.05f;
        OLO_PROPERTY()
        bool m_DisableGravity = false;
        OLO_PROPERTY()
        bool m_IsTrigger = false;
        EActorAxis m_LockedAxes = EActorAxis::None;

        glm::vec3 m_InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_InitialAngularVelocity = { 0.0f, 0.0f, 0.0f };

        f32 m_MaxLinearVelocity = 500.0f;
        f32 m_MaxAngularVelocity = 50.0f;

        // Storage for runtime - Jolt BodyID token for safe access
        u64 m_RuntimeBodyToken = 0;

        Rigidbody3DComponent() = default;
        Rigidbody3DComponent(const Rigidbody3DComponent&) = default;

        // m_RuntimeBodyToken is a Jolt body ID assigned on Play; excluded from
        // authored-state equality so play-mode enter/exit doesn't show as a change.
        auto operator==(const Rigidbody3DComponent& other) const -> bool
        {
            return m_Type == other.m_Type && m_LayerID == other.m_LayerID && Math::BitwiseEqual(m_Mass, other.m_Mass) && Math::BitwiseEqual(m_LinearDrag, other.m_LinearDrag) && Math::BitwiseEqual(m_AngularDrag, other.m_AngularDrag) && m_DisableGravity == other.m_DisableGravity && m_IsTrigger == other.m_IsTrigger && m_LockedAxes == other.m_LockedAxes && Math::BitwiseEqual(m_InitialLinearVelocity, other.m_InitialLinearVelocity) && Math::BitwiseEqual(m_InitialAngularVelocity, other.m_InitialAngularVelocity) && Math::BitwiseEqual(m_MaxLinearVelocity, other.m_MaxLinearVelocity) && Math::BitwiseEqual(m_MaxAngularVelocity, other.m_MaxAngularVelocity);
        }
    };

    struct BoxCollider3DComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        OLO_PROPERTY()
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        BoxCollider3DComponent() = default;
        BoxCollider3DComponent(const BoxCollider3DComponent&) = default;

        auto operator==(const BoxCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct SphereCollider3DComponent
    {
        OLO_PROPERTY()
        f32 m_Radius = 0.5f;
        OLO_PROPERTY()
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        SphereCollider3DComponent() = default;
        SphereCollider3DComponent(const SphereCollider3DComponent&) = default;

        auto operator==(const SphereCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct CapsuleCollider3DComponent
    {
        OLO_PROPERTY()
        f32 m_Radius = 0.5f;
        OLO_PROPERTY()
        f32 m_HalfHeight = 1.0f;
        OLO_PROPERTY()
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        CapsuleCollider3DComponent() = default;
        CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;

        auto operator==(const CapsuleCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct MeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Collision complexity setting
        bool m_UseComplexAsSimple = false; // If true, use triangle mesh for dynamic bodies

        MeshCollider3DComponent() = default;
        MeshCollider3DComponent(const MeshCollider3DComponent&) = default;
        explicit MeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}

        auto operator==(const MeshCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct ConvexMeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Convex hull settings
        f32 m_ConvexRadius = 0.05f; // Jolt convex radius for shape rounding
        u32 m_MaxVertices = 256;    // Maximum vertices in convex hull

        ConvexMeshCollider3DComponent() = default;
        ConvexMeshCollider3DComponent(const ConvexMeshCollider3DComponent&) = default;
        explicit ConvexMeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}

        auto operator==(const ConvexMeshCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct TriangleMeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Triangle mesh is always static - no additional settings needed

        TriangleMeshCollider3DComponent() = default;
        TriangleMeshCollider3DComponent(const TriangleMeshCollider3DComponent&) = default;
        explicit TriangleMeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}

        auto operator==(const TriangleMeshCollider3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct CharacterController3DComponent
    {
        OLO_PROPERTY(Name = "SlopeLimit")
        f32 m_SlopeLimitDeg = 45.0f;
        OLO_PROPERTY()
        f32 m_StepOffset = 0.4f;
        OLO_PROPERTY()
        f32 m_JumpPower = 8.0f;
        u32 m_LayerID = 0;

        OLO_PROPERTY()
        bool m_DisableGravity = false;
        bool m_ControlMovementInAir = false;
        bool m_ControlRotationInAir = false;

        CharacterController3DComponent() = default;
        CharacterController3DComponent(const CharacterController3DComponent&) = default;

        auto operator==(const CharacterController3DComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // Two-body constraint types backed by Jolt's constraint library.
    enum class JointType3D
    {
        Fixed = 0,    // Welds two bodies rigidly (all 6 DOF locked).
        Point,        // Ball-socket: keeps the two anchors coincident, rotation free.
        Distance,     // Keeps the two anchors within [MinDistance, MaxDistance].
        Hinge,        // Rotation only about Axis, with optional angle limits.
        Slider,       // Translation only along Axis, with optional limits (prismatic).
        Cone,         // Ball-socket whose twist axis is limited to a cone half-angle.
        SwingTwist,   // Ragdoll cone + twist: separate swing cone half-angles and a twist range about Axis.
        SixDOF,       // Fully configurable: each of 3 translation + 3 rotation DOF is Locked/Limited/Free.
        Pulley,       // Two bodies over two fixed world points: keeps Length1 + Ratio*Length2 within [Min,Max].
        Gear,         // Couples two bodies' rotation about their axes by a ratio (body-to-body v1; no tooth-tracking).
        RackAndPinion // Couples the connected body's rotation (pinion) to this body's translation (rack), by a ratio.
    };

    // Per-axis constraint mode for a SixDOF joint (one of the six degrees of
    // freedom). Maps to Jolt's MakeFixedAxis / SetLimitedAxis / MakeFreeAxis.
    // Serialized as int — keep the underlying values stable.
    enum class JointAxisMode
    {
        Locked = 0, // Axis fixed at 0 — no motion on this DOF (Jolt MakeFixedAxis).
        Limited,    // Axis constrained to the authored [min, max] (Jolt SetLimitedAxis).
        Free        // Axis unconstrained (Jolt MakeFreeAxis).
    };

    // Drive mode for a powered Hinge / Slider joint. Off leaves the joint free
    // (any authored friction still resists motion); Velocity drives toward a
    // target angular / linear velocity, limited by the max motor torque / force;
    // Position drives toward a target angle / offset via the motor spring.
    // Serialized as int — keep the underlying values stable.
    enum class JointMotorMode
    {
        Off = 0,  // Motor disabled; friction (if any) still resists motion.
        Velocity, // Drive to a target velocity, capped by max motor torque/force.
        Position  // Drive to a target angle/position via the motor spring.
    };

    // Connects this entity's rigidbody to another body (or the world) with a Jolt
    // two-body constraint. The constraint is created in a second pass at runtime
    // start, once every JoltBody exists (both endpoints must be present). Anchors
    // are authored in each body's local space (relative to the entity origin);
    // m_Axis is the primary axis (hinge axis / slide axis / cone twist axis) in
    // this entity's local space. A zero m_ConnectedEntity anchors the joint to the
    // world (JPH::Body::sFixedToWorld). The owning entity must have a
    // Rigidbody3DComponent to be a constraint endpoint.
    struct PhysicsJoint3DComponent
    {
        OLO_PROPERTY(Name = "JointType", Type = "int", Get = "static_cast<int>(comp.m_Type)", Set = "comp.m_Type = static_cast<JointType3D>({v})")
        JointType3D m_Type = JointType3D::Fixed;

        // The other body this joint connects to. 0 = connect to the world.
        UUID m_ConnectedEntity = 0;

        // Anchor on this body / the connected body, in their respective local spaces.
        OLO_PROPERTY()
        glm::vec3 m_LocalAnchorA = { 0.0f, 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec3 m_LocalAnchorB = { 0.0f, 0.0f, 0.0f };

        // Primary axis in this body's local space — hinge axis (Hinge), slide axis
        // (Slider), or twist axis (Cone). Unused by Fixed / Point / Distance.
        OLO_PROPERTY()
        glm::vec3 m_Axis = { 0.0f, 1.0f, 0.0f };

        // Distance joint: allowed separation range (meters). A negative value means
        // "use the initial anchor distance" (Jolt convention), giving a rigid link.
        OLO_PROPERTY()
        f32 m_MinDistance = 0.0f;
        OLO_PROPERTY()
        f32 m_MaxDistance = 1.0f;

        // Hinge joint: rotation limits about the hinge axis (degrees). Jolt clamps
        // min to [-180, 0] and max to [0, 180]; the full range disables the limit.
        OLO_PROPERTY()
        f32 m_HingeMinAngleDeg = -180.0f;
        OLO_PROPERTY()
        f32 m_HingeMaxAngleDeg = 180.0f;

        // Slider joint: translation limits along the slide axis (meters), measured
        // from the position where the two anchors coincide.
        OLO_PROPERTY()
        f32 m_SliderMinLimit = 0.0f;
        OLO_PROPERTY()
        f32 m_SliderMaxLimit = 1.0f;

        // Cone joint: half-angle of the allowed twist cone (degrees).
        OLO_PROPERTY()
        f32 m_ConeHalfAngleDeg = 45.0f;

        // Breakable joints. At runtime the per-step constraint impulse is read
        // back from Jolt, converted to an average force/torque (impulse / dt),
        // and compared against these thresholds; when either is exceeded the
        // constraint is removed and a JointBrokeEvent (Physics3D/PhysicsEvents.h)
        // is published on the Scene's GameplayEventBus. A non-positive threshold
        // (<= 0) means "disabled" — that axis never breaks the joint. Both
        // default to 0, so an authored joint is unbreakable until a designer
        // opts in. m_BreakForce is in newtons, m_BreakTorque in newton-metres.
        OLO_PROPERTY()
        f32 m_BreakForce = 0.0f;
        OLO_PROPERTY()
        f32 m_BreakTorque = 0.0f;

        // Powered joint motors + friction (issue #308 item 3). Only the Hinge and
        // Slider arms of JoltScene::CreateConstraint read these; other joint types
        // ignore them. The motor is configured once from these authored fields at
        // constraint-creation time and left running — no per-frame scripting needed
        // for v1.
        //
        // Hinge motor (acts about the hinge axis). m_HingeMotorMode selects the
        // drive: Off leaves the hinge free, Velocity drives toward
        // m_HingeMotorTargetVelocityDeg (deg/s), Position drives toward
        // m_HingeMotorTargetAngleDeg (deg, clamped to the angle limits).
        // m_HingeMaxMotorTorque (N·m, >= 0) caps the torque the motor may apply —
        // 0 leaves the motor without authority, so set it when enabling a motor.
        // m_HingeMaxFrictionTorque (N·m, >= 0) resists rotation when the motor is
        // Off (Jolt ignores friction while a motor drives the joint).
        OLO_PROPERTY(Name = "HingeMotorMode", Type = "int", Get = "static_cast<int>(comp.m_HingeMotorMode)", Set = "comp.m_HingeMotorMode = static_cast<JointMotorMode>({v})")
        JointMotorMode m_HingeMotorMode = JointMotorMode::Off;
        OLO_PROPERTY()
        f32 m_HingeMotorTargetVelocityDeg = 0.0f;
        OLO_PROPERTY()
        f32 m_HingeMotorTargetAngleDeg = 0.0f;
        OLO_PROPERTY()
        f32 m_HingeMaxMotorTorque = 0.0f;
        OLO_PROPERTY()
        f32 m_HingeMaxFrictionTorque = 0.0f;

        // Springy (soft) hinge limits (Jolt mLimitsSpringSettings). A frequency
        // > 0 turns the angle limits into a spring: exceeding a limit is allowed
        // and a restoring torque at this oscillation frequency (Hz) pulls the
        // joint back. 0 (the default, matching Jolt) keeps the limits hard.
        // Damping is the spring's damping ratio (0 = undamped/bouncy,
        // 1 = critical); ignored while the frequency is 0. The spring only acts
        // when the limits do — a full-range [-180, 180] hinge has no limit to
        // soften.
        OLO_PROPERTY()
        f32 m_HingeLimitSpringFrequency = 0.0f;
        OLO_PROPERTY()
        f32 m_HingeLimitSpringDamping = 0.0f;

        // Slider motor (acts along the slide axis) — same shape as the hinge.
        // Velocity drives toward m_SliderMotorTargetVelocity (m/s), Position toward
        // m_SliderMotorTargetPosition (m, clamped to the slide limits).
        // m_SliderMaxMotorForce (N, >= 0) caps the motor force; 0 = no authority.
        // m_SliderMaxFrictionForce (N, >= 0) resists sliding when the motor is Off.
        OLO_PROPERTY(Name = "SliderMotorMode", Type = "int", Get = "static_cast<int>(comp.m_SliderMotorMode)", Set = "comp.m_SliderMotorMode = static_cast<JointMotorMode>({v})")
        JointMotorMode m_SliderMotorMode = JointMotorMode::Off;
        OLO_PROPERTY()
        f32 m_SliderMotorTargetVelocity = 0.0f;
        OLO_PROPERTY()
        f32 m_SliderMotorTargetPosition = 0.0f;
        OLO_PROPERTY()
        f32 m_SliderMaxMotorForce = 0.0f;
        OLO_PROPERTY()
        f32 m_SliderMaxFrictionForce = 0.0f;

        // Springy (soft) slider limits — same shape as the hinge: a frequency
        // > 0 makes the translation limits soft springs, 0 keeps them hard.
        OLO_PROPERTY()
        f32 m_SliderLimitSpringFrequency = 0.0f;
        OLO_PROPERTY()
        f32 m_SliderLimitSpringDamping = 0.0f;

        // SwingTwist joint (ragdoll cone + twist). m_Axis is the twist axis; a
        // plane axis perpendicular to it is derived automatically. The swing is
        // confined by an (elliptical) cone whose two half-angles need not be
        // equal — m_SwingNormalHalfAngleDeg about the derived plane normal and
        // m_SwingPlaneHalfAngleDeg in the plane — and the twist about m_Axis is
        // limited to [m_TwistMinAngleDeg, m_TwistMaxAngleDeg]. Angles in degrees;
        // half-cone angles clamp to [0, 180], twist angles to [-180, 180].
        OLO_PROPERTY()
        f32 m_SwingNormalHalfAngleDeg = 45.0f;
        OLO_PROPERTY()
        f32 m_SwingPlaneHalfAngleDeg = 45.0f;
        OLO_PROPERTY()
        f32 m_TwistMinAngleDeg = -45.0f;
        OLO_PROPERTY()
        f32 m_TwistMaxAngleDeg = 45.0f;

        // SixDOF joint — fully configurable per-axis. The constraint frame's X
        // axis is m_Axis (normalized); the other two axes (Y, Z) are derived
        // perpendicular. Each translation and rotation DOF has a mode: Locked
        // (fixed at 0), Limited (clamped to the matching min/max below), or Free
        // (unconstrained). Default: every axis Locked → behaves as a rigid weld
        // until a designer opens a DOF.
        OLO_PROPERTY(Name = "SixDOFTransXMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFTransXMode)", Set = "comp.m_SixDOFTransXMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFTransXMode = JointAxisMode::Locked;
        OLO_PROPERTY(Name = "SixDOFTransYMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFTransYMode)", Set = "comp.m_SixDOFTransYMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFTransYMode = JointAxisMode::Locked;
        OLO_PROPERTY(Name = "SixDOFTransZMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFTransZMode)", Set = "comp.m_SixDOFTransZMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFTransZMode = JointAxisMode::Locked;
        OLO_PROPERTY(Name = "SixDOFRotXMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFRotXMode)", Set = "comp.m_SixDOFRotXMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFRotXMode = JointAxisMode::Locked;
        OLO_PROPERTY(Name = "SixDOFRotYMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFRotYMode)", Set = "comp.m_SixDOFRotYMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFRotYMode = JointAxisMode::Locked;
        OLO_PROPERTY(Name = "SixDOFRotZMode", Type = "int", Get = "static_cast<int>(comp.m_SixDOFRotZMode)", Set = "comp.m_SixDOFRotZMode = static_cast<JointAxisMode>({v})")
        JointAxisMode m_SixDOFRotZMode = JointAxisMode::Locked;

        // Per-axis limits used only by SixDOF axes in Limited mode. Translation
        // in meters (component frame), rotation in degrees. Each component's min
        // should be <= its max (an inverted range is treated as Locked by Jolt).
        OLO_PROPERTY()
        glm::vec3 m_SixDOFTranslationMin = { -0.5f, -0.5f, -0.5f };
        OLO_PROPERTY()
        glm::vec3 m_SixDOFTranslationMax = { 0.5f, 0.5f, 0.5f };
        OLO_PROPERTY()
        glm::vec3 m_SixDOFRotationMinDeg = { -45.0f, -45.0f, -45.0f };
        OLO_PROPERTY()
        glm::vec3 m_SixDOFRotationMaxDeg = { 45.0f, 45.0f, 45.0f };

        // Whether the two bodies this joint connects still collide with each
        // other (issue #308 item 1). Defaults to true, so existing scenes are
        // unchanged; a designer unticks it to let the jointed bodies overlap —
        // the usual choice for ragdoll links and tightly-coupled mechanisms.
        // Jolt has no per-constraint "collide connected" flag for two-body
        // constraints, so JoltScene::ApplyJointCollisionFilters implements it
        // with a shared collision group + GroupFilterTable that disables exactly
        // the authored no-collide pairs. Ignored for a world anchor
        // (m_ConnectedEntity == 0): there is no second body to filter against.
        OLO_PROPERTY()
        bool m_CollideConnected = true;

        // Pulley joint (issue #308 item 4). Connects two bodies over two fixed
        // world-space points, like a rope through two pulleys: the constraint
        // keeps Length1 + Ratio*Length2 within [MinLength, MaxLength], where
        // Length1 = |worldAnchorA - FixedPointA| (this body, anchored at
        // m_LocalAnchorA) and Length2 = |worldAnchorB - FixedPointB| (the
        // connected body, anchored at m_LocalAnchorB). Unlike the local-space
        // anchors above, the two fixed points are authored in WORLD space — they
        // are level-fixed hooks, not body-relative. m_PulleyRatio is the
        // block-and-tackle ratio applied to segment B. A negative min/max length
        // means "use the segment length at constraint-creation time" (Jolt's
        // auto convention); the default max of -1 makes a rope that can contract
        // but not extend.
        OLO_PROPERTY()
        glm::vec3 m_PulleyFixedPointA = { 0.0f, 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec3 m_PulleyFixedPointB = { 0.0f, 0.0f, 0.0f };
        OLO_PROPERTY()
        f32 m_PulleyRatio = 1.0f;
        OLO_PROPERTY()
        f32 m_PulleyMinLength = 0.0f;
        OLO_PROPERTY()
        f32 m_PulleyMaxLength = -1.0f;

        // Gear / RackAndPinion joints (issue #308 item 4), body-to-body v1 form.
        // Both couple two bodies that are EACH already constrained by their own
        // joint entity (a Hinge per gear; a Hinge on the pinion + a Slider on the
        // rack) — the coupling only relates their motion rates, it does not pin
        // the bodies, so the companion joints are required:
        //   - Gear: this body's rotation about m_Axis is geared to the connected
        //     body's rotation about m_ConnectedAxis with ratio m_GearRatio
        //     (Jolt convention: connectedRotation = -ratio * thisRotation).
        //   - RackAndPinion: the CONNECTED body is the pinion (rotates about
        //     m_ConnectedAxis) and THIS body is the rack (slides along m_Axis);
        //     pinionRotation = m_GearRatio * rackTranslation.
        // m_ConnectedAxis is in the connected body's local space (mirrors how
        // m_Axis is in this body's local space). A world anchor
        // (m_ConnectedEntity == 0) is rejected for these types — both need a
        // real second body. v1 limitation: Jolt can reference the companion
        // Hinge/Slider constraints to cancel numerical drift (SetConstraints);
        // the component model has no "reference two other joint entities"
        // concept, so this form omits that and may drift over long runs.
        OLO_PROPERTY()
        glm::vec3 m_ConnectedAxis = { 0.0f, 1.0f, 0.0f };
        OLO_PROPERTY()
        f32 m_GearRatio = 1.0f;

        // Storage for runtime - non-zero once the Jolt constraint has been created.
        // Excluded from authored-state equality so play-mode enter/exit doesn't show
        // as a change (mirrors Rigidbody3DComponent::m_RuntimeBodyToken). Cleared
        // back to 0 when the constraint breaks at runtime (see JoltScene).
        u64 m_RuntimeConstraintToken = 0;

        PhysicsJoint3DComponent() = default;
        PhysicsJoint3DComponent(const PhysicsJoint3DComponent&) = default;

        auto operator==(const PhysicsJoint3DComponent& other) const -> bool
        {
            return m_Type == other.m_Type && m_ConnectedEntity == other.m_ConnectedEntity && Math::BitwiseEqual(m_LocalAnchorA, other.m_LocalAnchorA) && Math::BitwiseEqual(m_LocalAnchorB, other.m_LocalAnchorB) && Math::BitwiseEqual(m_Axis, other.m_Axis) && Math::BitwiseEqual(m_MinDistance, other.m_MinDistance) && Math::BitwiseEqual(m_MaxDistance, other.m_MaxDistance) && Math::BitwiseEqual(m_HingeMinAngleDeg, other.m_HingeMinAngleDeg) && Math::BitwiseEqual(m_HingeMaxAngleDeg, other.m_HingeMaxAngleDeg) && Math::BitwiseEqual(m_SliderMinLimit, other.m_SliderMinLimit) && Math::BitwiseEqual(m_SliderMaxLimit, other.m_SliderMaxLimit) && Math::BitwiseEqual(m_ConeHalfAngleDeg, other.m_ConeHalfAngleDeg) && Math::BitwiseEqual(m_BreakForce, other.m_BreakForce) && Math::BitwiseEqual(m_BreakTorque, other.m_BreakTorque) && m_HingeMotorMode == other.m_HingeMotorMode && Math::BitwiseEqual(m_HingeMotorTargetVelocityDeg, other.m_HingeMotorTargetVelocityDeg) && Math::BitwiseEqual(m_HingeMotorTargetAngleDeg, other.m_HingeMotorTargetAngleDeg) && Math::BitwiseEqual(m_HingeMaxMotorTorque, other.m_HingeMaxMotorTorque) && Math::BitwiseEqual(m_HingeMaxFrictionTorque, other.m_HingeMaxFrictionTorque) && Math::BitwiseEqual(m_HingeLimitSpringFrequency, other.m_HingeLimitSpringFrequency) && Math::BitwiseEqual(m_HingeLimitSpringDamping, other.m_HingeLimitSpringDamping) && m_SliderMotorMode == other.m_SliderMotorMode && Math::BitwiseEqual(m_SliderMotorTargetVelocity, other.m_SliderMotorTargetVelocity) && Math::BitwiseEqual(m_SliderMotorTargetPosition, other.m_SliderMotorTargetPosition) && Math::BitwiseEqual(m_SliderMaxMotorForce, other.m_SliderMaxMotorForce) && Math::BitwiseEqual(m_SliderMaxFrictionForce, other.m_SliderMaxFrictionForce) && Math::BitwiseEqual(m_SliderLimitSpringFrequency, other.m_SliderLimitSpringFrequency) && Math::BitwiseEqual(m_SliderLimitSpringDamping, other.m_SliderLimitSpringDamping) && Math::BitwiseEqual(m_SwingNormalHalfAngleDeg, other.m_SwingNormalHalfAngleDeg) && Math::BitwiseEqual(m_SwingPlaneHalfAngleDeg, other.m_SwingPlaneHalfAngleDeg) && Math::BitwiseEqual(m_TwistMinAngleDeg, other.m_TwistMinAngleDeg) && Math::BitwiseEqual(m_TwistMaxAngleDeg, other.m_TwistMaxAngleDeg) && m_SixDOFTransXMode == other.m_SixDOFTransXMode && m_SixDOFTransYMode == other.m_SixDOFTransYMode && m_SixDOFTransZMode == other.m_SixDOFTransZMode && m_SixDOFRotXMode == other.m_SixDOFRotXMode && m_SixDOFRotYMode == other.m_SixDOFRotYMode && m_SixDOFRotZMode == other.m_SixDOFRotZMode && Math::BitwiseEqual(m_SixDOFTranslationMin, other.m_SixDOFTranslationMin) && Math::BitwiseEqual(m_SixDOFTranslationMax, other.m_SixDOFTranslationMax) && Math::BitwiseEqual(m_SixDOFRotationMinDeg, other.m_SixDOFRotationMinDeg) && Math::BitwiseEqual(m_SixDOFRotationMaxDeg, other.m_SixDOFRotationMaxDeg) && m_CollideConnected == other.m_CollideConnected && Math::BitwiseEqual(m_PulleyFixedPointA, other.m_PulleyFixedPointA) && Math::BitwiseEqual(m_PulleyFixedPointB, other.m_PulleyFixedPointB) && Math::BitwiseEqual(m_PulleyRatio, other.m_PulleyRatio) && Math::BitwiseEqual(m_PulleyMinLength, other.m_PulleyMinLength) && Math::BitwiseEqual(m_PulleyMaxLength, other.m_PulleyMaxLength) && Math::BitwiseEqual(m_ConnectedAxis, other.m_ConnectedAxis) && Math::BitwiseEqual(m_GearRatio, other.m_GearRatio);
        }
    };

    struct TextComponent
    {
        OLO_PROPERTY(Name = "Text", Type = "string")
        std::string TextString;
        Ref<Font> FontAsset = Font::GetDefault();
        OLO_PROPERTY()
        glm::vec4 Color{ 1.0f };
        OLO_PROPERTY()
        f32 Kerning = 0.0f;
        OLO_PROPERTY()
        f32 LineSpacing = 0.0f;
        f32 MaxWidth = 0.0f; // 0 = no wrapping
        bool DropShadow = false;
        f32 ShadowDistance = 0.02f;
        glm::vec4 ShadowColor{ 0.0f, 0.0f, 0.0f, 1.0f };

        auto operator==(const TextComponent&) const -> bool = default;
    };

    struct ScriptComponent
    {
        std::string ClassName;

        ScriptComponent() = default;
        ScriptComponent(const ScriptComponent&) = default;

        auto operator==(const ScriptComponent&) const -> bool = default;
    };

    struct LuaScriptComponent
    {
        std::string ScriptFile; // Relative path from project assets directory

        LuaScriptComponent() = default;
        LuaScriptComponent(const LuaScriptComponent&) = default;

        auto operator==(const LuaScriptComponent&) const -> bool = default;
    };

    struct AudioSourceComponent
    {
        OLO_PROPERTY(Name = "Volume", Type = "float", Get = "comp.Config.VolumeMultiplier", Set = "comp.Config.VolumeMultiplier = {v}; if (comp.Source) comp.Source->SetVolume({v})")
        OLO_PROPERTY(Name = "Pitch", Type = "float", Get = "comp.Config.PitchMultiplier", Set = "comp.Config.PitchMultiplier = {v}; if (comp.Source) comp.Source->SetPitch({v})")
        OLO_PROPERTY(Name = "PlayOnAwake", Type = "bool", Get = "comp.Config.PlayOnAwake", Set = "comp.Config.PlayOnAwake = {v}")
        OLO_PROPERTY(Name = "Looping", Type = "bool", Get = "comp.Config.Looping", Set = "comp.Config.Looping = {v}; if (comp.Source) comp.Source->SetLooping({v})")
        OLO_PROPERTY(Name = "Spatialization", Type = "bool", Get = "comp.Config.Spatialization", Set = "comp.Config.Spatialization = {v}; if (comp.Source) comp.Source->SetSpatialization({v})")
        OLO_PROPERTY(Name = "AttenuationModel", Type = "int", Get = "static_cast<int>(comp.Config.AttenuationModel)", Set = "comp.Config.AttenuationModel = static_cast<AttenuationModelType>({v}); if (comp.Source) comp.Source->SetAttenuationModel(comp.Config.AttenuationModel)")
        OLO_PROPERTY(Name = "RollOff", Type = "float", Get = "comp.Config.RollOff", Set = "comp.Config.RollOff = {v}; if (comp.Source) comp.Source->SetRollOff({v})")
        OLO_PROPERTY(Name = "MinGain", Type = "float", Get = "comp.Config.MinGain", Set = "comp.Config.MinGain = {v}; if (comp.Source) comp.Source->SetMinGain({v})")
        OLO_PROPERTY(Name = "MaxGain", Type = "float", Get = "comp.Config.MaxGain", Set = "comp.Config.MaxGain = {v}; if (comp.Source) comp.Source->SetMaxGain({v})")
        OLO_PROPERTY(Name = "MinDistance", Type = "float", Get = "comp.Config.MinDistance", Set = "comp.Config.MinDistance = {v}; if (comp.Source) comp.Source->SetMinDistance({v})")
        OLO_PROPERTY(Name = "MaxDistance", Type = "float", Get = "comp.Config.MaxDistance", Set = "comp.Config.MaxDistance = {v}; if (comp.Source) comp.Source->SetMaxDistance({v})")
        OLO_PROPERTY(Name = "ConeInnerAngle", Type = "float", Get = "comp.Config.ConeInnerAngle", Set = "comp.Config.ConeInnerAngle = {v}; if (comp.Source) comp.Source->SetCone(comp.Config.ConeInnerAngle, comp.Config.ConeOuterAngle, comp.Config.ConeOuterGain)")
        OLO_PROPERTY(Name = "ConeOuterAngle", Type = "float", Get = "comp.Config.ConeOuterAngle", Set = "comp.Config.ConeOuterAngle = {v}; if (comp.Source) comp.Source->SetCone(comp.Config.ConeInnerAngle, comp.Config.ConeOuterAngle, comp.Config.ConeOuterGain)")
        OLO_PROPERTY(Name = "ConeOuterGain", Type = "float", Get = "comp.Config.ConeOuterGain", Set = "comp.Config.ConeOuterGain = {v}; if (comp.Source) comp.Source->SetCone(comp.Config.ConeInnerAngle, comp.Config.ConeOuterAngle, comp.Config.ConeOuterGain)")
        OLO_PROPERTY(Name = "DopplerFactor", Type = "float", Get = "comp.Config.DopplerFactor", Set = "comp.Config.DopplerFactor = {v}; if (comp.Source) comp.Source->SetDopplerFactor({v})")
        AudioSourceConfig Config;

        // Optional SoundConfig (.olosoundc) preset. When non-zero, Scene::InitAudioRuntime
        // loads the referenced SoundConfigAsset and overwrites Config with its values at
        // playback — a reusable preset shared across entities. Zero means "use the inline
        // Config above". Exposed as ulong so scripts can swap presets at runtime.
        OLO_PROPERTY()
        AssetHandle SoundConfigHandle = 0;

        Ref<AudioSource> Source = nullptr;

        // Event-driven audio
        std::string StartEvent;          // Event name, e.g. "PlayFootsteps"
        Audio::CommandID StartCommandID; // CRC32 of StartEvent (cached)
        bool UseEventSystem = false;     // If true, uses events instead of direct play
        u64 ActiveEventID = 0;           // Runtime handle from AudioPlayback::PostTrigger

        AudioSourceComponent() = default;

        AudioSourceComponent(const AudioSourceComponent& other)
            : Config(other.Config), SoundConfigHandle(other.SoundConfigHandle), Source(other.Source), StartEvent(other.StartEvent), StartCommandID(other.StartCommandID), UseEventSystem(other.UseEventSystem)
        {
        }

        auto operator=(const AudioSourceComponent& other) -> AudioSourceComponent&
        {
            if (this != &other)
            {
                Config = other.Config;
                SoundConfigHandle = other.SoundConfigHandle;
                Source = other.Source;
                StartEvent = other.StartEvent;
                StartCommandID = other.StartCommandID;
                UseEventSystem = other.UseEventSystem;
                ActiveEventID = 0;
            }
            return *this;
        }

        // Equality for undo/redo — compares serialized/editor-visible fields only
        auto operator==(const AudioSourceComponent& other) const -> bool
        {
            return Math::BitwiseEqual(Config, other.Config) && SoundConfigHandle == other.SoundConfigHandle && StartEvent == other.StartEvent && StartCommandID == other.StartCommandID && UseEventSystem == other.UseEventSystem;
        }
    };

    struct AudioListenerComponent
    {
        bool Active = true;
        AudioListenerConfig Config;

        Ref<AudioListener> Listener;

        AudioListenerComponent() = default;
        AudioListenerComponent(const AudioListenerComponent&) = default;

        auto operator==(const AudioListenerComponent&) const -> bool = default;
    };

    // Plays a sound graph (.olosoundgraph) asset on an entity. Mirrors the AudioSourceComponent
    // shape but is driven by SoundGraph's compute-graph audio runtime instead of a raw clip.
    // Per-instance overrides (volume/pitch/loop) live inline on the component; the underlying
    // graph topology comes from the referenced SoundGraphAsset.
    struct AudioSoundGraphComponent
    {
        OLO_PROPERTY(Name = "Volume", Type = "float", Get = "comp.VolumeMultiplier", Set = "comp.VolumeMultiplier = {v}")
        OLO_PROPERTY(Name = "Pitch", Type = "float", Get = "comp.PitchMultiplier", Set = "comp.PitchMultiplier = {v}")
        OLO_PROPERTY(Name = "Looping", Type = "bool", Get = "comp.Looping", Set = "comp.Looping = {v}")
        OLO_PROPERTY(Name = "PlayOnAwake", Type = "bool", Get = "comp.PlayOnAwake", Set = "comp.PlayOnAwake = {v}")
        // Exposed as ulong so scripts can swap the graph at runtime (e.g. pick
        // a stinger per gameplay state). The bound Sound is not torn down here;
        // it rebinds on next Play().
        OLO_PROPERTY()
        AssetHandle SoundGraphHandle = 0;
        f32 VolumeMultiplier = 1.0f;
        f32 PitchMultiplier = 1.0f;
        bool Looping = false;
        bool PlayOnAwake = true;

        // Runtime-only state. Allocated by Scene::InitAudioRuntime; not serialized.
        Ref<Audio::SoundGraph::SoundGraphSound> Sound = nullptr;

        AudioSoundGraphComponent() = default;

        AudioSoundGraphComponent(const AudioSoundGraphComponent& other)
            : SoundGraphHandle(other.SoundGraphHandle),
              VolumeMultiplier(other.VolumeMultiplier),
              PitchMultiplier(other.PitchMultiplier),
              Looping(other.Looping),
              PlayOnAwake(other.PlayOnAwake),
              Sound(nullptr) // Don't share live audio state across copies.
        {
        }

        auto operator=(const AudioSoundGraphComponent& other) -> AudioSoundGraphComponent&
        {
            if (this != &other)
            {
                SoundGraphHandle = other.SoundGraphHandle;
                VolumeMultiplier = other.VolumeMultiplier;
                PitchMultiplier = other.PitchMultiplier;
                Looping = other.Looping;
                PlayOnAwake = other.PlayOnAwake;
                Sound = nullptr;
            }
            return *this;
        }

        // Equality for undo/redo — compares serialized/editor-visible fields only.
        // Float fields use Math::BitwiseEqual per cpp-coding-quality §2a.
        auto operator==(const AudioSoundGraphComponent& other) const -> bool
        {
            return static_cast<u64>(SoundGraphHandle) == static_cast<u64>(other.SoundGraphHandle) && Math::BitwiseEqual(VolumeMultiplier, other.VolumeMultiplier) && Math::BitwiseEqual(PitchMultiplier, other.PitchMultiplier) && Looping == other.Looping && PlayOnAwake == other.PlayOnAwake;
        }

        // Gameplay-facing helpers to drive graph input parameters at runtime. Returns
        // false if the component has no live Sound (e.g. graph not instantiated yet
        // because PlayOnAwake was false and Play hasn't been called, or the asset failed
        // to compile). Implementations live in AudioSoundGraphComponent.cpp to avoid
        // pulling SoundGraphSource.h / miniaudio into this header.
        bool SetParameter(const std::string& name, f32 value) const;
        bool SetParameter(const std::string& name, i32 value) const;
        bool SetParameter(const std::string& name, bool value) const;
    };

    // Plays a video file (MPEG-1 .mpg) as a fullscreen overlay — cutscenes, studio logos,
    // splash screens, credits. Decoded frames stream to a GPU texture the renderer
    // composites on top of the scene. The file is referenced by an asset-relative path;
    // the live VideoPlayer is runtime-only (allocated when playback starts, not serialized
    // and not shared across copies).
    struct VideoOverlayComponent
    {
        OLO_PROPERTY(Name = "PlayOnStart", Type = "bool", Get = "comp.PlayOnStart", Set = "comp.PlayOnStart = {v}")
        OLO_PROPERTY(Name = "SkipOnInput", Type = "bool", Get = "comp.SkipOnInput", Set = "comp.SkipOnInput = {v}")
        OLO_PROPERTY(Name = "Looping", Type = "bool", Get = "comp.Looping", Set = "comp.Looping = {v}")
        OLO_PROPERTY(Name = "Volume", Type = "float", Get = "comp.Volume", Set = "comp.Volume = {v}")
        OLO_PROPERTY(Name = "VideoPath", Type = "string", Get = "comp.VideoPath", Set = "comp.VideoPath = {v}")
        std::string VideoPath; // Asset-relative path to the video file.
        bool PlayOnStart = false;
        bool SkipOnInput = true;
        bool Looping = false;
        f32 Volume = 1.0f;

        // Runtime-only state, not serialized and reset to null on copy.
        Ref<VideoPlayer> Player = nullptr;

        VideoOverlayComponent() = default;

        VideoOverlayComponent(const VideoOverlayComponent& other)
            : VideoPath(other.VideoPath),
              PlayOnStart(other.PlayOnStart),
              SkipOnInput(other.SkipOnInput),
              Looping(other.Looping),
              Volume(other.Volume),
              Player(nullptr) // Don't share live playback state across copies.
        {
        }

        auto operator=(const VideoOverlayComponent& other) -> VideoOverlayComponent&
        {
            if (this != &other)
            {
                VideoPath = other.VideoPath;
                PlayOnStart = other.PlayOnStart;
                SkipOnInput = other.SkipOnInput;
                Looping = other.Looping;
                Volume = other.Volume;
                Player = nullptr;
            }
            return *this;
        }

        // Equality for undo/redo — compares serialized/editor-visible fields only.
        auto operator==(const VideoOverlayComponent& other) const -> bool
        {
            return VideoPath == other.VideoPath && PlayOnStart == other.PlayOnStart && SkipOnInput == other.SkipOnInput && Looping == other.Looping && Math::BitwiseEqual(Volume, other.Volume);
        }
    };

    // Plays a video on a world-space mesh surface — TV screens, monitors, billboards,
    // security-camera feeds. Each frame VideoSystem binds the decoded video texture to the
    // entity material's albedo slot. Same path/runtime split as VideoOverlayComponent.
    struct VideoSurfaceComponent
    {
        OLO_PROPERTY(Name = "AutoPlay", Type = "bool", Get = "comp.AutoPlay", Set = "comp.AutoPlay = {v}")
        OLO_PROPERTY(Name = "Looping", Type = "bool", Get = "comp.Looping", Set = "comp.Looping = {v}")
        OLO_PROPERTY(Name = "Volume", Type = "float", Get = "comp.Volume", Set = "comp.Volume = {v}")
        OLO_PROPERTY(Name = "VideoPath", Type = "string", Get = "comp.VideoPath", Set = "comp.VideoPath = {v}")
        std::string VideoPath; // Asset-relative path to the video file.
        bool AutoPlay = true;
        bool Looping = true;
        f32 Volume = 0.5f;

        // Runtime-only state, not serialized and reset to null on copy.
        Ref<VideoPlayer> Player = nullptr;

        VideoSurfaceComponent() = default;

        VideoSurfaceComponent(const VideoSurfaceComponent& other)
            : VideoPath(other.VideoPath),
              AutoPlay(other.AutoPlay),
              Looping(other.Looping),
              Volume(other.Volume),
              Player(nullptr)
        {
        }

        auto operator=(const VideoSurfaceComponent& other) -> VideoSurfaceComponent&
        {
            if (this != &other)
            {
                VideoPath = other.VideoPath;
                AutoPlay = other.AutoPlay;
                Looping = other.Looping;
                Volume = other.Volume;
                Player = nullptr;
            }
            return *this;
        }

        auto operator==(const VideoSurfaceComponent& other) const -> bool
        {
            return VideoPath == other.VideoPath && AutoPlay == other.AutoPlay && Looping == other.Looping && Math::BitwiseEqual(Volume, other.Volume);
        }
    };

    // Note: SubmeshComponent, MeshComponent, AnimationStateComponent,
    // and SkeletonComponent are now defined in OloEngine/Animation/AnimatedMeshComponents.h
    // which is already included above

    // Material component for storing PBR material data
    struct MaterialComponent
    {
        OLO_PROPERTY(Name = "AlbedoColor", Type = "vec4", Get = "comp.m_Material.GetBaseColorFactor()", Set = "comp.m_Material.SetBaseColorFactor({v})")
        OLO_PROPERTY(Name = "Metallic", Type = "float", Get = "comp.m_Material.GetMetallicFactor()", Set = "comp.m_Material.SetMetallicFactor({v})")
        OLO_PROPERTY(Name = "Roughness", Type = "float", Get = "comp.m_Material.GetRoughnessFactor()", Set = "comp.m_Material.SetRoughnessFactor({v})")
        OLO_PROPERTY(Name = "Emissive", Type = "vec4", Get = "comp.m_Material.GetEmissiveFactor()", Set = "comp.m_Material.SetEmissiveFactor({v})")
        Material m_Material;
        AssetHandle m_ShaderGraphHandle = 0;

        MaterialComponent() = default;
        MaterialComponent(const Material& material) : m_Material(material) {}
        MaterialComponent(const MaterialComponent&) = default;

        // Manual operator== — Material lacks defaulted ==, AssetHandle/UUID
        // triggers C2666. Compare the PBR factors bit-exactly via Math::BitwiseEqual
        // and the asset handle via u64. Texture references are not compared (they
        // are loaded from disk and equal if the factors round-trip).
        auto operator==(const MaterialComponent& other) const -> bool
        {
            if (!Math::BitwiseEqual(m_Material.GetBaseColorFactor(), other.m_Material.GetBaseColorFactor()))
                return false;
            if (!Math::BitwiseEqual(m_Material.GetMetallicFactor(), other.m_Material.GetMetallicFactor()))
                return false;
            if (!Math::BitwiseEqual(m_Material.GetRoughnessFactor(), other.m_Material.GetRoughnessFactor()))
                return false;
            if (!Math::BitwiseEqual(m_Material.GetEmissiveFactor(), other.m_Material.GetEmissiveFactor()))
                return false;
            return static_cast<u64>(m_ShaderGraphHandle) == static_cast<u64>(other.m_ShaderGraphHandle);
        }
    };

    // 3D Light Components

    struct DirectionalLightComponent
    {
        glm::vec3 m_Direction = { 0.0f, -1.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        bool m_CastShadows = true;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;
        f32 m_MaxShadowDistance = 200.0f;
        f32 m_CascadeSplitLambda = 0.5f;
        bool m_CascadeDebugVisualization = false;

        DirectionalLightComponent() = default;
        DirectionalLightComponent(const DirectionalLightComponent&) = default;

        auto operator==(const DirectionalLightComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct PointLightComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        f32 m_Range = 10.0f;      // Falloff range
        f32 m_Attenuation = 2.0f; // Attenuation power
        OLO_PROPERTY()
        bool m_CastShadows = false;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;

        PointLightComponent() = default;
        PointLightComponent(const PointLightComponent&) = default;

        auto operator==(const PointLightComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct SpotLightComponent
    {
        glm::vec3 m_Direction = { 0.0f, -1.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        f32 m_Range = 10.0f;
        OLO_PROPERTY()
        f32 m_InnerCutoff = 12.5f; // Inner cone angle in degrees
        OLO_PROPERTY()
        f32 m_OuterCutoff = 17.5f; // Outer cone angle in degrees
        f32 m_Attenuation = 2.0f;
        OLO_PROPERTY()
        bool m_CastShadows = false;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;

        SpotLightComponent() = default;
        SpotLightComponent(const SpotLightComponent&) = default;

        auto operator==(const SpotLightComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // Sphere area light: a light with a physical radius producing soft specular
    // highlights via the Karis 2013 representative-point technique. Diffuse uses
    // a solid-angle correction so the BRDF stays energy-conserving as radius -> 0
    // (matches a point light).
    struct SphereAreaLightComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        f32 m_Radius = 0.5f; // Physical emitter radius (world units)
        OLO_PROPERTY()
        f32 m_Range = 10.0f; // Falloff range
        OLO_PROPERTY()
        bool m_CastShadows = false; // Soft shadows from area lights are a follow-up

        SphereAreaLightComponent() = default;
        SphereAreaLightComponent(const SphereAreaLightComponent&) = default;

        auto operator==(const SphereAreaLightComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // Environment map component for skybox and IBL
    struct EnvironmentMapComponent
    {
        AssetHandle m_EnvironmentMapAsset = 0;
        std::string m_FilePath;               // Path to HDR/EXR file OR folder containing cubemap faces
        Ref<EnvironmentMap> m_EnvironmentMap; // Cached environment map (loaded from file path)

        // Cubemap mode: if true, m_FilePath is a folder with right.jpg, left.jpg, top.jpg, bottom.jpg, front.jpg, back.jpg
        // If false, m_FilePath is an HDR/EXR equirectangular file
        bool m_IsCubemapFolder = true; // Default to cubemap folder mode

        // Skybox display settings
        bool m_EnableSkybox = true;
        f32 m_Rotation = 0.0f;   // Rotation around Y axis in degrees
        f32 m_Exposure = 1.0f;   // Exposure multiplier
        f32 m_BlurAmount = 0.0f; // Blur for background (0 = sharp, 1 = fully blurred)

        // IBL settings
        bool m_EnableIBL = true;
        f32 m_IBLIntensity = 1.0f;

        // Diffuse-irradiance generator selection. False = Monte-Carlo cubemap
        // convolution (production default — IrradianceConvolution.glsl, ~1024
        // samples per output texel). True = L2 spherical-harmonics projection
        // (IrradianceFromSH.glsl, 9 coefficients, ~100x faster generation).
        // Output texture is the same RGBA32F irradiance cubemap in both cases
        // so no PBR shader cares which path produced it.
        // Editor-toggled at runtime; flipping the value drops the cached
        // EnvironmentMap and forces a regen on the next OnUpdateRuntime tick.
        bool m_UseSphericalHarmonics = false;

        // Tint/color adjustment
        glm::vec3 m_Tint = glm::vec3(1.0f);

        EnvironmentMapComponent() = default;
        EnvironmentMapComponent(const EnvironmentMapComponent&) = default;
        explicit EnvironmentMapComponent(const std::string& filepath) : m_FilePath(filepath) {}

        // Manual operator== — AssetHandle is UUID (implicit u64 → C2666);
        // Ref<EnvironmentMap> compares by pointer which is what we want for
        // undo (a different loaded cache means a real change).
        auto operator==(const EnvironmentMapComponent& other) const -> bool
        {
            if (static_cast<u64>(m_EnvironmentMapAsset) != static_cast<u64>(other.m_EnvironmentMapAsset))
                return false;
            if (m_FilePath != other.m_FilePath)
                return false;
            if (m_EnvironmentMap.Raw() != other.m_EnvironmentMap.Raw())
                return false;
            return m_IsCubemapFolder == other.m_IsCubemapFolder && m_EnableSkybox == other.m_EnableSkybox && m_EnableIBL == other.m_EnableIBL && m_UseSphericalHarmonics == other.m_UseSphericalHarmonics && Math::BitwiseEqual(m_Rotation, other.m_Rotation) && Math::BitwiseEqual(m_Exposure, other.m_Exposure) && Math::BitwiseEqual(m_BlurAmount, other.m_BlurAmount) && Math::BitwiseEqual(m_IBLIntensity, other.m_IBLIntensity) && Math::BitwiseEqual(m_Tint, other.m_Tint);
        }
    };

    // Procedural sky driven by the Preetham 1999 analytic daylight model.
    // Bakes into the same kind of cubemap + IBL texture set as
    // EnvironmentMapComponent so PBR shaders consume the result identically.
    //
    // The component holds *parameters* and a *cache*; when parameters change
    // (detected via PreethamCoefficientsUBO hash), Scene::LoadAndRenderSkybox
    // regenerates the EnvironmentMap on the next tick.
    //
    // When both this and EnvironmentMapComponent live on the scene, the
    // procedural sky wins — the file-based component stays available so users
    // can swap back without losing the asset reference.
    struct ProceduralSkyComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_SunDirection = glm::vec3(0.3f, 0.7f, 0.4f); // Toward sun, world space
        OLO_PROPERTY()
        f32 m_Turbidity = 2.5f; // [1.7, 10]; clamped before use
        OLO_PROPERTY()
        f32 m_Exposure = 0.1f; // Rate in luminance tonemap 1-exp(-Exposure*Y); higher = brighter sky
        OLO_PROPERTY()
        f32 m_SunIntensity = 1.0f; // Disk brightness multiplier
        OLO_PROPERTY()
        f32 m_SunDiskSize = 1.0f; // Multiplier on solar angular radius (~0.265 deg nominal)
        OLO_PROPERTY()
        bool m_ShowSunDisk = true;

        // If true, the runtime overwrites m_SunDirection from the first
        // DirectionalLightComponent in the scene each frame (negating the
        // light's direction since a "toward sun" vec is the opposite of a
        // light's outgoing direction).  Useful when authoring a time-of-day
        // controller that drives a single source of truth.
        OLO_PROPERTY()
        bool m_LinkSunToDirectionalLight = false;

        OLO_PROPERTY()
        bool m_EnableSkybox = true;
        OLO_PROPERTY()
        bool m_EnableIBL = true;
        OLO_PROPERTY()
        f32 m_IBLIntensity = 1.0f;

        // Bake resolution. 256 is enough for a smooth sky; bump to 512 if
        // crisper sun edges are required at the cost of bake time.
        // Intentionally NOT an OLO_PROPERTY (so not script-exposed): unlike the
        // visual params, changing it reallocates the cubemap and forces a full
        // re-bake + IBL convolution, so it's an authoring-time setting tuned in
        // the editor inspector, not something to drive per-frame from scripts.
        u32 m_CubemapResolution = 256;

        // Runtime cache (not serialised). Regenerated by
        // Scene::LoadAndRenderSkybox when m_LastBakeHash != HashParameters(...).
        Ref<EnvironmentMap> m_EnvironmentMap;
        u64 m_LastBakeHash = 0;

        ProceduralSkyComponent() = default;
        ProceduralSkyComponent(const ProceduralSkyComponent&) = default;

        auto operator==(const ProceduralSkyComponent& other) const -> bool
        {
            // EnvironmentMap is a runtime cache; comparing by pointer captures
            // "a different bake happened" without forcing a deep texture-compare.
            if (m_EnvironmentMap.Raw() != other.m_EnvironmentMap.Raw())
                return false;
            if (m_LastBakeHash != other.m_LastBakeHash)
                return false;
            return m_CubemapResolution == other.m_CubemapResolution &&
                   m_ShowSunDisk == other.m_ShowSunDisk &&
                   m_LinkSunToDirectionalLight == other.m_LinkSunToDirectionalLight &&
                   m_EnableSkybox == other.m_EnableSkybox &&
                   m_EnableIBL == other.m_EnableIBL &&
                   Math::BitwiseEqual(m_SunDirection, other.m_SunDirection) &&
                   Math::BitwiseEqual(m_Turbidity, other.m_Turbidity) &&
                   Math::BitwiseEqual(m_Exposure, other.m_Exposure) &&
                   Math::BitwiseEqual(m_SunIntensity, other.m_SunIntensity) &&
                   Math::BitwiseEqual(m_SunDiskSize, other.m_SunDiskSize) &&
                   Math::BitwiseEqual(m_IBLIntensity, other.m_IBLIntensity);
        }
    };

    // Procedural "Star Nest" nebula sky — a raymarched volumetric fractal
    // (Pablo Roman Andrioli's shadertoy, https://www.shadertoy.com/view/XlfGRj).
    // Bakes into the same cubemap + IBL texture set as ProceduralSkyComponent,
    // so PBR shaders, reflection probes and SSR all reflect the nebula
    // identically — this is the "skybox reflectiveness" feature (issue #292).
    //
    // The component holds *parameters* and a *cache*; when parameters drift
    // (detected via a StarNest parameter hash), Scene::LoadAndRenderSkybox
    // regenerates the EnvironmentMap on the next tick.
    //
    // Precedence when several sky components coexist on one scene:
    // StarNestSky > ProceduralSky > EnvironmentMap — the explicit nebula wins.
    struct StarNestSkyComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_Offset = glm::vec3(1.0f, 0.5f, 0.5f); // Camera position in the nebula field
        OLO_PROPERTY()
        f32 m_Rotation1 = 0.5f; // Whole-sky rotation, xz plane (radians)
        OLO_PROPERTY()
        f32 m_Rotation2 = 0.8f; // Whole-sky rotation, xy plane (radians)
        OLO_PROPERTY()
        f32 m_Formuparam = 0.53f; // Fold constant in p = abs(p)/dot(p,p) - formuparam
        OLO_PROPERTY()
        f32 m_StepSize = 0.1f; // March step length
        OLO_PROPERTY()
        f32 m_Tile = 0.85f; // Tiling-fold half period
        OLO_PROPERTY()
        f32 m_Brightness = 0.0015f; // Per-step coloring brightness
        OLO_PROPERTY()
        f32 m_DarkMatter = 0.3f; // Dark-matter occlusion strength
        OLO_PROPERTY()
        f32 m_DistFading = 0.73f; // Per-step distance fade
        OLO_PROPERTY()
        f32 m_Saturation = 0.85f; // 0 = greyscale, 1 = full colour
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f; // Overall output multiplier
        OLO_PROPERTY()
        i32 m_Iterations = 17; // Inner fractal iterations [1, 40]
        OLO_PROPERTY()
        i32 m_VolSteps = 20; // Volumetric march steps [1, 40]

        OLO_PROPERTY()
        bool m_EnableSkybox = true;
        OLO_PROPERTY()
        bool m_EnableIBL = true;
        OLO_PROPERTY()
        f32 m_IBLIntensity = 1.0f;

        // Bake resolution. Intentionally NOT an OLO_PROPERTY (not script-exposed):
        // changing it reallocates the cubemap and forces a full re-bake + IBL
        // convolution, so it's an authoring-time setting, not a per-frame knob.
        u32 m_CubemapResolution = 256;

        // Runtime cache (not serialised). Regenerated by
        // Scene::LoadAndRenderSkybox when m_LastBakeHash != HashParameters(...).
        Ref<EnvironmentMap> m_EnvironmentMap;
        u64 m_LastBakeHash = 0;

        StarNestSkyComponent() = default;
        StarNestSkyComponent(const StarNestSkyComponent&) = default;

        auto operator==(const StarNestSkyComponent& other) const -> bool
        {
            // Compare authored/persistent state only. m_EnvironmentMap and
            // m_LastBakeHash are a runtime bake cache mutated by
            // Scene::LoadAndRenderSkybox; including them would report the
            // component as "changed" purely because a re-bake happened,
            // producing spurious editor undo/redo churn.
            return m_CubemapResolution == other.m_CubemapResolution &&
                   m_Iterations == other.m_Iterations &&
                   m_VolSteps == other.m_VolSteps &&
                   m_EnableSkybox == other.m_EnableSkybox &&
                   m_EnableIBL == other.m_EnableIBL &&
                   Math::BitwiseEqual(m_Offset, other.m_Offset) &&
                   Math::BitwiseEqual(m_Rotation1, other.m_Rotation1) &&
                   Math::BitwiseEqual(m_Rotation2, other.m_Rotation2) &&
                   Math::BitwiseEqual(m_Formuparam, other.m_Formuparam) &&
                   Math::BitwiseEqual(m_StepSize, other.m_StepSize) &&
                   Math::BitwiseEqual(m_Tile, other.m_Tile) &&
                   Math::BitwiseEqual(m_Brightness, other.m_Brightness) &&
                   Math::BitwiseEqual(m_DarkMatter, other.m_DarkMatter) &&
                   Math::BitwiseEqual(m_DistFading, other.m_DistFading) &&
                   Math::BitwiseEqual(m_Saturation, other.m_Saturation) &&
                   Math::BitwiseEqual(m_Intensity, other.m_Intensity) &&
                   Math::BitwiseEqual(m_IBLIntensity, other.m_IBLIntensity);
        }
    };

    // Light probe component for a single standalone probe
    struct LightProbeComponent
    {
        OLO_PROPERTY()
        f32 m_InfluenceRadius = 10.0f;
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        bool m_Active = true;
        SHCoefficients m_SHCoefficients{};

        LightProbeComponent() = default;
        LightProbeComponent(const LightProbeComponent&) = default;

        auto operator==(const LightProbeComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // Light probe volume for grid-based global illumination
    struct LightProbeVolumeComponent
    {
        OLO_PROPERTY(Set = "comp.m_BoundsMin = {v}; comp.m_Dirty = true")
        glm::vec3 m_BoundsMin = glm::vec3(-10.0f);
        OLO_PROPERTY(Set = "comp.m_BoundsMax = {v}; comp.m_Dirty = true")
        glm::vec3 m_BoundsMax = glm::vec3(10.0f);
        glm::ivec3 m_Resolution = glm::ivec3(4, 2, 4);
        OLO_PROPERTY(Set = "comp.m_Spacing = {v}; comp.m_Dirty = true")
        f32 m_Spacing = 5.0f;
        OLO_PROPERTY(Set = "comp.m_Intensity = {v}; comp.m_Dirty = true")
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY(Set = "comp.m_Active = {v}; comp.m_Dirty = true")
        bool m_Active = true;
        bool m_Dirty = true;
        bool m_ShowDebugProbes = false;
        AssetHandle m_BakedDataAsset = 0;

        [[nodiscard]] i32 GetTotalProbeCount() const noexcept
        {
            return m_Resolution.x * m_Resolution.y * m_Resolution.z;
        }

        [[nodiscard]] glm::vec3 WorldToGrid(const glm::vec3& worldPos) const
        {
            constexpr f32 epsilon = 1e-6f;
            glm::vec3 const extent = glm::max(m_BoundsMax - m_BoundsMin, glm::vec3(epsilon));
            glm::vec3 const normalized = (worldPos - m_BoundsMin) / extent;
            return normalized * glm::vec3(m_Resolution - glm::ivec3(1));
        }

        [[nodiscard]] i32 GridIndex(i32 x, i32 y, i32 z) const noexcept
        {
            return z * m_Resolution.y * m_Resolution.x + y * m_Resolution.x + x;
        }

        LightProbeVolumeComponent() = default;
        LightProbeVolumeComponent(const LightProbeVolumeComponent&) = default;

        // m_Dirty is set transiently by the property setters above and cleared
        // by the bake pipeline; it is not authored data and not persisted by
        // SceneSerializer. Including it in equality would make any bake / edit
        // cycle look like an authored change to undo and scene-equality
        // consumers. Compare the persisted fields field-wise instead — floats
        // via Math::BitwiseEqual per cpp-coding-quality §2a, the asset handle
        // via u64 to dodge UUID's C2666 ambiguity.
        auto operator==(const LightProbeVolumeComponent& other) const -> bool
        {
            return Math::BitwiseEqual(m_BoundsMin, other.m_BoundsMin) && Math::BitwiseEqual(m_BoundsMax, other.m_BoundsMax) && m_Resolution == other.m_Resolution && Math::BitwiseEqual(m_Spacing, other.m_Spacing) && Math::BitwiseEqual(m_Intensity, other.m_Intensity) && m_Active == other.m_Active && m_ShowDebugProbes == other.m_ShowDebugProbes && static_cast<u64>(m_BakedDataAsset) == static_cast<u64>(other.m_BakedDataAsset);
        }
    };

    // Local reflection probe — captures the scene from a probe position into a
    // cubemap + IBL chain, overrides the global EnvironmentMapComponent IBL
    // when the camera is inside the influence sphere. Indoor surfaces get the
    // indoor reflection instead of leaking the outdoor sky.
    //
    // Bake is editor-driven via SceneHierarchyPanel; the captured EnvironmentMap
    // lives only in memory (m_BakedEnvironment is not persisted by SceneSerializer
    // or SaveGameComponentSerializer — re-bake after load).
    struct ReflectionProbeComponent
    {
        OLO_PROPERTY()
        f32 m_InfluenceRadius = 10.0f;
        OLO_PROPERTY()
        f32 m_BlendDistance = 1.0f;
        OLO_PROPERTY()
        u32 m_Resolution = 256;
        OLO_PROPERTY()
        f32 m_Intensity = 1.0f;
        OLO_PROPERTY()
        bool m_Active = true;

        // Runtime (not serialized — captured cubemap + IBL chain is regenerated by
        // editor "Bake" action; survives a scene save/load cycle only as a flag to
        // re-bake on next interactive request).
        Ref<EnvironmentMap> m_BakedEnvironment;
        bool m_NeedsBake = true;

        ReflectionProbeComponent() = default;
        ReflectionProbeComponent(const ReflectionProbeComponent&) = default;

        // m_NeedsBake is transient state (set by edits, cleared by bake) — not
        // included in equality, matching the m_Dirty pattern on
        // LightProbeVolumeComponent. Pointer-compare m_BakedEnvironment so a
        // re-bake (new EnvironmentMap instance) reads as a real change.
        auto operator==(const ReflectionProbeComponent& other) const -> bool
        {
            return Math::BitwiseEqual(m_InfluenceRadius, other.m_InfluenceRadius) && Math::BitwiseEqual(m_BlendDistance, other.m_BlendDistance) && m_Resolution == other.m_Resolution && Math::BitwiseEqual(m_Intensity, other.m_Intensity) && m_Active == other.m_Active && m_BakedEnvironment.Raw() == other.m_BakedEnvironment.Raw();
        }
    };

    // Entity relationship component for parent-child hierarchies (Hazel-style)
    struct RelationshipComponent
    {
        UUID m_ParentHandle{};
        std::vector<UUID> m_Children;

        RelationshipComponent() = default;
        RelationshipComponent(const RelationshipComponent&) = default;
        RelationshipComponent(RelationshipComponent&&) = default;
        RelationshipComponent& operator=(const RelationshipComponent&) = default;
        RelationshipComponent& operator=(RelationshipComponent&&) = default;
        explicit RelationshipComponent(UUID parent) : m_ParentHandle(parent) {}

        // Manual operator== — UUID's implicit u64 conversion makes defaulted
        // operator== ambiguous (C2666). Compare via u64 explicitly.
        auto operator==(const RelationshipComponent& other) const -> bool
        {
            if (static_cast<u64>(m_ParentHandle) != static_cast<u64>(other.m_ParentHandle))
                return false;
            if (m_Children.size() != other.m_Children.size())
                return false;
            for (sizet i = 0; i < m_Children.size(); ++i)
            {
                if (static_cast<u64>(m_Children[i]) != static_cast<u64>(other.m_Children[i]))
                    return false;
            }
            return true;
        }
    };

    // ── UI Components ────────────────────────────────────────────────────

    enum class UICanvasRenderMode : u8
    {
        ScreenSpaceOverlay = 0,
        WorldSpace
    };

    enum class UICanvasScaleMode : u8
    {
        ConstantPixelSize = 0,
        ScaleWithScreenSize
    };

    struct UICanvasComponent
    {
        UICanvasRenderMode m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
        UICanvasScaleMode m_ScaleMode = UICanvasScaleMode::ConstantPixelSize;
        OLO_PROPERTY()
        i32 m_SortOrder = 0;
        glm::vec2 m_ReferenceResolution = { 1920.0f, 1080.0f };

        UICanvasComponent() = default;
        UICanvasComponent(const UICanvasComponent&) = default;

        auto operator==(const UICanvasComponent&) const -> bool = default;
    };

    struct UIRectTransformComponent
    {
        OLO_PROPERTY()
        glm::vec2 m_AnchorMin = { 0.5f, 0.5f };
        OLO_PROPERTY()
        glm::vec2 m_AnchorMax = { 0.5f, 0.5f };
        OLO_PROPERTY()
        glm::vec2 m_AnchoredPosition = { 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec2 m_SizeDelta = { 100.0f, 100.0f };
        OLO_PROPERTY()
        glm::vec2 m_Pivot = { 0.5f, 0.5f };
        OLO_PROPERTY()
        f32 m_Rotation = 0.0f;
        OLO_PROPERTY()
        glm::vec2 m_Scale = { 1.0f, 1.0f };

        UIRectTransformComponent() = default;
        UIRectTransformComponent(const UIRectTransformComponent&) = default;

        auto operator==(const UIRectTransformComponent&) const -> bool = default;
    };

    // Transient per-frame component — resolved screen-pixel rect, NOT serialized
    struct UIResolvedRectComponent
    {
        glm::vec2 m_Position = { 0.0f, 0.0f }; // Top-left corner in pixels
        glm::vec2 m_Size = { 0.0f, 0.0f };     // Width/height in pixels

        UIResolvedRectComponent() = default;
        UIResolvedRectComponent(const UIResolvedRectComponent&) = default;

        auto operator==(const UIResolvedRectComponent&) const -> bool = default;
    };

    enum class UITextAlignment : u8
    {
        TopLeft = 0,
        TopCenter,
        TopRight,
        MiddleLeft,
        MiddleCenter,
        MiddleRight,
        BottomLeft,
        BottomCenter,
        BottomRight
    };

    enum class UIButtonState : u8
    {
        Normal = 0,
        Hovered,
        Pressed,
        Disabled
    };

    struct UIImageComponent
    {
        Ref<Texture2D> m_Texture = nullptr;
        OLO_PROPERTY()
        glm::vec4 m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
        // 9-slice border insets (left, right, top, bottom) in pixels
        glm::vec4 m_BorderInsets = { 0.0f, 0.0f, 0.0f, 0.0f };

        UIImageComponent() = default;
        UIImageComponent(const UIImageComponent&) = default;

        auto operator==(const UIImageComponent&) const -> bool = default;
    };

    struct UIPanelComponent
    {
        OLO_PROPERTY()
        glm::vec4 m_BackgroundColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        Ref<Texture2D> m_BackgroundTexture = nullptr;

        UIPanelComponent() = default;
        UIPanelComponent(const UIPanelComponent&) = default;

        auto operator==(const UIPanelComponent&) const -> bool = default;
    };

    struct UITextComponent
    {
        OLO_PROPERTY(Name = "Text", Type = "string")
        std::string m_Text;
        Ref<Font> m_FontAsset = Font::GetDefault();
        OLO_PROPERTY()
        f32 m_FontSize = 24.0f;
        OLO_PROPERTY()
        glm::vec4 m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
        UITextAlignment m_Alignment = UITextAlignment::MiddleCenter;
        OLO_PROPERTY()
        f32 m_Kerning = 0.0f;
        OLO_PROPERTY()
        f32 m_LineSpacing = 0.0f;

        UITextComponent() = default;
        UITextComponent(const UITextComponent&) = default;

        auto operator==(const UITextComponent&) const -> bool = default;
    };

    struct UIButtonComponent
    {
        OLO_PROPERTY()
        glm::vec4 m_NormalColor = { 0.3f, 0.3f, 0.3f, 1.0f };
        OLO_PROPERTY()
        glm::vec4 m_HoveredColor = { 0.4f, 0.4f, 0.4f, 1.0f };
        OLO_PROPERTY()
        glm::vec4 m_PressedColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        OLO_PROPERTY()
        glm::vec4 m_DisabledColor = { 0.15f, 0.15f, 0.15f, 0.5f };
        OLO_PROPERTY()
        bool m_Interactable = true;

        // Runtime state — not serialized
        OLO_PROPERTY(Name = "State", Type = "int", Get = "static_cast<int>(comp.m_State)", Set = "comp.m_State = static_cast<UIButtonState>({v})")
        UIButtonState m_State = UIButtonState::Normal;

        UIButtonComponent() = default;
        UIButtonComponent(const UIButtonComponent&) = default;

        auto operator==(const UIButtonComponent&) const -> bool = default;
    };

    enum class UISliderDirection : u8
    {
        LeftToRight = 0,
        RightToLeft,
        TopToBottom,
        BottomToTop
    };

    struct UISliderComponent
    {
        OLO_PROPERTY()
        f32 m_Value = 0.0f;
        OLO_PROPERTY()
        f32 m_MinValue = 0.0f;
        OLO_PROPERTY()
        f32 m_MaxValue = 1.0f;
        UISliderDirection m_Direction = UISliderDirection::LeftToRight;
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        glm::vec4 m_FillColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_HandleColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsDragging = false;

        UISliderComponent() = default;
        UISliderComponent(const UISliderComponent&) = default;

        auto operator==(const UISliderComponent&) const -> bool = default;
    };

    struct UICheckboxComponent
    {
        OLO_PROPERTY()
        bool m_IsChecked = false;
        glm::vec4 m_UncheckedColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        glm::vec4 m_CheckedColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_CheckmarkColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        bool m_Interactable = true;

        UICheckboxComponent() = default;
        UICheckboxComponent(const UICheckboxComponent&) = default;

        auto operator==(const UICheckboxComponent&) const -> bool = default;
    };

    enum class UIFillMethod : u8
    {
        Horizontal = 0,
        Vertical
    };

    struct UIProgressBarComponent
    {
        OLO_PROPERTY()
        f32 m_Value = 0.0f;
        OLO_PROPERTY()
        f32 m_MinValue = 0.0f;
        OLO_PROPERTY()
        f32 m_MaxValue = 1.0f;
        UIFillMethod m_FillMethod = UIFillMethod::Horizontal;
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        glm::vec4 m_FillColor = { 0.3f, 0.8f, 0.3f, 1.0f };

        UIProgressBarComponent() = default;
        UIProgressBarComponent(const UIProgressBarComponent&) = default;

        auto operator==(const UIProgressBarComponent&) const -> bool = default;
    };

    // Anchors a UI element's screen position to a world-space entity.
    // During layout resolution, the target entity's world position (+offset) is
    // projected to screen coordinates and used as the UI element's position.
    struct UIWorldAnchorComponent
    {
        UUID m_TargetEntity;
        OLO_PROPERTY()
        glm::vec3 m_WorldOffset = { 0.0f, 2.0f, 0.0f };

        UIWorldAnchorComponent() = default;
        UIWorldAnchorComponent(const UIWorldAnchorComponent&) = default;

        // Manual operator== — UUID's implicit u64 conversion (C2666).
        auto operator==(const UIWorldAnchorComponent& other) const -> bool
        {
            return static_cast<u64>(m_TargetEntity) == static_cast<u64>(other.m_TargetEntity) && Math::BitwiseEqual(m_WorldOffset, other.m_WorldOffset);
        }
    };

    struct UIInputFieldComponent
    {
        OLO_PROPERTY(Name = "Text", Type = "string")
        std::string m_Text;
        OLO_PROPERTY(Name = "Placeholder", Type = "string")
        std::string m_Placeholder = "Enter text...";
        Ref<Font> m_FontAsset = Font::GetDefault();
        OLO_PROPERTY()
        f32 m_FontSize = 24.0f;
        OLO_PROPERTY()
        glm::vec4 m_TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 m_PlaceholderColor = { 0.5f, 0.5f, 0.5f, 1.0f };
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        i32 m_CharacterLimit = 0; // 0 = no limit
        OLO_PROPERTY()
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsFocused = false;
        i32 m_CursorPosition = 0;

        UIInputFieldComponent() = default;
        UIInputFieldComponent(const UIInputFieldComponent&) = default;

        auto operator==(const UIInputFieldComponent&) const -> bool = default;
    };

    // --- Phase 4: Complex Widgets ---

    enum class UIScrollDirection : u8
    {
        Vertical = 0,
        Horizontal,
        Both
    };

    struct UIScrollViewComponent
    {
        OLO_PROPERTY()
        glm::vec2 m_ScrollPosition = { 0.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec2 m_ContentSize = { 0.0f, 0.0f }; // total scrollable content area
        UIScrollDirection m_ScrollDirection = UIScrollDirection::Vertical;
        OLO_PROPERTY()
        f32 m_ScrollSpeed = 20.0f;
        bool m_ShowHorizontalScrollbar = false;
        bool m_ShowVerticalScrollbar = true;
        glm::vec4 m_ScrollbarColor = { 0.4f, 0.4f, 0.4f, 0.6f };
        glm::vec4 m_ScrollbarTrackColor = { 0.15f, 0.15f, 0.15f, 0.3f };

        UIScrollViewComponent() = default;
        UIScrollViewComponent(const UIScrollViewComponent&) = default;

        auto operator==(const UIScrollViewComponent&) const -> bool = default;
    };

    struct UIDropdownOption
    {
        std::string m_Label;

        auto operator==(const UIDropdownOption&) const -> bool = default;
    };

    struct UIDropdownComponent
    {
        std::vector<UIDropdownOption> m_Options;
        OLO_PROPERTY()
        i32 m_SelectedIndex = -1;
        glm::vec4 m_BackgroundColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        glm::vec4 m_HighlightColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        Ref<Font> m_FontAsset = Font::GetDefault();
        f32 m_FontSize = 24.0f;
        f32 m_ItemHeight = 30.0f;
        OLO_PROPERTY()
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsOpen = false;
        i32 m_HoveredIndex = -1;

        UIDropdownComponent() = default;
        UIDropdownComponent(const UIDropdownComponent&) = default;

        auto operator==(const UIDropdownComponent&) const -> bool = default;
    };

    enum class UIGridLayoutStartCorner : u8
    {
        UpperLeft = 0,
        UpperRight,
        LowerLeft,
        LowerRight
    };

    enum class UIGridLayoutAxis : u8
    {
        Horizontal = 0,
        Vertical
    };

    struct UIGridLayoutComponent
    {
        OLO_PROPERTY()
        glm::vec2 m_CellSize = { 100.0f, 100.0f };
        OLO_PROPERTY()
        glm::vec2 m_Spacing = { 5.0f, 5.0f };
        glm::vec4 m_Padding = { 5.0f, 5.0f, 5.0f, 5.0f }; // left, right, top, bottom
        UIGridLayoutStartCorner m_StartCorner = UIGridLayoutStartCorner::UpperLeft;
        UIGridLayoutAxis m_StartAxis = UIGridLayoutAxis::Horizontal;
        OLO_PROPERTY()
        i32 m_ConstraintCount = 0; // 0 = flexible, >0 = fixed columns (Horizontal) or rows (Vertical)

        UIGridLayoutComponent() = default;
        UIGridLayoutComponent(const UIGridLayoutComponent&) = default;

        auto operator==(const UIGridLayoutComponent&) const -> bool = default;
    };

    struct UIToggleComponent
    {
        OLO_PROPERTY()
        bool m_IsOn = false;
        glm::vec4 m_OffColor = { 0.3f, 0.3f, 0.3f, 1.0f };
        glm::vec4 m_OnColor = { 0.3f, 0.8f, 0.3f, 1.0f };
        glm::vec4 m_KnobColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        OLO_PROPERTY()
        bool m_Interactable = true;

        UIToggleComponent() = default;
        UIToggleComponent(const UIToggleComponent&) = default;

        auto operator==(const UIToggleComponent&) const -> bool = default;
    };

    // ── Particle System ──────────────────────────────────────────────────

    struct ParticleSystemComponent
    {
        OLO_PROPERTY(Name = "Playing", Type = "bool", Get = "comp.System.Playing", Set = "comp.System.Playing = {v}")
        OLO_PROPERTY(Name = "Looping", Type = "bool", Get = "comp.System.Looping", Set = "comp.System.Looping = {v}")
        OLO_PROPERTY(Name = "EmissionRate", Type = "float", Get = "comp.System.Emitter.RateOverTime", Set = "comp.System.Emitter.RateOverTime = {v}")
        OLO_PROPERTY(Name = "WindInfluence", Type = "float", Get = "comp.System.WindInfluence", Set = "comp.System.WindInfluence = {v}")
        ParticleSystem System;
        Ref<Texture2D> Texture = nullptr;
        Ref<Mesh> ParticleMesh = nullptr; // Mesh for ParticleRenderMode::Mesh

        // Child particle systems for sub-emitters (each has independent settings)
        std::vector<ParticleSystem> ChildSystems;
        std::vector<Ref<Texture2D>> ChildTextures;

        ParticleSystemComponent() = default;
        ParticleSystemComponent(const ParticleSystemComponent&) = default;

        // Undo coverage: ParticleSystem itself does not implement operator==
        // because its emitter / curve state is too large to bit-compare
        // reliably. The editor falls through to the "no undo" tier per
        // SceneHierarchyPanel::DrawComponent<T>; tracked as a follow-up.
    };

    // ── Terrain ──────────────────────────────────────────────────────────

    struct TerrainComponent
    {
        // Serialized properties
        std::string m_HeightmapPath;
        OLO_PROPERTY()
        f32 m_WorldSizeX = 256.0f;
        OLO_PROPERTY()
        f32 m_WorldSizeZ = 256.0f;
        OLO_PROPERTY()
        f32 m_HeightScale = 64.0f;

        // Procedural generation settings (serialized, used when m_HeightmapPath is empty).
        // Exposed to C#/Lua via OLO_PROPERTY so gameplay scripts can drive procedural
        // world generation at runtime (pick a seed per run, regenerate on a level
        // transition, …). Setting any of these only takes effect once the script also
        // calls Regenerate() (see below) — the params are inputs to the next rebuild.
        OLO_PROPERTY(Name = "ProceduralEnabled")
        bool m_ProceduralEnabled = false;
        OLO_PROPERTY(Name = "Seed")
        i32 m_ProceduralSeed = 42;
        OLO_PROPERTY(Name = "Resolution")
        u32 m_ProceduralResolution = 512;
        OLO_PROPERTY(Name = "Octaves")
        u32 m_ProceduralOctaves = 6;
        OLO_PROPERTY(Name = "Frequency")
        f32 m_ProceduralFrequency = 3.0f;
        OLO_PROPERTY(Name = "Lacunarity")
        f32 m_ProceduralLacunarity = 2.0f;
        OLO_PROPERTY(Name = "Persistence")
        f32 m_ProceduralPersistence = 0.45f;

        // Advanced height-field shaping (serialized) — ridged mountains, domain
        // warp, terracing, height redistribution. Defaults are identity, so the
        // base fBm above is unchanged unless these are touched. The scalars live in
        // the nested TerrainHeightShaping struct, which OLO_PROPERTY can't bind
        // directly, so they're flattened onto the component via custom Get/Set
        // expressions that forward to m_HeightShaping.
        OLO_PROPERTY(Name = "RidgeBlend", Type = "float", Get = "comp.m_HeightShaping.RidgeBlend", Set = "comp.m_HeightShaping.RidgeBlend = {v}")
        OLO_PROPERTY(Name = "WarpStrength", Type = "float", Get = "comp.m_HeightShaping.WarpStrength", Set = "comp.m_HeightShaping.WarpStrength = {v}")
        OLO_PROPERTY(Name = "WarpFrequency", Type = "float", Get = "comp.m_HeightShaping.WarpFrequency", Set = "comp.m_HeightShaping.WarpFrequency = {v}")
        OLO_PROPERTY(Name = "TerraceSteps", Type = "uint", Get = "comp.m_HeightShaping.TerraceSteps", Set = "comp.m_HeightShaping.TerraceSteps = {v}")
        OLO_PROPERTY(Name = "TerraceSharpness", Type = "float", Get = "comp.m_HeightShaping.TerraceSharpness", Set = "comp.m_HeightShaping.TerraceSharpness = {v}")
        OLO_PROPERTY(Name = "HeightExponent", Type = "float", Get = "comp.m_HeightShaping.HeightExponent", Set = "comp.m_HeightShaping.HeightExponent = {v}")
        TerrainHeightShaping m_HeightShaping;

        // Automatic material assignment (serialized). When enabled (and a material
        // with layers + rules exist), the splatmap is generated from m_LayerRules
        // by height/slope each rebuild instead of being hand-painted.
        OLO_PROPERTY(Name = "AutoMaterial")
        bool m_AutoMaterial = false;
        std::vector<TerrainLayerRule> m_LayerRules;
        OLO_PROPERTY(Name = "SplatmapGenResolution")
        u32 m_SplatmapGenResolution = 512;

        // LOD / tessellation settings (serialized)
        bool m_TessellationEnabled = true;
        f32 m_TargetTriangleSize = 8.0f; // Screen-space pixel target
        f32 m_MorphRegion = 0.3f;        // Morph blend fraction [0,1]

        // Streaming settings (serialized)
        bool m_StreamingEnabled = false;
        std::string m_TileDirectory; // Directory containing tile files
        std::string m_TileFilePattern = "tile_%d_%d.raw";
        f32 m_TileWorldSize = 256.0f;  // World-space size per tile
        u32 m_TileResolution = 513;    // Heightmap resolution per tile
        u32 m_StreamingLoadRadius = 3; // Tile load radius around camera
        u32 m_StreamingMaxTiles = 25;  // LRU tile budget

        // Voxel override settings (serialized)
        bool m_VoxelEnabled = false;
        f32 m_VoxelSize = 1.0f;

        // Runtime state — not serialized
        Ref<TerrainData> m_TerrainData;
        Ref<TerrainChunkManager> m_ChunkManager;
        Ref<TerrainMaterial> m_Material;
        Ref<TerrainStreamer> m_Streamer;
        Ref<VoxelOverride> m_VoxelOverride;
        std::unordered_map<VoxelCoord, VoxelMesh, VoxelCoordHash> m_VoxelMeshes;
        bool m_NeedsRebuild = true;
        bool m_MaterialNeedsRebuild = true;
        bool m_AutoSplatNeedsRebuild = true; // Regenerate the auto-material splatmap on next tick

        TerrainComponent() = default;
        TerrainComponent(const TerrainComponent& other)
            : m_HeightmapPath(other.m_HeightmapPath), m_WorldSizeX(other.m_WorldSizeX), m_WorldSizeZ(other.m_WorldSizeZ), m_HeightScale(other.m_HeightScale), m_ProceduralEnabled(other.m_ProceduralEnabled), m_ProceduralSeed(other.m_ProceduralSeed), m_ProceduralResolution(other.m_ProceduralResolution), m_ProceduralOctaves(other.m_ProceduralOctaves), m_ProceduralFrequency(other.m_ProceduralFrequency), m_ProceduralLacunarity(other.m_ProceduralLacunarity), m_ProceduralPersistence(other.m_ProceduralPersistence), m_HeightShaping(other.m_HeightShaping), m_AutoMaterial(other.m_AutoMaterial), m_LayerRules(other.m_LayerRules), m_SplatmapGenResolution(other.m_SplatmapGenResolution), m_TessellationEnabled(other.m_TessellationEnabled), m_TargetTriangleSize(other.m_TargetTriangleSize), m_MorphRegion(other.m_MorphRegion), m_StreamingEnabled(other.m_StreamingEnabled), m_TileDirectory(other.m_TileDirectory), m_TileFilePattern(other.m_TileFilePattern), m_TileWorldSize(other.m_TileWorldSize), m_TileResolution(other.m_TileResolution), m_StreamingLoadRadius(other.m_StreamingLoadRadius), m_StreamingMaxTiles(other.m_StreamingMaxTiles), m_VoxelEnabled(other.m_VoxelEnabled), m_VoxelSize(other.m_VoxelSize)
        {
            // Runtime state intentionally NOT copied — force rebuild
        }
        TerrainComponent& operator=(const TerrainComponent& other)
        {
            if (this != &other)
            {
                m_HeightmapPath = other.m_HeightmapPath;
                m_WorldSizeX = other.m_WorldSizeX;
                m_WorldSizeZ = other.m_WorldSizeZ;
                m_HeightScale = other.m_HeightScale;
                m_ProceduralEnabled = other.m_ProceduralEnabled;
                m_ProceduralSeed = other.m_ProceduralSeed;
                m_ProceduralResolution = other.m_ProceduralResolution;
                m_ProceduralOctaves = other.m_ProceduralOctaves;
                m_ProceduralFrequency = other.m_ProceduralFrequency;
                m_ProceduralLacunarity = other.m_ProceduralLacunarity;
                m_ProceduralPersistence = other.m_ProceduralPersistence;
                m_HeightShaping = other.m_HeightShaping;
                m_AutoMaterial = other.m_AutoMaterial;
                m_LayerRules = other.m_LayerRules;
                m_SplatmapGenResolution = other.m_SplatmapGenResolution;
                m_TessellationEnabled = other.m_TessellationEnabled;
                m_TargetTriangleSize = other.m_TargetTriangleSize;
                m_MorphRegion = other.m_MorphRegion;
                m_StreamingEnabled = other.m_StreamingEnabled;
                m_TileDirectory = other.m_TileDirectory;
                m_TileFilePattern = other.m_TileFilePattern;
                m_TileWorldSize = other.m_TileWorldSize;
                m_TileResolution = other.m_TileResolution;
                m_StreamingLoadRadius = other.m_StreamingLoadRadius;
                m_StreamingMaxTiles = other.m_StreamingMaxTiles;
                m_VoxelEnabled = other.m_VoxelEnabled;
                m_VoxelSize = other.m_VoxelSize;
                // Runtime state reset — force rebuild
                m_TerrainData = nullptr;
                m_ChunkManager = nullptr;
                m_Material = nullptr;
                m_Streamer = nullptr;
                m_VoxelOverride = nullptr;
                m_VoxelMeshes.clear();
                m_NeedsRebuild = true;
                m_MaterialNeedsRebuild = true;
                m_AutoSplatNeedsRebuild = true;
            }
            return *this;
        }
        TerrainComponent(TerrainComponent&&) noexcept = default;
        TerrainComponent& operator=(TerrainComponent&&) noexcept = default;

        // Script/runtime regeneration trigger. OLO_PROPERTY only generates field
        // get/set, so a script that changes Seed/Octaves/… has no bound way to
        // re-run generation. This is that trigger: drop the cached runtime data so
        // the next Scene::ProcessScene3DSharedLogic tick rebuilds the height field
        // from the current procedural params (instead of reusing the previously
        // generated TerrainData), and re-derives the dependent chunks + auto-splat.
        // Mirrors the copy-assignment "force rebuild" reset. The authored material
        // asset (m_Material) is deliberately kept — only the procedural height field
        // and what derives from it are regenerated. Bound to C# (Regenerate
        // internal-call) and Lua (usertype "regenerate") so gameplay scripts can
        // drive procedural worlds at runtime. Safe to call every frame; the actual
        // work happens once, on the next render tick.
        void Regenerate()
        {
            m_TerrainData = nullptr;  // force height-field regen from current params
            m_ChunkManager = nullptr; // force chunk/quadtree rebuild from new data
            m_Streamer = nullptr;     // streaming mode: re-init the streamer
            m_NeedsRebuild = true;
            m_AutoSplatNeedsRebuild = true; // re-derive auto-material splat from new heights
        }

        // Compares the serialized fields only — runtime state is rebuild-on-load
        // so it's intentionally not considered for undo equality.
        auto operator==(const TerrainComponent& other) const -> bool
        {
            return m_HeightmapPath == other.m_HeightmapPath && Math::BitwiseEqual(m_WorldSizeX, other.m_WorldSizeX) && Math::BitwiseEqual(m_WorldSizeZ, other.m_WorldSizeZ) && Math::BitwiseEqual(m_HeightScale, other.m_HeightScale) && m_ProceduralEnabled == other.m_ProceduralEnabled && m_ProceduralSeed == other.m_ProceduralSeed && m_ProceduralResolution == other.m_ProceduralResolution && m_ProceduralOctaves == other.m_ProceduralOctaves && Math::BitwiseEqual(m_ProceduralFrequency, other.m_ProceduralFrequency) && Math::BitwiseEqual(m_ProceduralLacunarity, other.m_ProceduralLacunarity) && Math::BitwiseEqual(m_ProceduralPersistence, other.m_ProceduralPersistence) && m_HeightShaping == other.m_HeightShaping && m_AutoMaterial == other.m_AutoMaterial && m_LayerRules == other.m_LayerRules && m_SplatmapGenResolution == other.m_SplatmapGenResolution && m_TessellationEnabled == other.m_TessellationEnabled && Math::BitwiseEqual(m_TargetTriangleSize, other.m_TargetTriangleSize) && Math::BitwiseEqual(m_MorphRegion, other.m_MorphRegion) && m_StreamingEnabled == other.m_StreamingEnabled && m_TileDirectory == other.m_TileDirectory && m_TileFilePattern == other.m_TileFilePattern && Math::BitwiseEqual(m_TileWorldSize, other.m_TileWorldSize) && m_TileResolution == other.m_TileResolution && m_StreamingLoadRadius == other.m_StreamingLoadRadius && m_StreamingMaxTiles == other.m_StreamingMaxTiles && m_VoxelEnabled == other.m_VoxelEnabled && Math::BitwiseEqual(m_VoxelSize, other.m_VoxelSize);
        }
    };

    struct FoliageComponent
    {
        // Serialized
        std::vector<FoliageLayer> m_Layers;
        bool m_Enabled = true;

        // Runtime (not serialized)
        Ref<FoliageRenderer> m_Renderer;
        bool m_NeedsRebuild = true;

        FoliageComponent() = default;
        FoliageComponent(const FoliageComponent& other)
            : m_Layers(other.m_Layers), m_Enabled(other.m_Enabled)
        {
            // Runtime state intentionally NOT copied — force rebuild
        }
        FoliageComponent& operator=(const FoliageComponent& other)
        {
            if (this != &other)
            {
                m_Layers = other.m_Layers;
                m_Enabled = other.m_Enabled;
                m_Renderer = nullptr;
                m_NeedsRebuild = true;
            }
            return *this;
        }
        FoliageComponent(FoliageComponent&&) noexcept = default;
        FoliageComponent& operator=(FoliageComponent&&) noexcept = default;

        // Compares serialized state only — runtime renderer is rebuild-on-load.
        auto operator==(const FoliageComponent& other) const -> bool
        {
            return m_Layers == other.m_Layers && m_Enabled == other.m_Enabled;
        }
    };

    // ── Water Surface ────────────────────────────────────────────────────

    struct WaterComponent
    {
        // Serialized
        f32 m_WorldSizeX = 100.0f;
        f32 m_WorldSizeZ = 100.0f;
        f32 m_WaveAmplitude = 0.5f;
        f32 m_WaveFrequency = 1.0f;
        f32 m_WaveSpeed = 1.0f;
        glm::vec2 m_WaveDir0 = { 1.0f, 0.0f };
        f32 m_WaveSteepness0 = 0.5f;
        f32 m_Wavelength0 = 10.0f;
        glm::vec2 m_WaveDir1 = { 0.7f, 0.7f };
        f32 m_WaveSteepness1 = 0.3f;
        f32 m_Wavelength1 = 15.0f;
        glm::vec3 m_WaterColor = { 0.1f, 0.4f, 0.5f };
        glm::vec3 m_DeepColor = { 0.0f, 0.1f, 0.2f };
        f32 m_Transparency = 0.6f;
        f32 m_Reflectivity = 0.5f;
        f32 m_FresnelPower = 5.0f;
        f32 m_SpecularIntensity = 1.0f;
        u32 m_GridResolutionX = 128;
        u32 m_GridResolutionZ = 128;
        bool m_Enabled = true;

        // Normal map scrolling
        glm::vec2 m_NormalMapScrollDir0 = { 1.0f, 0.0f };
        glm::vec2 m_NormalMapScrollDir1 = { 0.0f, 1.0f };
        f32 m_NormalMapScrollSpeed0 = 0.02f;
        f32 m_NormalMapScrollSpeed1 = 0.015f;
        f32 m_NormalMapTiling = 1.0f;
        f32 m_NoiseIntensity = 0.3f;

        // Normal map asset handles (serialized)
        AssetHandle m_NormalMap0 = 0;
        AssetHandle m_NormalMap1 = 0;
        AssetHandle m_NoiseTexture = 0;

        // Depth-based effects (Phase 2)
        bool m_RefractionEnabled = true;
        f32 m_DepthSofteningDistance = 2.0f;
        f32 m_RefractionDistortion = 0.05f;
        f32 m_RefractionHeightFactor = 0.5f;
        glm::vec3 m_RefractionColor = { 0.0f, 0.05f, 0.1f };

        // Foam (Phase 3)
        AssetHandle m_FoamTexture = 0;
        f32 m_FoamHeightStart = 0.3f;
        f32 m_FoamFadeDistance = 0.5f;
        f32 m_FoamTiling = 2.0f;
        f32 m_FoamBrightness = 1.5f;
        f32 m_FoamAngleExponent = 2.0f;
        f32 m_ShorelineFoamPower = 3.0f;

        // Subsurface scattering approximation
        glm::vec3 m_SSSColor = { 0.0f, 0.5f, 0.4f };
        f32 m_SSSIntensity = 0.5f;

        // Screen Space Reflections (Phase 4)
        bool m_SSREnabled = true;
        f32 m_SSRMaxSteps = 64.0f;
        f32 m_SSRStepSize = 0.1f;
        f32 m_SSRMaxDistance = 50.0f;
        f32 m_SSRThickness = 0.5f;

        // Tessellation (Phase 5)
        f32 m_TessellationFactor = 8.0f;
        bool m_TessellationEnabled = false;
        f32 m_TessMinDistance = 10.0f;
        f32 m_TessMaxDistance = 200.0f;

        // Underwater rendering (Phase 6 — WATER_FUTURE_IMPROVEMENTS.md §7.2)
        // Applied as a screen-space exponential color shift when the camera
        // sits below the water plane. Density is a per-metre absorption
        // coefficient; the depth-fade math mirrors UnderwaterFog::Apply()
        // (UnderwaterFog.h) and is pinned by UnderwaterFogMathTest.
        glm::vec3 m_UnderwaterFogColor = { 0.05f, 0.15f, 0.25f };
        f32 m_UnderwaterFogDensity = 0.08f;

        // Submerged refraction distortion (§7.2, bullet 2) — a screen-space wobble
        // + chromatic split applied to the underwater image by the tone-map pass
        // when the camera is below the surface. Strength is in UV units (0 = off);
        // mirrored by UnderwaterCaustics::RefractionOffset (UnderwaterCaustics.h).
        f32 m_UnderwaterRefractionStrength = 0.006f; // UV-space wobble amplitude (0 disables)
        f32 m_UnderwaterRefractionScale = 18.0f;     // spatial frequency of the wobble
        f32 m_UnderwaterRefractionSpeed = 1.2f;      // wobble scroll speed
        f32 m_UnderwaterChromaticStrength = 0.4f;    // per-channel UV split (fraction of the wobble)

        // Caustics (§7.1) — animated light pattern projected onto submerged
        // geometry by the tone-map pass, faded by depth below the surface and by
        // upward-facing-ness / sun overhead-ness. Intensity 0 disables. Pattern +
        // depth-fade mirrored by UnderwaterCaustics (UnderwaterCaustics.h).
        f32 m_CausticsIntensity = 0.5f;                    // additive caustic brightness (0 disables)
        f32 m_CausticsScale = 0.35f;                       // world-space frequency of the caustic cells
        f32 m_CausticsSpeed = 0.6f;                        // caustic animation speed
        f32 m_CausticsMaxDepth = 25.0f;                    // metres below surface where caustics fade to zero
        glm::vec3 m_CausticsColor = { 0.7f, 0.85f, 1.0f }; // caustic light tint (slightly blue-white)

        // Volumetric light shafts / "god rays" (§3.3) — a screen-space radial
        // blur that marches from each pixel toward the sun's screen position,
        // accumulating where the path through the water toward the sun is
        // unoccluded (depth-based, so solid geometry casts the dark gaps between
        // shafts). Added on top of the submerged image by the tone-map pass so
        // shafts of sunlight stream down through the water, dappled by the surface
        // waves (sharing the caustic scale/speed). Only visible when the camera is
        // below the surface and the sun is above the horizon AND on screen (in front
        // of the camera). Intensity 0 disables. The decay normaliser mirrors
        // UnderwaterCaustics::GodRayDecaySum and the dapple UnderwaterCaustics::GodRayDapple.
        f32 m_GodRayIntensity = 0.5f;                    // master brightness of the shafts (0 disables)
        f32 m_GodRayDecay = 0.97f;                       // march weighting falloff toward the sun (<1)
        f32 m_GodRayDensity = 0.85f;                     // march length toward the sun (fraction of screen distance)
        f32 m_GodRayWeight = 1.0f;                       // shaft strength — peak in-scatter at full openness (x intensity)
        glm::vec3 m_GodRayColor = { 1.0f, 0.95f, 0.8f }; // warm sun tint of the shafts
        u32 m_GodRaySamples = 48;                        // radial-blur step count (quality/perf trade-off)
        f32 m_GodRayDappleFloor = 0.35f;                 // surface-wave shimmer trough darkness (0 = full extinction, 1 = no dapple)
        f32 m_GodRaySunFalloff = 16.0f;                  // sun-source tightness (higher = narrower shafts)

        bool m_RenderFromBelow = true; // Allow the water plane to be visible from below

        // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1 — Tessendorf spectral ocean).
        // When enabled, the surface samples a GPU displacement/normal field derived
        // from an ocean spectrum (OceanFFTField) instead of summing Gerstner waves.
        // The Gerstner path stays available (toggle off) for comparison/fallback.
        bool m_UseFFT = false;
        u32 m_FFTResolution = 128;                     ///< FFT grid size N (power of two); CPU cost ~ N² log N
        f32 m_FFTPatchSize = 80.0f;                    ///< L, world tile size in metres (the field repeats every L)
        f32 m_FFTWindSpeed = 18.0f;                    ///< V, wind speed (m/s) — sets the dominant wavelength
        glm::vec2 m_FFTWindDirection = { 1.0f, 0.0f }; ///< wind heading (normalised on use)
        f32 m_FFTAmplitude = 2.0f;                     ///< Phillips spectrum energy scale
        f32 m_FFTChoppiness = 1.2f;                    ///< horizontal-displacement (sharp crests) scale
        f32 m_FFTHeightScale = 1.0f;                   ///< artistic multiplier on the vertical displacement
        u32 m_FFTSeed = 1337;                          ///< RNG seed for the spectrum (deterministic look)
        bool m_FFTUseGpuCompute = true;                ///< generate the field with the compute butterfly FFT (§1.2); off = CPU reference path

        // Spectrum selection (WATER_FUTURE_IMPROVEMENTS.md §1.4). Phillips is the
        // classic Tessendorf spectrum; JONSWAP gives a sharper fetch-limited peak
        // (Atlantic/Pacific swell). Defaults to Phillips so existing scenes look
        // unchanged. Gamma/fetch only affect the JONSWAP path.
        Ocean::SpectrumType m_FFTSpectrumType = Ocean::SpectrumType::Phillips;
        f32 m_FFTJonswapGamma = 3.3f;      ///< γ peak-enhancement (JONSWAP only); 1 ≈ Pierson-Moskowitz
        f32 m_FFTJonswapFetch = 100000.0f; ///< fetch in metres the wind blows over (JONSWAP only) — sets the peak frequency

        // Runtime (not serialized)
        Ref<Mesh> m_WaterMesh;
        Ref<Ocean::OceanFFTField> m_OceanField; ///< lazily-created FFT cascade (runtime)
        bool m_NeedsRebuild = true;

        // Pack wave direction + steepness + wavelength into a vec4 for shader UBO.
        [[nodiscard]] glm::vec4 PackWaveDir0() const
        {
            return { m_WaveDir0.x, m_WaveDir0.y, m_WaveSteepness0, m_Wavelength0 };
        }
        [[nodiscard]] glm::vec4 PackWaveDir1() const
        {
            return { m_WaveDir1.x, m_WaveDir1.y, m_WaveSteepness1, m_Wavelength1 };
        }

        WaterComponent() = default;

        // Bitwise comparison for undo/redo change detection (SonarQube rule 2a: no float ==).
        // memcmp on contiguous float/vec blocks; regular == for non-float fields.
        // Runtime fields (m_WaterMesh, m_NeedsRebuild) intentionally excluded.
        auto operator==(WaterComponent const& o) const -> bool
        {
            // Helper: memcmp a contiguous range [&first .. &last+sizeof(last)) in *this vs o
            auto blkEq = [this, &o](auto const& selfFirst, auto const& selfLast) -> bool
            {
                auto const* a = reinterpret_cast<unsigned char const*>(&selfFirst);
                auto const len = reinterpret_cast<unsigned char const*>(&selfLast) + sizeof(selfLast) - a;
                auto const off = a - reinterpret_cast<unsigned char const*>(this);
                return std::memcmp(a, reinterpret_cast<unsigned char const*>(&o) + off,
                                   static_cast<sizet>(len)) == 0;
            };
            // clang-format off
            return blkEq(m_WorldSizeX, m_Wavelength1)           // f32*5 + vec2 + f32*2 + vec2 + f32*2
                && blkEq(m_WaterColor, m_SpecularIntensity)     // vec3*2 + f32*4
                && m_GridResolutionX == o.m_GridResolutionX
                && m_GridResolutionZ == o.m_GridResolutionZ
                && m_Enabled == o.m_Enabled
                && blkEq(m_NormalMapScrollDir0, m_NoiseIntensity) // vec2*2 + f32*4
                && m_NormalMap0 == o.m_NormalMap0
                && m_NormalMap1 == o.m_NormalMap1
                && m_NoiseTexture == o.m_NoiseTexture
                && m_RefractionEnabled == o.m_RefractionEnabled
                && blkEq(m_DepthSofteningDistance, m_RefractionColor) // f32*3 + vec3
                && m_FoamTexture == o.m_FoamTexture
                && blkEq(m_FoamHeightStart, m_SSSIntensity)    // f32*6 + vec3 + f32
                && m_SSREnabled == o.m_SSREnabled
                && blkEq(m_SSRMaxSteps, m_SSRThickness)        // f32*4
                && blkEq(m_TessellationFactor, m_TessellationFactor) // f32
                && m_TessellationEnabled == o.m_TessellationEnabled
                && blkEq(m_TessMinDistance, m_TessMaxDistance) // f32*2
                && blkEq(m_UnderwaterFogColor, m_GodRayColor) // vec3 + f32 + f32*8 + vec3 + f32*4 + vec3
                && m_GodRaySamples == o.m_GodRaySamples
                && blkEq(m_GodRayDappleFloor, m_GodRaySunFalloff) // f32*2
                && m_RenderFromBelow == o.m_RenderFromBelow
                && m_UseFFT == o.m_UseFFT
                && m_FFTResolution == o.m_FFTResolution
                && blkEq(m_FFTPatchSize, m_FFTWindSpeed)       // f32*2
                && blkEq(m_FFTWindDirection, m_FFTHeightScale) // vec2 + f32*3
                && m_FFTSeed == o.m_FFTSeed
                && m_FFTUseGpuCompute == o.m_FFTUseGpuCompute
                && m_FFTSpectrumType == o.m_FFTSpectrumType
                && blkEq(m_FFTJonswapGamma, m_FFTJonswapFetch); // f32*2
            // clang-format on
        }

        WaterComponent(const WaterComponent& other)
        {
            CopySerializedStateFrom(other);
            // Runtime state intentionally NOT copied — force rebuild
        }
        WaterComponent& operator=(const WaterComponent& other)
        {
            if (this != &other)
            {
                CopySerializedStateFrom(other);
                m_WaterMesh = nullptr;
                m_NeedsRebuild = true;
            }
            return *this;
        }
        WaterComponent(WaterComponent&&) noexcept = default;
        WaterComponent& operator=(WaterComponent&&) noexcept = default;

      private:
        void CopySerializedStateFrom(const WaterComponent& src)
        {
            m_WorldSizeX = src.m_WorldSizeX;
            m_WorldSizeZ = src.m_WorldSizeZ;
            m_WaveAmplitude = src.m_WaveAmplitude;
            m_WaveFrequency = src.m_WaveFrequency;
            m_WaveSpeed = src.m_WaveSpeed;
            m_WaveDir0 = src.m_WaveDir0;
            m_WaveSteepness0 = src.m_WaveSteepness0;
            m_Wavelength0 = src.m_Wavelength0;
            m_WaveDir1 = src.m_WaveDir1;
            m_WaveSteepness1 = src.m_WaveSteepness1;
            m_Wavelength1 = src.m_Wavelength1;
            m_WaterColor = src.m_WaterColor;
            m_DeepColor = src.m_DeepColor;
            m_Transparency = src.m_Transparency;
            m_Reflectivity = src.m_Reflectivity;
            m_FresnelPower = src.m_FresnelPower;
            m_SpecularIntensity = src.m_SpecularIntensity;
            m_GridResolutionX = src.m_GridResolutionX;
            m_GridResolutionZ = src.m_GridResolutionZ;
            m_Enabled = src.m_Enabled;
            m_NormalMapScrollDir0 = src.m_NormalMapScrollDir0;
            m_NormalMapScrollDir1 = src.m_NormalMapScrollDir1;
            m_NormalMapScrollSpeed0 = src.m_NormalMapScrollSpeed0;
            m_NormalMapScrollSpeed1 = src.m_NormalMapScrollSpeed1;
            m_NormalMapTiling = src.m_NormalMapTiling;
            m_NoiseIntensity = src.m_NoiseIntensity;
            m_NormalMap0 = src.m_NormalMap0;
            m_NormalMap1 = src.m_NormalMap1;
            m_NoiseTexture = src.m_NoiseTexture;
            m_RefractionEnabled = src.m_RefractionEnabled;
            m_DepthSofteningDistance = src.m_DepthSofteningDistance;
            m_RefractionDistortion = src.m_RefractionDistortion;
            m_RefractionHeightFactor = src.m_RefractionHeightFactor;
            m_RefractionColor = src.m_RefractionColor;
            m_FoamTexture = src.m_FoamTexture;
            m_FoamHeightStart = src.m_FoamHeightStart;
            m_FoamFadeDistance = src.m_FoamFadeDistance;
            m_FoamTiling = src.m_FoamTiling;
            m_FoamBrightness = src.m_FoamBrightness;
            m_FoamAngleExponent = src.m_FoamAngleExponent;
            m_ShorelineFoamPower = src.m_ShorelineFoamPower;
            m_SSSColor = src.m_SSSColor;
            m_SSSIntensity = src.m_SSSIntensity;
            m_SSREnabled = src.m_SSREnabled;
            m_SSRMaxSteps = src.m_SSRMaxSteps;
            m_SSRStepSize = src.m_SSRStepSize;
            m_SSRMaxDistance = src.m_SSRMaxDistance;
            m_SSRThickness = src.m_SSRThickness;
            m_TessellationFactor = src.m_TessellationFactor;
            m_TessellationEnabled = src.m_TessellationEnabled;
            m_TessMinDistance = src.m_TessMinDistance;
            m_TessMaxDistance = src.m_TessMaxDistance;
            m_UnderwaterFogColor = src.m_UnderwaterFogColor;
            m_UnderwaterFogDensity = src.m_UnderwaterFogDensity;
            m_UnderwaterRefractionStrength = src.m_UnderwaterRefractionStrength;
            m_UnderwaterRefractionScale = src.m_UnderwaterRefractionScale;
            m_UnderwaterRefractionSpeed = src.m_UnderwaterRefractionSpeed;
            m_UnderwaterChromaticStrength = src.m_UnderwaterChromaticStrength;
            m_CausticsIntensity = src.m_CausticsIntensity;
            m_CausticsScale = src.m_CausticsScale;
            m_CausticsSpeed = src.m_CausticsSpeed;
            m_CausticsMaxDepth = src.m_CausticsMaxDepth;
            m_CausticsColor = src.m_CausticsColor;
            m_GodRayIntensity = src.m_GodRayIntensity;
            m_GodRayDecay = src.m_GodRayDecay;
            m_GodRayDensity = src.m_GodRayDensity;
            m_GodRayWeight = src.m_GodRayWeight;
            m_GodRayColor = src.m_GodRayColor;
            m_GodRaySamples = src.m_GodRaySamples;
            m_GodRayDappleFloor = src.m_GodRayDappleFloor;
            m_GodRaySunFalloff = src.m_GodRaySunFalloff;
            m_RenderFromBelow = src.m_RenderFromBelow;
            m_UseFFT = src.m_UseFFT;
            m_FFTResolution = src.m_FFTResolution;
            m_FFTPatchSize = src.m_FFTPatchSize;
            m_FFTWindSpeed = src.m_FFTWindSpeed;
            m_FFTWindDirection = src.m_FFTWindDirection;
            m_FFTAmplitude = src.m_FFTAmplitude;
            m_FFTChoppiness = src.m_FFTChoppiness;
            m_FFTHeightScale = src.m_FFTHeightScale;
            m_FFTSeed = src.m_FFTSeed;
            m_FFTUseGpuCompute = src.m_FFTUseGpuCompute;
            m_FFTSpectrumType = src.m_FFTSpectrumType;
            m_FFTJonswapGamma = src.m_FFTJonswapGamma;
            m_FFTJonswapFetch = src.m_FFTJonswapFetch;
        }
    };

    // Archimedes buoyancy for a dynamic Rigidbody3D floating on a WaterComponent
    // surface. The BuoyancySystem samples the Gerstner wave field (WaterSurface,
    // the CPU mirror of Water.glsl) at eight corner probes derived from
    // m_ProbeExtents, applies an upward force per submerged probe (which yields a
    // self-righting torque because the forces act at the corners), and damps
    // bobbing/rocking with submerged linear + angular drag. See
    // docs/WATER_FUTURE_IMPROVEMENTS.md §5.1.
    //
    // Trivially copyable on purpose: the editor's DrawComponent undo path uses a
    // byte-wise memcmp for trivially-copyable components, so no operator== is
    // needed (and we avoid float == which the coding rules forbid).
    struct BuoyancyComponent
    {
        bool m_Enabled = true;

        // Local-space half-extents of the buoyancy box (the 8 corners become the
        // submersion probes). Default matches a 1 m cube; pick values close to the
        // body's collider so the displaced-volume estimate is sensible.
        glm::vec3 m_ProbeExtents = { 0.5f, 0.5f, 0.5f };

        f32 m_FluidDensity = 1000.0f;  ///< kg/m^3 (fresh water ~1000). Body floats when its mass < density * boxVolume.
        f32 m_BuoyancyScale = 1.0f;    ///< global multiplier on the Archimedes force (tuning / artistic control)
        f32 m_LinearDrag = 0.8f;       ///< submerged linear drag coefficient (damps vertical bobbing)
        f32 m_AngularDrag = 0.5f;      ///< submerged angular drag coefficient (damps rocking)
        f32 m_SubmergenceRamp = 0.25f; ///< metres over which a probe ramps dry -> fully wet (smooths the waterline)
    };

    struct SnowDeformerComponent
    {
        f32 m_DeformRadius = 0.5f;     // World-space radius of the deformation stamp
        f32 m_DeformDepth = 0.1f;      // How deep the deformer stamps into snow (meters)
        f32 m_FalloffExponent = 2.0f;  // Radial falloff curve (1=linear, 2=quadratic)
        f32 m_CompactionFactor = 0.5f; // 0=full removal, 1=compact only (no displacement)
        bool m_EmitEjecta = true;      // Emit snow puff particles on deformation

        SnowDeformerComponent() = default;
        SnowDeformerComponent(const SnowDeformerComponent&) = default;
        SnowDeformerComponent& operator=(const SnowDeformerComponent&) = default;
        SnowDeformerComponent(SnowDeformerComponent&&) noexcept = default;
        SnowDeformerComponent& operator=(SnowDeformerComponent&&) noexcept = default;

        auto operator==(const SnowDeformerComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // ── Local Fog Volume ─────────────────────────────────────────────────

    enum class FogVolumeShape : i32
    {
        Box = 0,
        Sphere = 1,
        Cylinder = 2
    };

    struct FogVolumeComponent
    {
        // Shape & spatial parameters
        FogVolumeShape m_Shape = FogVolumeShape::Box;
        glm::vec3 m_Extents = { 5.0f, 5.0f, 5.0f }; // Half-extents for Box/Cylinder, radius for Sphere uses x

        // Fog parameters
        glm::vec3 m_Color = { 0.6f, 0.65f, 0.7f };
        f32 m_Density = 0.5f;
        f32 m_FalloffDistance = 1.0f; // Boundary fade distance (world-space)
        i32 m_Priority = 0;           // Sorting priority for overlapping volumes
        f32 m_BlendWeight = 1.0f;     // 0-1 blend strength

        // Flags
        bool m_Enabled = true;
        bool m_AffectTransparent = false; // Whether to affect transparent objects

        FogVolumeComponent() = default;
        FogVolumeComponent(const FogVolumeComponent&) = default;
        FogVolumeComponent& operator=(const FogVolumeComponent&) = default;
        FogVolumeComponent(FogVolumeComponent&&) noexcept = default;
        FogVolumeComponent& operator=(FogVolumeComponent&&) noexcept = default;

        auto operator==(const FogVolumeComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // ── Deferred Decal ───────────────────────────────────────────────────

    // Which G-Buffer channel(s) a decal overrides. In Deferred mode each mode
    // selects a different shader variant + draw-buffer mask so the renderer
    // can layer projected albedo, normal, and roughness/metallic/AO decals
    // over the same surface without touching unrelated G-Buffer slots.
    enum class DecalMode : u8
    {
        Albedo = 0,
        Normal = 1,
        RMA = 2,      // Roughness / Metallic / AO
        Emissive = 3, // HDR emissive into RT2 (deferred bloom-capable)
    };

    struct DecalComponent
    {
        Ref<Texture2D> m_AlbedoTexture = nullptr;
        Ref<Texture2D> m_NormalTexture = nullptr;   // Tangent-space normal map (Normal mode)
        Ref<Texture2D> m_RMATexture = nullptr;      // R=Roughness, G=Metallic, B=AO (RMA mode)
        Ref<Texture2D> m_EmissiveTexture = nullptr; // RGB=emissive, A=mask (Emissive mode)
        glm::vec4 m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec3 m_Size = { 1.0f, 1.0f, 1.0f };
        f32 m_FadeDistance = 0.5f;
        f32 m_NormalAngleThreshold = 0.5f;
        DecalMode m_Mode = DecalMode::Albedo;
        // Force the forward (alpha-blended / WB-OIT) decal path even when
        // the active RenderingPath is Deferred. Opaque deferred decals are
        // baked into the G-Buffer pre-lighting and can't blend against the
        // lit scene; transparent decals bypass the G-Buffer overlay and
        // composite over the lit scene colour in the graph-scheduled
        // DecalRenderPass::Execute that runs after DeferredLightingPass.
        bool m_Transparent = false;

        DecalComponent() = default;
        DecalComponent(const DecalComponent&) = default;
        DecalComponent& operator=(const DecalComponent&) = default;
        DecalComponent(DecalComponent&&) noexcept = default;
        DecalComponent& operator=(DecalComponent&&) noexcept = default;

        auto operator==(const DecalComponent& other) const -> bool = default;
    };

    // ── LOD Group ────────────────────────────────────────────────────

    struct LODGroupComponent
    {
        LODGroup m_LODGroup;
        std::vector<AssetHandle> m_GeneratedLODHandles; // Transient — excluded from copy & equality
        bool m_Enabled = true;

        LODGroupComponent() = default;
        LODGroupComponent(const LODGroupComponent& other)
            : m_LODGroup(other.m_LODGroup), m_Enabled(other.m_Enabled)
        {
        }
        LODGroupComponent& operator=(const LODGroupComponent& other)
        {
            if (this != &other)
            {
                m_LODGroup = other.m_LODGroup;
                m_Enabled = other.m_Enabled;
            }
            return *this;
        }
        LODGroupComponent(LODGroupComponent&&) noexcept = default;
        LODGroupComponent& operator=(LODGroupComponent&&) noexcept = default;

        auto operator==(const LODGroupComponent& other) const -> bool
        {
            return m_LODGroup == other.m_LODGroup && m_Enabled == other.m_Enabled;
        }
    };

    // ── Tile Renderer ────────────────────────────────────────────────────

    struct TileRendererComponent
    {
        static constexpr u32 MaxGridDimension = 256;

        Ref<Mesh> TileMesh;
        u32 Width = 16;
        u32 Height = 16;
        f32 TileSize = 1.0f;
        std::vector<Material> Materials;
        std::vector<u8> MaterialIDs;

        TileRendererComponent()
        {
            MaterialIDs.resize(Width * Height, 0);
            Materials.emplace_back();
        }
        TileRendererComponent(const TileRendererComponent&) = default;

        // Resize grid preserving existing cell data at their (row, column) positions.
        void ResizeGrid(u32 newWidth, u32 newHeight)
        {
            newWidth = std::clamp(newWidth, 1u, MaxGridDimension);
            newHeight = std::clamp(newHeight, 1u, MaxGridDimension);
            if (newWidth == Width && newHeight == Height)
                return;

            std::vector<u8> newIDs(static_cast<sizet>(newWidth) * newHeight, 0);
            u32 copyW = std::min(Width, newWidth);
            u32 copyH = std::min(Height, newHeight);
            for (u32 row = 0; row < copyH; ++row)
            {
                sizet srcRowStart = static_cast<sizet>(row) * Width;
                sizet available = (srcRowStart < MaterialIDs.size()) ? (MaterialIDs.size() - srcRowStart) : 0;
                sizet bytesToCopy = std::min<sizet>(copyW, available);
                if (bytesToCopy > 0)
                    std::memcpy(&newIDs[row * newWidth], &MaterialIDs[srcRowStart], bytesToCopy);
            }

            Width = newWidth;
            Height = newHeight;
            MaterialIDs = std::move(newIDs);
        }

        // Manual operator== for undo tracking (Material lacks defaulted ==).
        // Uses bitwise comparison for floats (intentional: detect any change for undo).
        auto operator==(const TileRendererComponent& other) const -> bool
        {
            if (TileMesh != other.TileMesh || Width != other.Width ||
                Height != other.Height || MaterialIDs != other.MaterialIDs)
                return false;
            if (!Math::BitwiseEqual(TileSize, other.TileSize))
                return false;
            const auto materialCount = Materials.size();
            if (materialCount != other.Materials.size())
                return false;
            for (sizet i = 0; i < materialCount; ++i)
            {
                if (!Math::BitwiseEqual(Materials[i].GetBaseColorFactor(), other.Materials[i].GetBaseColorFactor()))
                    return false;
                if (!Math::BitwiseEqual(Materials[i].GetMetallicFactor(), other.Materials[i].GetMetallicFactor()))
                    return false;
                if (!Math::BitwiseEqual(Materials[i].GetRoughnessFactor(), other.Materials[i].GetRoughnessFactor()))
                    return false;
                if (Materials[i].GetFlags() != other.Materials[i].GetFlags())
                    return false;
            }
            return true;
        }
    };

    // ── Networking ───────────────────────────────────────────────────────

    enum class ENetworkAuthority : u8
    {
        Server = 0,
        Client,
        Shared
    };

    struct NetworkIdentityComponent
    {
        u32 OwnerClientID = 0;
        ENetworkAuthority Authority = ENetworkAuthority::Server;
        bool IsReplicated = true;

        NetworkIdentityComponent() = default;
        NetworkIdentityComponent(const NetworkIdentityComponent&) = default;
        NetworkIdentityComponent& operator=(const NetworkIdentityComponent&) = default;
        NetworkIdentityComponent(NetworkIdentityComponent&&) noexcept = default;
        NetworkIdentityComponent& operator=(NetworkIdentityComponent&&) noexcept = default;

        auto operator==(const NetworkIdentityComponent&) const -> bool = default;
    };

    struct NetworkInterestComponent
    {
        f32 RelevanceRadius = 0.0f; // 0 = always relevant (no distance culling)
        u32 InterestGroup = 0;      // 0 = default group (always included)

        NetworkInterestComponent() = default;
        NetworkInterestComponent(const NetworkInterestComponent&) = default;
        NetworkInterestComponent& operator=(const NetworkInterestComponent&) = default;
        NetworkInterestComponent(NetworkInterestComponent&&) noexcept = default;
        NetworkInterestComponent& operator=(NetworkInterestComponent&&) noexcept = default;

        auto operator==(const NetworkInterestComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    // ── MMO Networking Components ────────────────────────────────────

    // Phasing visibility filter — entities with matching PhaseID are visible.
    // PhaseID 0 means visible to all (default).
    struct PhaseComponent
    {
        u32 PhaseID = 0;

        PhaseComponent() = default;
        PhaseComponent(const PhaseComponent&) = default;
        PhaseComponent& operator=(const PhaseComponent&) = default;
        PhaseComponent(PhaseComponent&&) noexcept = default;
        PhaseComponent& operator=(PhaseComponent&&) noexcept = default;

        auto operator==(const PhaseComponent&) const -> bool = default;
    };

    // Instance portal — interaction triggers instance creation or join.
    struct InstancePortalComponent
    {
        u32 TargetZoneID = 0;
        u8 InstanceType = 0; // Maps to EInstanceType
        u32 MaxPlayers = 5;

        InstancePortalComponent() = default;
        InstancePortalComponent(const InstancePortalComponent&) = default;
        InstancePortalComponent& operator=(const InstancePortalComponent&) = default;
        InstancePortalComponent(InstancePortalComponent&&) noexcept = default;
        InstancePortalComponent& operator=(InstancePortalComponent&&) noexcept = default;

        auto operator==(const InstancePortalComponent&) const -> bool = default;
    };

    // Per-entity network update detail level.
    enum class ENetworkLOD : u8
    {
        Full = 0, // All components replicated
        Reduced,  // Position only (skip rotation/scale)
        Minimal,  // Position at reduced rate
        Dormant   // No updates sent
    };

    struct NetworkLODComponent
    {
        ENetworkLOD Level = ENetworkLOD::Full;

        NetworkLODComponent() = default;
        NetworkLODComponent(const NetworkLODComponent&) = default;
        NetworkLODComponent& operator=(const NetworkLODComponent&) = default;
        NetworkLODComponent(NetworkLODComponent&&) noexcept = default;
        NetworkLODComponent& operator=(NetworkLODComponent&&) noexcept = default;

        auto operator==(const NetworkLODComponent&) const -> bool = default;
    };

    // ----- Dialogue -----

    struct DialogueComponent
    {
        AssetHandle m_DialogueTree = 0;
        bool m_AutoTrigger = false;
        f32 m_TriggerRadius = 3.0f;
        bool m_HasTriggered = false; // runtime-only, not serialized
        bool m_TriggerOnce = true;

        DialogueComponent() = default;
        DialogueComponent(const DialogueComponent& other)
            : m_DialogueTree(other.m_DialogueTree), m_AutoTrigger(other.m_AutoTrigger), m_TriggerRadius(other.m_TriggerRadius), m_HasTriggered(false), m_TriggerOnce(other.m_TriggerOnce)
        {
        }
        DialogueComponent& operator=(const DialogueComponent& other)
        {
            if (this != &other)
            {
                m_DialogueTree = other.m_DialogueTree;
                m_AutoTrigger = other.m_AutoTrigger;
                m_TriggerRadius = other.m_TriggerRadius;
                m_HasTriggered = false;
                m_TriggerOnce = other.m_TriggerOnce;
            }
            return *this;
        }

        // Manual operator== — AssetHandle is UUID (C2666); runtime
        // m_HasTriggered intentionally excluded so undo treats authoring
        // changes as the only difference.
        auto operator==(const DialogueComponent& other) const -> bool
        {
            return static_cast<u64>(m_DialogueTree) == static_cast<u64>(other.m_DialogueTree) && m_AutoTrigger == other.m_AutoTrigger && Math::BitwiseEqual(m_TriggerRadius, other.m_TriggerRadius) && m_TriggerOnce == other.m_TriggerOnce;
        }
    };

    enum class DialogueState : u8
    {
        Inactive,
        Displaying,       // showing text, waiting for advance input
        WaitingForChoice, // showing choices, waiting for selection
        Processing        // evaluating condition/action nodes (single frame)
    };

    struct DialogueStateComponent
    {
        UUID m_CurrentNodeID = 0;
        DialogueState m_State = DialogueState::Inactive;
        std::string m_CurrentText;
        std::string m_CurrentSpeaker;
        std::vector<DialogueChoice> m_AvailableChoices;
        i32 m_SelectedChoiceIndex = -1;
        i32 m_HoveredChoiceIndex = -1;
        f32 m_TextRevealProgress = 0.0f; // 0..1 for typewriter effect
        f32 m_TextRevealSpeed = 30.0f;   // characters per second

        DialogueStateComponent() = default;
    };

    // ----- Navigation -----

    struct NavMeshBoundsComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_Min = { -100.0f, -10.0f, -100.0f };
        OLO_PROPERTY()
        glm::vec3 m_Max = { 100.0f, 50.0f, 100.0f };

        NavMeshBoundsComponent() = default;
        NavMeshBoundsComponent(const NavMeshBoundsComponent&) = default;

        auto operator==(const NavMeshBoundsComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    struct NavAgentComponent
    {
        f32 m_Radius = 0.5f;
        f32 m_Height = 2.0f;
        OLO_PROPERTY()
        f32 m_MaxSpeed = 3.5f;
        OLO_PROPERTY()
        f32 m_Acceleration = 8.0f;
        OLO_PROPERTY()
        f32 m_StoppingDistance = 0.1f;
        i32 m_AvoidancePriority = 50;
        OLO_PROPERTY()
        bool m_LockYAxis = false; // When true, navigation only moves on XZ plane

        // Runtime state (not serialized)
        OLO_PROPERTY(Name = "TargetPosition", Type = "vec3", Set = "comp.m_TargetPosition = {v}; comp.m_HasTarget = true; comp.m_HasPath = false")
        glm::vec3 m_TargetPosition = { 0.0f, 0.0f, 0.0f };
        bool m_HasTarget = false;
        bool m_HasPath = false;
        std::vector<glm::vec3> m_PathCorners;
        u32 m_CurrentCornerIndex = 0;
        i32 m_CrowdAgentId = -1;

        NavAgentComponent() = default;
        NavAgentComponent(const NavAgentComponent& other)
            : m_Radius(other.m_Radius), m_Height(other.m_Height), m_MaxSpeed(other.m_MaxSpeed),
              m_Acceleration(other.m_Acceleration), m_StoppingDistance(other.m_StoppingDistance),
              m_AvoidancePriority(other.m_AvoidancePriority), m_LockYAxis(other.m_LockYAxis)
        {
        }
        NavAgentComponent& operator=(const NavAgentComponent& other)
        {
            if (this != &other)
            {
                m_Radius = other.m_Radius;
                m_Height = other.m_Height;
                m_MaxSpeed = other.m_MaxSpeed;
                m_Acceleration = other.m_Acceleration;
                m_StoppingDistance = other.m_StoppingDistance;
                m_AvoidancePriority = other.m_AvoidancePriority;
                m_LockYAxis = other.m_LockYAxis;
                m_TargetPosition = {};
                m_HasTarget = false;
                m_HasPath = false;
                m_PathCorners.clear();
                m_CurrentCornerIndex = 0;
                m_CrowdAgentId = -1;
            }
            return *this;
        }
        NavAgentComponent(NavAgentComponent&& other) noexcept
            : m_Radius(other.m_Radius), m_Height(other.m_Height), m_MaxSpeed(other.m_MaxSpeed),
              m_Acceleration(other.m_Acceleration), m_StoppingDistance(other.m_StoppingDistance),
              m_AvoidancePriority(other.m_AvoidancePriority), m_LockYAxis(other.m_LockYAxis)
        {
        }
        NavAgentComponent& operator=(NavAgentComponent&& other) noexcept
        {
            if (this != &other)
            {
                m_Radius = other.m_Radius;
                m_Height = other.m_Height;
                m_MaxSpeed = other.m_MaxSpeed;
                m_Acceleration = other.m_Acceleration;
                m_StoppingDistance = other.m_StoppingDistance;
                m_AvoidancePriority = other.m_AvoidancePriority;
                m_LockYAxis = other.m_LockYAxis;
                m_TargetPosition = {};
                m_HasTarget = false;
                m_HasPath = false;
                m_PathCorners.clear();
                m_CurrentCornerIndex = 0;
                m_CrowdAgentId = -1;
            }
            return *this;
        }

        // Compares serialized fields only — runtime navigation state is
        // intentionally excluded (it's path-finder-managed, not authoring-visible).
        auto operator==(const NavAgentComponent& other) const -> bool
        {
            return Math::BitwiseEqual(m_Radius, other.m_Radius) && Math::BitwiseEqual(m_Height, other.m_Height) && Math::BitwiseEqual(m_MaxSpeed, other.m_MaxSpeed) && Math::BitwiseEqual(m_Acceleration, other.m_Acceleration) && Math::BitwiseEqual(m_StoppingDistance, other.m_StoppingDistance) && m_AvoidancePriority == other.m_AvoidancePriority && m_LockYAxis == other.m_LockYAxis;
        }
    };

    // Renders a floating health/mana bar above an entity.
    // Values are read automatically from AbilityComponent at render time.
    struct NameplateComponent
    {
        OLO_PROPERTY()
        bool m_Enabled = true;
        OLO_PROPERTY()
        bool m_ShowHealthBar = true;
        OLO_PROPERTY()
        bool m_ShowManaBar = false;
        OLO_PROPERTY()
        glm::vec3 m_WorldOffset = { 0.0f, 2.0f, 0.0f };
        OLO_PROPERTY()
        glm::vec2 m_BarSize = { 160.0f, 12.0f };
        OLO_PROPERTY()
        glm::vec4 m_HealthBarColor = { 0.2f, 0.8f, 0.2f, 1.0f };
        OLO_PROPERTY()
        glm::vec4 m_ManaBarColor = { 0.2f, 0.4f, 0.9f, 1.0f };
        OLO_PROPERTY()
        glm::vec4 m_BarBackgroundColor = { 0.15f, 0.15f, 0.15f, 0.85f };
        OLO_PROPERTY()
        f32 m_ManaBarGap = 2.0f; // pixels between HP and mana bar

        NameplateComponent() = default;
        NameplateComponent(const NameplateComponent&) = default;

        auto operator==(const NameplateComponent& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    template<typename... Component>
    struct ComponentGroup
    {
    };

    using AllComponents = ComponentGroup<
        TransformComponent,
        SpriteRendererComponent,
        CircleRendererComponent,
        CameraComponent,
        PrefabComponent,
        Rigidbody2DComponent,
        BoxCollider2DComponent,
        CircleCollider2DComponent,
        Rigidbody3DComponent,
        BoxCollider3DComponent,
        SphereCollider3DComponent,
        CapsuleCollider3DComponent,
        MeshCollider3DComponent,
        ConvexMeshCollider3DComponent,
        TriangleMeshCollider3DComponent,
        CharacterController3DComponent,
        PhysicsJoint3DComponent,
        TextComponent,
        ScriptComponent,
        LuaScriptComponent,
        AudioSourceComponent,
        AudioListenerComponent,
        AudioSoundGraphComponent,
        VideoOverlayComponent,
        VideoSurfaceComponent,
        SubmeshComponent,
        MeshComponent,
        ModelComponent,
        AnimationStateComponent,
        SkeletonComponent,
        MaterialComponent,
        DirectionalLightComponent,
        PointLightComponent,
        SpotLightComponent,
        SphereAreaLightComponent,
        EnvironmentMapComponent,
        ProceduralSkyComponent,
        StarNestSkyComponent,
        RelationshipComponent,
        UICanvasComponent,
        UIRectTransformComponent,
        UIImageComponent,
        UIPanelComponent,
        UITextComponent,
        UIButtonComponent,
        UISliderComponent,
        UICheckboxComponent,
        UIProgressBarComponent,
        UIWorldAnchorComponent,
        UIInputFieldComponent,
        UIScrollViewComponent,
        UIDropdownComponent,
        UIGridLayoutComponent,
        UIToggleComponent,
        LocalizedTextComponent,
        ParticleSystemComponent,
        TerrainComponent,
        FoliageComponent,
        WaterComponent,
        BuoyancyComponent,
        SnowDeformerComponent,
        FogVolumeComponent,
        DecalComponent,
        LODGroupComponent,
        LightProbeComponent,
        LightProbeVolumeComponent,
        ReflectionProbeComponent,
        StreamingVolumeComponent,
        NetworkIdentityComponent,
        NetworkInterestComponent,
        PhaseComponent,
        InstancePortalComponent,
        NetworkLODComponent,
        DialogueComponent,
        NavMeshBoundsComponent,
        NavAgentComponent,
        AnimationGraphComponent,
        MorphTargetComponent,
        BehaviorTreeComponent,
        StateMachineComponent,
        GoapAgentComponent,
        TileRendererComponent,
        InventoryComponent,
        ItemPickupComponent,
        ItemContainerComponent,
        QuestJournalComponent,
        QuestGiverComponent,
        AbilityComponent,
        NameplateComponent,
        IKTargetComponent,
        SpringBoneComponent,
        NoiseAnimationComponent,
        InstancedMeshComponent,
        CinematicComponent>;
} // namespace OloEngine
