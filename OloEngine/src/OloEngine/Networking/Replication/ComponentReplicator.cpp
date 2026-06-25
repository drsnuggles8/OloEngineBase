#include "OloEnginePCH.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Snapshot bytes arrive from the network, which is untrusted: a NaN / ±inf
        // float poisons transform math (and, downstream, integer tick casts —
        // see SnapshotInterpolator). Replace any non-finite wire float with a safe
        // fallback. See docs/agent-rules/cpp-coding-quality.md §2.
        [[nodiscard]] f32 SanitizeWireFloat(f32 value, f32 fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] glm::vec3 SanitizeWireVec3(const glm::vec3& value, const glm::vec3& fallback)
        {
            return {
                SanitizeWireFloat(value.x, fallback.x),
                SanitizeWireFloat(value.y, fallback.y),
                SanitizeWireFloat(value.z, fallback.z),
            };
        }
    } // namespace

    void ComponentReplicator::Serialize(FArchive& ar, TransformComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        ar << component.Translation.x << component.Translation.y << component.Translation.z;
        glm::vec3 euler = component.GetRotationEuler();
        ar << euler.x << euler.y << euler.z;
        ar << component.Scale.x << component.Scale.y << component.Scale.z;
        if (ar.IsLoading())
        {
            // Validate every float pulled off the wire before it reaches the scene.
            component.Translation = SanitizeWireVec3(component.Translation, glm::vec3(0.0f));
            component.Scale = SanitizeWireVec3(component.Scale, glm::vec3(1.0f));
            component.SetRotationEuler(SanitizeWireVec3(euler, glm::vec3(0.0f)));
        }
    }

    void ComponentReplicator::Serialize(FArchive& ar, Rigidbody2DComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        i32 bodyType = std::to_underlying(component.Type);
        ar << bodyType;
        if (ar.IsLoading())
        {
            component.Type = static_cast<Rigidbody2DComponent::BodyType>(bodyType);
        }
        ar << component.FixedRotation;
    }

    void ComponentReplicator::Serialize(FArchive& ar, Rigidbody3DComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        i32 bodyType = std::to_underlying(component.m_Type);
        ar << bodyType;
        if (ar.IsLoading())
        {
            component.m_Type = static_cast<BodyType3D>(bodyType);
        }
        ar << component.m_Mass;
        ar << component.m_InitialLinearVelocity.x << component.m_InitialLinearVelocity.y << component.m_InitialLinearVelocity.z;
        ar << component.m_InitialAngularVelocity.x << component.m_InitialAngularVelocity.y << component.m_InitialAngularVelocity.z;
        if (ar.IsLoading())
        {
            // Untrusted wire floats: mass must be finite and non-negative; velocities finite.
            if (!std::isfinite(component.m_Mass) || component.m_Mass < 0.0f)
            {
                component.m_Mass = 1.0f;
            }
            component.m_InitialLinearVelocity = SanitizeWireVec3(component.m_InitialLinearVelocity, glm::vec3(0.0f));
            component.m_InitialAngularVelocity = SanitizeWireVec3(component.m_InitialAngularVelocity, glm::vec3(0.0f));
        }
    }

    std::unordered_map<std::string, ComponentSerializeFn> ComponentReplicator::s_Registry;

    void ComponentReplicator::RegisterDefaults()
    {
        OLO_PROFILE_FUNCTION();

        Register("TransformComponent",
                 [](FArchive& ar, void* comp)
                 { Serialize(ar, *static_cast<TransformComponent*>(comp)); });

        Register("Rigidbody2DComponent",
                 [](FArchive& ar, void* comp)
                 { Serialize(ar, *static_cast<Rigidbody2DComponent*>(comp)); });

        Register("Rigidbody3DComponent",
                 [](FArchive& ar, void* comp)
                 { Serialize(ar, *static_cast<Rigidbody3DComponent*>(comp)); });

        OLO_CORE_TRACE("[ComponentReplicator] Registered {} default component serializers", s_Registry.size());
    }

    void ComponentReplicator::Register(const std::string& componentName, ComponentSerializeFn serializer)
    {
        s_Registry[componentName] = std::move(serializer);
    }

    bool ComponentReplicator::IsRegistered(const std::string& componentName)
    {
        return s_Registry.contains(componentName);
    }

    const std::unordered_map<std::string, ComponentSerializeFn>& ComponentReplicator::GetRegistry()
    {
        return s_Registry;
    }

    const ComponentSerializeFn* ComponentReplicator::GetSerializer(const std::string& componentName)
    {
        auto it = s_Registry.find(componentName);
        return it != s_Registry.end() ? &it->second : nullptr;
    }

    void ComponentReplicator::ClearRegistry()
    {
        s_Registry.clear();
    }
} // namespace OloEngine
