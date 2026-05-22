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
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Terrain/TerrainData.h"
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
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
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
            f32 const len2 = glm::dot(quat, quat);
            if (len2 < 1e-12f)
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
            const auto type = Camera.GetProjectionType();
            if (type == SceneCamera::ProjectionType::Perspective)
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

        Ref<AudioSource> Source = nullptr;

        // Event-driven audio
        std::string StartEvent;          // Event name, e.g. "PlayFootsteps"
        Audio::CommandID StartCommandID; // CRC32 of StartEvent (cached)
        bool UseEventSystem = false;     // If true, uses events instead of direct play
        u64 ActiveEventID = 0;           // Runtime handle from AudioPlayback::PostTrigger

        AudioSourceComponent() = default;

        AudioSourceComponent(const AudioSourceComponent& other)
            : Config(other.Config), Source(other.Source), StartEvent(other.StartEvent), StartCommandID(other.StartCommandID), UseEventSystem(other.UseEventSystem)
        {
        }

        auto operator=(const AudioSourceComponent& other) -> AudioSourceComponent&
        {
            if (this != &other)
            {
                Config = other.Config;
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
            return Math::BitwiseEqual(Config, other.Config) && StartEvent == other.StartEvent && StartCommandID == other.StartCommandID && UseEventSystem == other.UseEventSystem;
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
        bool SetParameter(const std::string& name, f32 value);
        bool SetParameter(const std::string& name, i32 value);
        bool SetParameter(const std::string& name, bool value);
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
            return m_IsCubemapFolder == other.m_IsCubemapFolder && m_EnableSkybox == other.m_EnableSkybox && m_EnableIBL == other.m_EnableIBL && Math::BitwiseEqual(m_Rotation, other.m_Rotation) && Math::BitwiseEqual(m_Exposure, other.m_Exposure) && Math::BitwiseEqual(m_BlurAmount, other.m_BlurAmount) && Math::BitwiseEqual(m_IBLIntensity, other.m_IBLIntensity) && Math::BitwiseEqual(m_Tint, other.m_Tint);
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
        f32 m_WorldSizeX = 256.0f;
        f32 m_WorldSizeZ = 256.0f;
        f32 m_HeightScale = 64.0f;

        // Procedural generation settings (serialized, used when m_HeightmapPath is empty)
        bool m_ProceduralEnabled = false;
        i32 m_ProceduralSeed = 42;
        u32 m_ProceduralResolution = 512;
        u32 m_ProceduralOctaves = 6;
        f32 m_ProceduralFrequency = 3.0f;
        f32 m_ProceduralLacunarity = 2.0f;
        f32 m_ProceduralPersistence = 0.45f;

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

        TerrainComponent() = default;
        TerrainComponent(const TerrainComponent& other)
            : m_HeightmapPath(other.m_HeightmapPath), m_WorldSizeX(other.m_WorldSizeX), m_WorldSizeZ(other.m_WorldSizeZ), m_HeightScale(other.m_HeightScale), m_ProceduralEnabled(other.m_ProceduralEnabled), m_ProceduralSeed(other.m_ProceduralSeed), m_ProceduralResolution(other.m_ProceduralResolution), m_ProceduralOctaves(other.m_ProceduralOctaves), m_ProceduralFrequency(other.m_ProceduralFrequency), m_ProceduralLacunarity(other.m_ProceduralLacunarity), m_ProceduralPersistence(other.m_ProceduralPersistence), m_TessellationEnabled(other.m_TessellationEnabled), m_TargetTriangleSize(other.m_TargetTriangleSize), m_MorphRegion(other.m_MorphRegion), m_StreamingEnabled(other.m_StreamingEnabled), m_TileDirectory(other.m_TileDirectory), m_TileFilePattern(other.m_TileFilePattern), m_TileWorldSize(other.m_TileWorldSize), m_TileResolution(other.m_TileResolution), m_StreamingLoadRadius(other.m_StreamingLoadRadius), m_StreamingMaxTiles(other.m_StreamingMaxTiles), m_VoxelEnabled(other.m_VoxelEnabled), m_VoxelSize(other.m_VoxelSize)
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
            }
            return *this;
        }
        TerrainComponent(TerrainComponent&&) noexcept = default;
        TerrainComponent& operator=(TerrainComponent&&) noexcept = default;

        // Compares the serialized fields only — runtime state is rebuild-on-load
        // so it's intentionally not considered for undo equality.
        auto operator==(const TerrainComponent& other) const -> bool
        {
            return m_HeightmapPath == other.m_HeightmapPath && Math::BitwiseEqual(m_WorldSizeX, other.m_WorldSizeX) && Math::BitwiseEqual(m_WorldSizeZ, other.m_WorldSizeZ) && Math::BitwiseEqual(m_HeightScale, other.m_HeightScale) && m_ProceduralEnabled == other.m_ProceduralEnabled && m_ProceduralSeed == other.m_ProceduralSeed && m_ProceduralResolution == other.m_ProceduralResolution && m_ProceduralOctaves == other.m_ProceduralOctaves && Math::BitwiseEqual(m_ProceduralFrequency, other.m_ProceduralFrequency) && Math::BitwiseEqual(m_ProceduralLacunarity, other.m_ProceduralLacunarity) && Math::BitwiseEqual(m_ProceduralPersistence, other.m_ProceduralPersistence) && m_TessellationEnabled == other.m_TessellationEnabled && Math::BitwiseEqual(m_TargetTriangleSize, other.m_TargetTriangleSize) && Math::BitwiseEqual(m_MorphRegion, other.m_MorphRegion) && m_StreamingEnabled == other.m_StreamingEnabled && m_TileDirectory == other.m_TileDirectory && m_TileFilePattern == other.m_TileFilePattern && Math::BitwiseEqual(m_TileWorldSize, other.m_TileWorldSize) && m_TileResolution == other.m_TileResolution && m_StreamingLoadRadius == other.m_StreamingLoadRadius && m_StreamingMaxTiles == other.m_StreamingMaxTiles && m_VoxelEnabled == other.m_VoxelEnabled && Math::BitwiseEqual(m_VoxelSize, other.m_VoxelSize);
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

        // Runtime (not serialized)
        Ref<Mesh> m_WaterMesh;
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
                && blkEq(m_TessMinDistance, m_TessMaxDistance); // f32*2
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
        }
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
        TextComponent,
        ScriptComponent,
        LuaScriptComponent,
        AudioSourceComponent,
        AudioListenerComponent,
        AudioSoundGraphComponent,
        SubmeshComponent,
        MeshComponent,
        ModelComponent,
        AnimationStateComponent,
        SkeletonComponent,
        MaterialComponent,
        DirectionalLightComponent,
        PointLightComponent,
        SpotLightComponent,
        EnvironmentMapComponent,
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
        ParticleSystemComponent,
        TerrainComponent,
        FoliageComponent,
        WaterComponent,
        SnowDeformerComponent,
        FogVolumeComponent,
        DecalComponent,
        LODGroupComponent,
        LightProbeComponent,
        LightProbeVolumeComponent,
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
        TileRendererComponent,
        InventoryComponent,
        ItemPickupComponent,
        ItemContainerComponent,
        QuestJournalComponent,
        QuestGiverComponent,
        AbilityComponent,
        NameplateComponent,
        IKTargetComponent,
        InstancedMeshComponent>;
} // namespace OloEngine
