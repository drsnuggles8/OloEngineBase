#include "OloEnginePCH.h"
#include "EntitySnapshot.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

namespace OloEngine
{
    std::vector<u8> EntitySnapshot::Capture(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };
            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            if (!nic.IsReplicated)
            {
                continue;
            }

            u64 uuid = static_cast<u64>(entity.GetUUID());
            writer << uuid;

            auto& transform = entity.GetComponent<TransformComponent>();
            ComponentReplicator::Serialize(writer, transform);
        }

        return buffer;
    }

    void EntitySnapshot::Apply(Scene& scene, const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.empty())
        {
            return;
        }

        FMemoryReader reader(data);
        reader.ArIsNetArchive = true;

        while (reader.Tell() < reader.TotalSize() && !reader.IsError())
        {
            u64 uuid = 0;
            reader << uuid;

            Entity entity = scene.GetEntityByUUID(UUID(uuid));
            if (!entity)
            {
                // Entity not found — skip the transform data
                TransformComponent dummy;
                ComponentReplicator::Serialize(reader, dummy);
                continue;
            }

            if (entity.HasComponent<TransformComponent>())
            {
                auto& transform = entity.GetComponent<TransformComponent>();
                ComponentReplicator::Serialize(reader, transform);
            }
        }
    }
}
