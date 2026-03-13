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

    void ComponentReplicator::RegisterDefaults()
    {
        OLO_PROFILE_FUNCTION();
        // Currently using static overloads. Future: register via function map for extensibility.
        OLO_CORE_TRACE("[ComponentReplicator] RegisterDefaults: using static Serialize overloads "
                       "(Transform, Rigidbody2D, Rigidbody3D)");
    }
} // namespace OloEngine
