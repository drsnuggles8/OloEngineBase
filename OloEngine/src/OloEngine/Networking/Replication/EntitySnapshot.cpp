#include "OloEnginePCH.h"
#include "EntitySnapshot.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <cstring>
#include <unordered_map>

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

    std::vector<u8> EntitySnapshot::CaptureDelta(Scene& scene, const std::vector<u8>& baseline)
    {
        OLO_PROFILE_FUNCTION();

        // Parse baseline into a map of UUID → serialized transform bytes
        // Each transform is 9 floats = 36 bytes.
        static constexpr u32 kTransformBytes = 9 * sizeof(f32);

        std::unordered_map<u64, const u8*> baselineMap;
        if (!baseline.empty())
        {
            FMemoryReader baseReader(baseline);
            baseReader.ArIsNetArchive = true;

            while (baseReader.Tell() < baseReader.TotalSize() && !baseReader.IsError())
            {
                u64 uuid = 0;
                baseReader << uuid;

                i64 const transformStart = baseReader.Tell();
                if (transformStart + kTransformBytes > baseReader.TotalSize())
                {
                    break;
                }

                baselineMap[uuid] = baseline.data() + transformStart;

                // Skip past the transform data
                TransformComponent dummy;
                ComponentReplicator::Serialize(baseReader, dummy);
            }
        }

        // Capture only entities whose serialized transform differs from baseline
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        // Reusable buffer for per-entity transform serialization
        std::vector<u8> currentTransformBuf;
        currentTransformBuf.reserve(kTransformBytes);

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

            // Serialize the current transform to the reusable buffer for comparison
            currentTransformBuf.clear();
            FMemoryWriter tmpWriter(currentTransformBuf);
            tmpWriter.ArIsNetArchive = true;
            auto& transform = entity.GetComponent<TransformComponent>();
            ComponentReplicator::Serialize(tmpWriter, transform);

            // Compare with baseline
            bool changed = true;
            if (auto it = baselineMap.find(uuid); it != baselineMap.end())
            {
                if (currentTransformBuf.size() == kTransformBytes)
                {
                    changed = (std::memcmp(it->second, currentTransformBuf.data(), kTransformBytes) != 0);
                }
            }

            if (changed)
            {
                writer << uuid;
                buffer.insert(buffer.end(), currentTransformBuf.begin(), currentTransformBuf.end());
            }
        }

        return buffer;
    }
} // namespace OloEngine
