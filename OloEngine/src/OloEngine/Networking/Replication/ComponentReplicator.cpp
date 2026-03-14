#include "OloEnginePCH.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

namespace OloEngine
{
    void ComponentReplicator::Serialize(FArchive& ar, TransformComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        ar << component.Translation.x << component.Translation.y << component.Translation.z;
        ar << component.Rotation.x << component.Rotation.y << component.Rotation.z;
        ar << component.Scale.x << component.Scale.y << component.Scale.z;
    }

    void ComponentReplicator::Serialize(FArchive& ar, Rigidbody2DComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        i32 bodyType = static_cast<i32>(component.Type);
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

        i32 bodyType = static_cast<i32>(component.m_Type);
        ar << bodyType;
        if (ar.IsLoading())
        {
            component.m_Type = static_cast<BodyType3D>(bodyType);
        }
        ar << component.m_Mass;
        ar << component.m_InitialLinearVelocity.x << component.m_InitialLinearVelocity.y << component.m_InitialLinearVelocity.z;
        ar << component.m_InitialAngularVelocity.x << component.m_InitialAngularVelocity.y << component.m_InitialAngularVelocity.z;
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
