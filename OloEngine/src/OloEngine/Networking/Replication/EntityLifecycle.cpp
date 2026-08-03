#include "OloEnginePCH.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <iterator>
#include <unordered_map>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Bound the two attacker-controlled string fields of a spawn payload.
        // std::string's operator<< resizes to the declared length before reading, so
        // an unbounded length is an allocation primitive even though the read then
        // fails. Entity names and archetype ids are short by construction.
        constexpr i32 kMaxSpawnStringLength = 512;

        FMutex g_SpawnRegistryMutex;
        std::unordered_map<std::string, NetworkSpawnFactory> g_SpawnFactories;
        bool g_SpawnDefaultsRegistered = false;

        [[nodiscard]] bool ReadBoundedString(FArchive& ar, std::string& out)
        {
            i32 length = 0;
            ar << length;
            if (ar.IsError() || length < 0 || length > kMaxSpawnStringLength ||
                ar.Tell() + static_cast<i64>(length) > ar.TotalSize())
            {
                return false;
            }
            out.assign(static_cast<sizet>(length), '\0');
            if (length > 0)
            {
                ar.Serialize(out.data(), length);
            }
            return !ar.IsError();
        }

        void WriteString(FArchive& ar, const std::string& value)
        {
            i32 length = static_cast<i32>(value.size());
            ar << length;
            if (length > 0)
            {
                ar.Serialize(const_cast<char*>(value.data()), length);
            }
        }

        void NetworkPlayerFactory(Entity& entity, const NetworkSpawnParams& params)
        {
            if (!entity.HasComponent<TransformComponent>())
            {
                entity.AddComponent<TransformComponent>();
            }

            if (!entity.HasComponent<NetworkIdentityComponent>())
            {
                entity.AddComponent<NetworkIdentityComponent>();
            }
            auto& nic = entity.GetComponent<NetworkIdentityComponent>();
            nic.OwnerClientID = params.OwnerClientID;
            nic.Authority = params.Authority;
            nic.IsReplicated = true;

            // Asset-free visual: a coloured quad renders on any client with a 2D
            // camera, so the demo needs no texture/mesh import to be watchable.
            if (!entity.HasComponent<SpriteRendererComponent>())
            {
                entity.AddComponent<SpriteRendererComponent>();
            }
            entity.GetComponent<SpriteRendererComponent>().Color =
                NetworkSpawnRegistry::PlayerColorForClient(params.OwnerClientID);
        }
    } // namespace

    void NetworkSpawnRegistry::RegisterDefaults()
    {
        TUniqueLock<FMutex> lock(g_SpawnRegistryMutex);
        if (g_SpawnDefaultsRegistered)
        {
            return;
        }
        g_SpawnDefaultsRegistered = true;
        g_SpawnFactories[kNetworkPlayerArchetype] = &NetworkPlayerFactory;
    }

    void NetworkSpawnRegistry::Register(std::string archetype, NetworkSpawnFactory factory)
    {
        if (archetype.empty())
        {
            OLO_CORE_WARN_TAG("Networking", "NetworkSpawnRegistry::Register ignored an empty archetype name");
            return;
        }
        TUniqueLock<FMutex> lock(g_SpawnRegistryMutex);
        g_SpawnFactories[std::move(archetype)] = std::move(factory);
    }

    NetworkSpawnFactory NetworkSpawnRegistry::Find(std::string_view archetype)
    {
        TUniqueLock<FMutex> lock(g_SpawnRegistryMutex);
        if (auto it = g_SpawnFactories.find(std::string(archetype)); it != g_SpawnFactories.end())
        {
            return it->second;
        }
        return {};
    }

    void NetworkSpawnRegistry::Clear()
    {
        TUniqueLock<FMutex> lock(g_SpawnRegistryMutex);
        g_SpawnFactories.clear();
        g_SpawnDefaultsRegistered = false;
    }

    glm::vec4 NetworkSpawnRegistry::PlayerColorForClient(u32 clientID)
    {
        // Six well-separated hues cycled by client id — deterministic on both ends
        // (the client derives the same colour the server did) and readable against
        // the demo's dark background.
        constexpr glm::vec4 kPalette[] = {
            { 0.90f, 0.30f, 0.25f, 1.0f }, // red
            { 0.25f, 0.55f, 0.95f, 1.0f }, // blue
            { 0.35f, 0.80f, 0.40f, 1.0f }, // green
            { 0.95f, 0.75f, 0.25f, 1.0f }, // amber
            { 0.70f, 0.40f, 0.85f, 1.0f }, // violet
            { 0.30f, 0.80f, 0.80f, 1.0f }, // teal
        };
        constexpr u32 kPaletteSize = static_cast<u32>(std::size(kPalette));
        return kPalette[clientID % kPaletteSize];
    }

    std::vector<u8> EntityLifecycle::EncodeSpawn(const NetworkSpawnParams& params, const SnapshotEntity& comps)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            writer.ArIsNetArchive = true;

            // The uuid lives in the HEADER, not only in the entity record. An entity
            // whose replicated component set happens to be empty produces an empty
            // record (the shared encoder writes nothing for zero components), and a
            // spawn that could not name its own entity would be undecodable.
            u64 uuid = params.EntityUUID;
            writer << uuid;
            WriteString(writer, params.Name);
            WriteString(writer, params.Archetype);
            u32 owner = params.OwnerClientID;
            u8 authority = static_cast<u8>(params.Authority);
            writer << owner << authority;
        }

        // The entity record is appended through the shared snapshot encoder so
        // spawn and snapshot payloads can never disagree about the format.
        EntitySnapshot::AppendEntityRecord(buffer, params.EntityUUID, comps);
        return buffer;
    }

    bool EntityLifecycle::DecodeSpawn(const u8* data, u32 size, NetworkSpawnParams& outParams, SnapshotEntity& outComps)
    {
        OLO_PROFILE_FUNCTION();

        if (data == nullptr || size == 0)
        {
            return false;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;

        u64 uuid = 0;
        reader << uuid;
        if (reader.IsError() || uuid == 0)
        {
            return false;
        }

        std::string name;
        std::string archetype;
        if (!ReadBoundedString(reader, name) || !ReadBoundedString(reader, archetype))
        {
            return false;
        }

        u32 owner = 0;
        u8 authority = 0;
        reader << owner << authority;
        if (reader.IsError())
        {
            return false;
        }

        // Only the three declared enumerators are legal — a forged value would be
        // cast into an out-of-range ENetworkAuthority and then compared against
        // ::Server / ::Client all over the authority checks.
        if (authority > static_cast<u8>(ENetworkAuthority::Shared))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping spawn with out-of-range authority {}", authority);
            return false;
        }

        // The remainder is exactly one standard entity record; hand it to the
        // hardened snapshot parser rather than re-implementing the walk.
        const i64 recordOffset = reader.Tell();
        if (recordOffset < 0 || recordOffset > static_cast<i64>(size))
        {
            return false;
        }
        std::vector<u8> record(data + recordOffset, data + size);
        ParsedSnapshot parsed = EntitySnapshot::Parse(record);
        if (parsed.size() > 1)
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping spawn whose entity record held {} entities (expected 0 or 1)",
                              parsed.size());
            return false;
        }

        // An empty record is legitimate — an entity can carry no replicated
        // component, which is why the uuid lives in the header. But Parse stops at
        // the first malformed byte and returns whatever it accumulated, so "bytes
        // present, zero entities parsed" means TRUNCATED, not empty. Without this
        // distinction a half-delivered spawn would silently apply as an entity with
        // no state instead of being refused.
        if (parsed.empty() && recordOffset < static_cast<i64>(size))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping spawn whose entity record is truncated ({} trailing bytes)",
                              static_cast<i64>(size) - recordOffset);
            return false;
        }

        SnapshotEntity comps;
        if (!parsed.empty())
        {
            auto it = parsed.begin();
            // A record naming a different entity than the header is a forged or
            // corrupt payload — refuse rather than picking one of the two.
            if (it->first != uuid)
            {
                OLO_CORE_WARN_TAG("Networking", "Dropping spawn whose record entity {} disagrees with header entity {}",
                                  it->first, uuid);
                return false;
            }
            comps = std::move(it->second);
        }

        outParams.EntityUUID = uuid;
        outParams.Name = std::move(name);
        outParams.Archetype = std::move(archetype);
        outParams.OwnerClientID = owner;
        outParams.Authority = static_cast<ENetworkAuthority>(authority);
        outComps = std::move(comps);
        return true;
    }

    std::vector<u8> EntityLifecycle::EncodeDespawn(u64 entityUUID)
    {
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;
        u64 uuid = entityUUID;
        writer << uuid;
        return buffer;
    }

    bool EntityLifecycle::DecodeDespawn(const u8* data, u32 size, u64& outEntityUUID)
    {
        if (data == nullptr || size < sizeof(u64))
        {
            return false;
        }
        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;
        u64 uuid = 0;
        reader << uuid;
        if (reader.IsError())
        {
            return false;
        }
        outEntityUUID = uuid;
        return true;
    }

    NetworkSpawnParams EntityLifecycle::DescribeEntity(Entity& entity, std::string_view archetype)
    {
        NetworkSpawnParams params;
        params.EntityUUID = static_cast<u64>(entity.GetUUID());
        params.Name = entity.GetName();
        params.Archetype = std::string(archetype);
        if (entity.HasComponent<NetworkIdentityComponent>())
        {
            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            params.OwnerClientID = nic.OwnerClientID;
            params.Authority = nic.Authority;
        }
        return params;
    }

    Entity EntityLifecycle::ApplySpawn(Scene& scene, const NetworkSpawnParams& params, const SnapshotEntity& comps,
                                       bool& outCreated)
    {
        OLO_PROFILE_FUNCTION();

        outCreated = false;

        if (params.EntityUUID == 0)
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping spawn with a null entity UUID");
            return {};
        }

        Entity entity;
        if (auto existing = scene.TryGetEntityWithUUID(UUID(params.EntityUUID)); existing.has_value())
        {
            entity = *existing;
        }
        else
        {
            entity = scene.CreateEntityWithUUID(UUID(params.EntityUUID), params.Name);
            outCreated = true;

            // Only run the archetype factory for an entity WE created. Re-running it
            // over a scene-authored entity would stomp locally-authored components
            // with the archetype's defaults every time the entity re-entered
            // relevance.
            if (!params.Archetype.empty())
            {
                if (auto factory = NetworkSpawnRegistry::Find(params.Archetype); factory)
                {
                    factory(entity, params);
                }
                else
                {
                    OLO_CORE_WARN_TAG("Networking",
                                      "Spawn for entity {} names unregistered archetype '{}'; creating a bare entity "
                                      "(replicated components still apply)",
                                      params.EntityUUID, params.Archetype);
                }
            }
        }

        // The identity is authoritative on every spawn, created or not — ownership
        // can change over an entity's life (a possessed pawn) and the client must
        // follow, or its authority checks and prediction gating go stale.
        if (!entity.HasComponent<NetworkIdentityComponent>())
        {
            entity.AddComponent<NetworkIdentityComponent>();
        }
        auto& nic = entity.GetComponent<NetworkIdentityComponent>();
        nic.OwnerClientID = params.OwnerClientID;
        nic.Authority = params.Authority;
        nic.IsReplicated = true;

        EntitySnapshot::ApplyEntityRecord(entity, comps, /*ensureComponents*/ true);
        return entity;
    }
} // namespace OloEngine
