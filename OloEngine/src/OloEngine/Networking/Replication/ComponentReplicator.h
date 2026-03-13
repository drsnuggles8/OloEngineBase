#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class FArchive;
    struct TransformComponent;
    struct Rigidbody2DComponent;
    struct Rigidbody3DComponent;

    class ComponentReplicator
    {
    public:
        static void Serialize(FArchive& ar, TransformComponent& component);
        static void Serialize(FArchive& ar, Rigidbody2DComponent& component);
        static void Serialize(FArchive& ar, Rigidbody3DComponent& component);

        static void RegisterDefaults();
    };
}
