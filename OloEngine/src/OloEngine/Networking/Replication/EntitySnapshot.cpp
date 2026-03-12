#include "OloEnginePCH.h"
#include "EntitySnapshot.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    std::vector<u8> EntitySnapshot::Capture(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        // Iterate all entities that have both NetworkIdentityComponent and TransformComponent
        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();

        for (auto entityHandle : view)
        {
            auto& nic = view.get<NetworkIdentityComponent>(entityHandle);
            auto& tc  = view.get<TransformComponent>(entityHandle);

            // Skip entities that are not marked for replication
            if (!nic.IsReplicated)
            {
                continue;
            }

            // Write the entity UUID so we can look it up on the receiving end
            Entity entity{ entityHandle, &scene };
            u64 uuid = entity.GetUUID();
            writer << uuid;

            // Serialize TransformComponent
            ComponentReplicator::Serialize(writer, tc);
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

        while (!reader.AtEnd())
        {
            u64 uuid = 0;
            reader << uuid;

            if (reader.IsError())
            {
                OLO_CORE_WARN("[EntitySnapshot] Read error while reading UUID.");
                break;
            }

            Entity entity = scene.GetEntityByUUID(static_cast<UUID>(uuid));
            if (!entity)
            {
                // Entity not found — skip the transform data (9 floats = 9 * 4 bytes)
                // We must advance past the serialized TransformComponent bytes to keep
                // the reader in sync. A dummy component is used for this.
                TransformComponent dummy;
                ComponentReplicator::Serialize(reader, dummy);
                continue;
            }

            if (entity.HasComponent<TransformComponent>())
            {
                auto& tc = entity.GetComponent<TransformComponent>();
                ComponentReplicator::Serialize(reader, tc);
            }
        }
    }

} // namespace OloEngine
