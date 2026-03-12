#include "OloEnginePCH.h"
#include "ComponentReplicator.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    void ComponentReplicator::Serialize(FArchive& ar, TransformComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        ar.ArIsNetArchive = true;

        // Translation
        ar << component.Translation.x;
        ar << component.Translation.y;
        ar << component.Translation.z;

        // Rotation (Euler angles, radians)
        ar << component.Rotation.x;
        ar << component.Rotation.y;
        ar << component.Rotation.z;

        // Scale
        ar << component.Scale.x;
        ar << component.Scale.y;
        ar << component.Scale.z;
    }

    void ComponentReplicator::Serialize(FArchive& ar, Rigidbody2DComponent& component)
    {
        OLO_PROFILE_FUNCTION();

        ar.ArIsNetArchive = true;

        auto bodyType = static_cast<u8>(component.Type);
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

        ar.ArIsNetArchive = true;

        auto bodyType = static_cast<u8>(component.m_Type);
        ar << bodyType;
        if (ar.IsLoading())
        {
            component.m_Type = static_cast<Rigidbody3DComponent::BodyType3D>(bodyType);
        }

        ar << component.m_Mass;
        ar << component.m_LinearDrag;
        ar << component.m_AngularDrag;
        ar << component.m_DisableGravity;
    }

} // namespace OloEngine
