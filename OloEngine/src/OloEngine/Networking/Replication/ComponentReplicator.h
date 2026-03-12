#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

namespace OloEngine
{
    // @class ComponentReplicator
    // @brief Provides FArchive-based serialization helpers for ECS components
    //        that participate in network replication.
    //
    // All serializers set ArIsNetArchive = true so that component serialize
    // implementations can branch on network context (e.g. skip editor-only data).
    //
    // Initial implementation uses full-state serialization.
    // Delta/dirty-flag tracking is deferred to a future optimization pass.
    class ComponentReplicator
    {
      public:
        // @brief Serialize a TransformComponent into the archive.
        static void Serialize(FArchive& ar, TransformComponent& component);

        // @brief Serialize a Rigidbody2DComponent into the archive.
        static void Serialize(FArchive& ar, Rigidbody2DComponent& component);

        // @brief Serialize a Rigidbody3DComponent into the archive.
        static void Serialize(FArchive& ar, Rigidbody3DComponent& component);
    };

} // namespace OloEngine
