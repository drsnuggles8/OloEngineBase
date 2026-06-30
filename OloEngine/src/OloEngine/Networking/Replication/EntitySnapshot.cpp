#include "OloEnginePCH.h"
#include "EntitySnapshot.h"
#include "ComponentInterpolationRegistry.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <utility>

namespace OloEngine
{
    namespace
    {
        // Serialize the registry-driven set of present components for one entity,
        // in registration order. Empty if the entity carries no replicated component.
        SnapshotEntity CollectComponents(Entity& entity)
        {
            SnapshotEntity comps;
            for (const auto& entry : ComponentInterpolationRegistry::GetEntries())
            {
                if (entry.Has == nullptr || entry.Capture == nullptr || !entry.Has(entity))
                {
                    continue;
                }
                std::vector<u8> bytes;
                FMemoryWriter compWriter(bytes);
                compWriter.ArIsNetArchive = true;
                entry.Capture(compWriter, entity);
                comps.push_back({ entry.Id, std::move(bytes) });
            }
            return comps;
        }

        // Append a raw byte run to a writer. FMemoryWriter::Serialize only *reads*
        // from the source when saving, so the const_cast is sound.
        void WriteBytes(FMemoryWriter& writer, const std::vector<u8>& bytes)
        {
            if (!bytes.empty())
            {
                writer.Serialize(const_cast<u8*>(bytes.data()), static_cast<i64>(bytes.size()));
            }
        }

        // Emit one entity record: [uuid][count] then [id][len][bytes] per component.
        void WriteEntity(FMemoryWriter& writer, u64 uuid, const SnapshotEntity& comps)
        {
            if (comps.empty())
            {
                return;
            }

            writer << uuid;
            u16 count = static_cast<u16>(comps.size());
            writer << count;
            for (const auto& sc : comps)
            {
                u32 id = sc.Id;
                u32 len = static_cast<u32>(sc.Bytes.size());
                writer << id << len;
                WriteBytes(writer, sc.Bytes);
            }
        }

        [[nodiscard]] bool ComponentsEqual(const SnapshotEntity& a, const SnapshotEntity& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (sizet i = 0; i < a.size(); ++i)
            {
                if (a[i].Id != b[i].Id || a[i].Bytes != b[i].Bytes)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

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
            if (auto const& nic = entity.GetComponent<NetworkIdentityComponent>(); !nic.IsReplicated)
            {
                continue;
            }

            u64 uuid = static_cast<u64>(entity.GetUUID());
            WriteEntity(writer, uuid, CollectComponents(entity));
        }

        return buffer;
    }

    ParsedSnapshot EntitySnapshot::Parse(const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        ParsedSnapshot result;
        if (data.empty())
        {
            return result;
        }

        FMemoryReader reader(data);
        reader.ArIsNetArchive = true;

        while (reader.Tell() < reader.TotalSize() && !reader.IsError())
        {
            u64 uuid = 0;
            reader << uuid;

            u16 count = 0;
            reader << count;
            if (reader.IsError())
            {
                break;
            }

            SnapshotEntity comps;
            comps.reserve(count);
            for (u16 i = 0; i < count && !reader.IsError(); ++i)
            {
                u32 id = 0;
                u32 len = 0;
                reader << id << len;
                if (reader.IsError())
                {
                    break;
                }

                // Reject a corrupt / hostile length that would run past the buffer.
                if (reader.Tell() + static_cast<i64>(len) > reader.TotalSize())
                {
                    reader.SetError();
                    break;
                }

                std::vector<u8> bytes(len);
                if (len > 0)
                {
                    reader.Serialize(bytes.data(), static_cast<i64>(len));
                }
                comps.push_back({ id, std::move(bytes) });
            }

            if (reader.IsError())
            {
                break;
            }
            result[uuid] = std::move(comps);
        }

        return result;
    }

    void EntitySnapshot::Apply(Scene& scene, const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.empty())
        {
            return;
        }

        const ParsedSnapshot parsed = Parse(data);
        for (const auto& [uuid, comps] : parsed)
        {
            Entity entity = scene.GetEntityByUUID(UUID(uuid));
            if (!entity)
            {
                continue;
            }

            for (const auto& sc : comps)
            {
                const auto* entry = ComponentInterpolationRegistry::FindById(sc.Id);
                if (entry == nullptr || entry->Snap == nullptr || entry->Has == nullptr)
                {
                    continue;
                }
                if (!entry->Has(entity))
                {
                    continue;
                }
                entry->Snap(entity, sc.Bytes);
            }
        }
    }

    std::vector<u8> EntitySnapshot::CaptureDelta(Scene& scene, const std::vector<u8>& baseline)
    {
        OLO_PROFILE_FUNCTION();

        const ParsedSnapshot baselineMap = Parse(baseline);

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };
            if (auto const& nic = entity.GetComponent<NetworkIdentityComponent>(); !nic.IsReplicated)
            {
                continue;
            }

            u64 uuid = static_cast<u64>(entity.GetUUID());
            SnapshotEntity comps = CollectComponents(entity);
            if (comps.empty())
            {
                continue;
            }

            bool changed = true;
            if (auto it = baselineMap.find(uuid); it != baselineMap.end())
            {
                changed = !ComponentsEqual(comps, it->second);
            }

            if (changed)
            {
                WriteEntity(writer, uuid, comps);
            }
        }

        return buffer;
    }
} // namespace OloEngine
