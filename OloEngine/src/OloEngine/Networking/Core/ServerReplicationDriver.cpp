#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/ServerReplicationDriver.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] i32 ReliabilityFlags(ERpcReliability reliability)
        {
            return reliability == ERpcReliability::Reliable ? k_nSteamNetworkingSend_Reliable
                                                            : k_nSteamNetworkingSend_Unreliable;
        }

        // Is this entity part of the replicated set at all? The interest manager
        // answers "may this client see it", which is a different question — its
        // candidate set is every entity with an ID and a transform, networked or not.
        [[nodiscard]] bool IsReplicated(Scene& scene, u64 uuid)
        {
            auto entityOpt = scene.TryGetEntityWithUUID(UUID(uuid));
            if (!entityOpt.has_value())
            {
                return false;
            }
            Entity entity = *entityOpt;
            if (!entity.HasComponent<NetworkIdentityComponent>() || !entity.HasComponent<TransformComponent>())
            {
                return false;
            }
            return entity.GetComponent<NetworkIdentityComponent>().IsReplicated;
        }
    } // namespace

    ServerReplicationDriver::ServerReplicationDriver() = default;

    void ServerReplicationDriver::SetSnapshotRate(u32 hz)
    {
        if (hz == 0)
        {
            OLO_CORE_WARN_TAG("Networking", "Ignoring snapshot rate 0; keeping {} Hz", m_SnapshotRateHz);
            return;
        }
        m_SnapshotRateHz = hz;
    }

    u32 ServerReplicationDriver::GetSnapshotRate() const
    {
        return m_SnapshotRateHz;
    }

    void ServerReplicationDriver::SetInterestScopingEnabled(bool enabled)
    {
        m_InterestScoping = enabled;
    }

    bool ServerReplicationDriver::IsInterestScopingEnabled() const
    {
        return m_InterestScoping;
    }

    void ServerReplicationDriver::SetPlayerArchetype(std::string archetype)
    {
        m_PlayerArchetype = std::move(archetype);
    }

    const std::string& ServerReplicationDriver::GetPlayerArchetype() const
    {
        return m_PlayerArchetype;
    }

    void ServerReplicationDriver::SetPlayerSpawnCallback(PlayerSpawnCallback callback)
    {
        m_PlayerSpawnCallback = std::move(callback);
    }

    void ServerReplicationDriver::SetPlayerDespawnCallback(PlayerDespawnCallback callback)
    {
        m_PlayerDespawnCallback = std::move(callback);
    }

    void ServerReplicationDriver::SetClientRenderDelay(f32 seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            OLO_CORE_WARN_TAG("Networking", "Ignoring invalid client render delay {}", seconds);
            return;
        }
        m_ClientRenderDelay = seconds;
    }

    f32 ServerReplicationDriver::GetClientRenderDelay() const
    {
        return m_ClientRenderDelay;
    }

    u32 ServerReplicationDriver::GetCurrentTick() const
    {
        return m_Tick;
    }

    const SnapshotBuffer& ServerReplicationDriver::GetHistory() const
    {
        return m_History;
    }

    SnapshotBuffer& ServerReplicationDriver::GetHistory()
    {
        return m_History;
    }

    NetworkInterestManager& ServerReplicationDriver::GetInterestManager()
    {
        return m_Interest;
    }

    ServerInputHandler& ServerReplicationDriver::GetInputHandler()
    {
        return m_InputHandler;
    }

    LagCompensator& ServerReplicationDriver::GetLagCompensator()
    {
        return m_LagCompensator;
    }

    u64 ServerReplicationDriver::GetPlayerEntity(u32 clientID) const
    {
        if (auto it = m_Clients.find(clientID); it != m_Clients.end())
        {
            return it->second.PlayerEntity;
        }
        return 0;
    }

    std::vector<u32> ServerReplicationDriver::GetTrackedClients() const
    {
        std::vector<u32> ids;
        ids.reserve(m_Clients.size());
        for (const auto& [clientID, state] : m_Clients)
        {
            ids.push_back(clientID);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void ServerReplicationDriver::Reset()
    {
        // Per-client state lives in three places, and dropping only m_Clients leaves
        // the other two stale. A stop/restart never runs HandleClientDisconnected,
        // so a reconnecting client that reused an ID would find its last-processed
        // input tick still at the previous session's high-water mark — and every
        // input it sent would be rejected as stale until it caught up.
        for (const auto& [clientID, state] : m_Clients)
        {
            m_InputHandler.RemoveClient(clientID);
            m_Interest.RemoveClient(clientID);
        }

        m_Clients.clear();
        m_SpawnedArchetypes.clear();
        // Queued-but-unapplied commands belong to the session that is ending; a
        // restart must not materialise entities the new session never asked for.
        m_PendingSpawns.clear();
        m_PendingDespawns.clear();
        m_History.Clear();
        m_Accumulator = 0.0f;
        m_Tick = 0;
    }

    void ServerReplicationDriver::ResetForSceneSwap(Scene& scene, NetworkServer& server)
    {
        OLO_PROFILE_FUNCTION();

        // Tell each client to drop the replicated entities it holds BEFORE we forget
        // that it knows them. Clearing Known first would strand every one of them on
        // the client forever: the despawn pass only fires for entities that leave a
        // client's known set, so an entity we have already forgotten is never
        // mentioned again. (A client only destroys what the server spawned for it,
        // so this cannot touch its own scene-authored content.)
        for (auto& [clientID, state] : m_Clients)
        {
            for (u64 known : state.Known)
            {
                std::vector<u8> payload = EntityLifecycle::EncodeDespawn(known);
                server.SendMessageToClient(clientID, ENetworkMessageType::EntityDespawn, payload.data(),
                                           static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Reliable);
            }

            // Scene-derived: every one of these describes entities that no longer
            // exist. The connection itself survives.
            state.AckedBaseline.clear();
            state.AckedTick = 0;
            state.PendingSnapshots.clear();
            state.Known.clear();
            state.PlayerEntity = 0;

            // The observer position is a point in the old scene's space.
            m_Interest.RemoveClient(clientID);
        }

        m_SpawnedArchetypes.clear();
        m_PendingSpawns.clear();
        m_PendingDespawns.clear();
        m_History.Clear();
        m_Accumulator = 0.0f;

        // m_Tick and the per-client input ticks deliberately survive — see the header.

        // A still-connected client never sends another connect event, so nothing else
        // would ever give it a pawn in the new scene.
        for (auto& [clientID, state] : m_Clients)
        {
            state.PlayerEntity = SpawnPlayerFor(scene, clientID);
        }

        OLO_CORE_INFO("[ServerReplication] Scene swap: {} connection(s) kept, replication state rebuilt at tick {}",
                      m_Clients.size(), m_Tick);
    }

    void ServerReplicationDriver::HandleClientConnected(Scene& scene, NetworkServer& server, u32 clientID)
    {
        OLO_PROFILE_FUNCTION();

        ClientState& state = m_Clients[clientID];

        // Tell the client which id it is. Everything on the client that has to
        // reason about ownership — "is this my pawn", "may I predict it" — needs
        // this, and it is the only piece of the identity contract the client
        // cannot derive from the scene.
        u32 assignedID = clientID;
        std::vector<u8> payload;
        {
            FMemoryWriter writer(payload);
            writer.ArIsNetArchive = true;
            writer << assignedID;
        }
        server.SendMessageToClient(clientID, ENetworkMessageType::Connect, payload.data(),
                                   static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Reliable);

        state.PlayerEntity = SpawnPlayerFor(scene, clientID);

        OLO_CORE_INFO("[ServerReplication] Client {} joined (player entity {})", clientID, state.PlayerEntity);
    }

    u64 ServerReplicationDriver::SpawnPlayerFor(Scene& scene, u32 clientID)
    {
        if (m_PlayerSpawnCallback)
        {
            return m_PlayerSpawnCallback(scene, clientID);
        }

        if (!m_PlayerArchetype.empty())
        {
            Entity player = SpawnReplicated(scene, m_PlayerArchetype, "Player " + std::to_string(clientID), clientID,
                                            ENetworkAuthority::Client);
            return player ? static_cast<u64>(player.GetUUID()) : 0;
        }

        return 0;
    }

    void ServerReplicationDriver::HandleClientDisconnected(Scene& scene, NetworkServer& server, u32 clientID)
    {
        OLO_PROFILE_FUNCTION();

        u64 playerEntity = 0;
        if (auto it = m_Clients.find(clientID); it != m_Clients.end())
        {
            playerEntity = it->second.PlayerEntity;
        }

        if (m_PlayerDespawnCallback)
        {
            m_PlayerDespawnCallback(scene, clientID, playerEntity);
        }
        else if (playerEntity != 0)
        {
            DespawnReplicated(scene, server, playerEntity);
        }

        // Erase AFTER the despawn: DespawnReplicated walks every client's Known set,
        // and the leaving client's own entry must still be there for its bookkeeping
        // to stay consistent if a callback inspects it.
        m_Clients.erase(clientID);
        m_Interest.RemoveClient(clientID);
        m_InputHandler.RemoveClient(clientID);

        OLO_CORE_INFO("[ServerReplication] Client {} left", clientID);
    }

    Entity ServerReplicationDriver::SpawnReplicated(Scene& scene, std::string_view archetype, const std::string& name,
                                                    u32 ownerClientID, ENetworkAuthority authority)
    {
        return SpawnReplicatedWithUUID(scene, UUID(), archetype, name, ownerClientID, authority);
    }

    void ServerReplicationDriver::QueueSpawn(UUID uuid, std::string archetype, std::string name, u32 ownerClientID,
                                             ENetworkAuthority authority)
    {
        m_PendingSpawns.push_back({ uuid, std::move(archetype), std::move(name), ownerClientID, authority });
    }

    void ServerReplicationDriver::QueueDespawn(u64 entityUUID)
    {
        if (entityUUID != 0)
        {
            m_PendingDespawns.push_back(entityUUID);
        }
    }

    Entity ServerReplicationDriver::SpawnReplicatedWithUUID(Scene& scene, UUID uuid, std::string_view archetype,
                                                            const std::string& name, u32 ownerClientID,
                                                            ENetworkAuthority authority)
    {
        OLO_PROFILE_FUNCTION();

        Entity entity = scene.CreateEntityWithUUID(uuid, name);

        NetworkSpawnParams params;
        params.EntityUUID = static_cast<u64>(entity.GetUUID());
        params.Name = name;
        params.Archetype = std::string(archetype);
        params.OwnerClientID = ownerClientID;
        params.Authority = authority;

        // The server builds the entity through the SAME factory the client will,
        // so an archetype cannot mean two different things on the two ends.
        if (!params.Archetype.empty())
        {
            if (auto factory = NetworkSpawnRegistry::Find(params.Archetype); factory)
            {
                factory(entity, params);
            }
            else
            {
                OLO_CORE_WARN_TAG("Networking", "SpawnReplicated: unregistered archetype '{}'", params.Archetype);
            }
        }

        if (!entity.HasComponent<NetworkIdentityComponent>())
        {
            entity.AddComponent<NetworkIdentityComponent>();
        }
        auto& nic = entity.GetComponent<NetworkIdentityComponent>();
        nic.OwnerClientID = ownerClientID;
        nic.Authority = authority;
        nic.IsReplicated = true;

        if (!params.Archetype.empty())
        {
            m_SpawnedArchetypes[params.EntityUUID] = params.Archetype;
        }

        return entity;
    }

    std::string_view ServerReplicationDriver::LookupArchetype(u64 entityUUID) const
    {
        if (auto it = m_SpawnedArchetypes.find(entityUUID); it != m_SpawnedArchetypes.end())
        {
            return it->second;
        }
        // A scene-authored entity has no construction recipe — the client either
        // already has it, or builds a bare one that the replicated components fill.
        return {};
    }

    void ServerReplicationDriver::DespawnReplicated(Scene& scene, NetworkServer& server, u64 entityUUID)
    {
        OLO_PROFILE_FUNCTION();

        if (entityUUID == 0)
        {
            return;
        }

        const auto payload = EntityLifecycle::EncodeDespawn(entityUUID);
        for (auto& [clientID, state] : m_Clients)
        {
            if (state.Known.erase(entityUUID) > 0)
            {
                server.SendMessageToClient(clientID, ENetworkMessageType::EntityDespawn, payload.data(),
                                           static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Reliable);
            }
            if (state.PlayerEntity == entityUUID)
            {
                state.PlayerEntity = 0;
            }
        }

        m_SpawnedArchetypes.erase(entityUUID);

        if (auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID)); entityOpt.has_value())
        {
            scene.DestroyEntityAndChildren(*entityOpt);
        }
    }

    std::vector<u64> ServerReplicationDriver::ComputeRelevantSet(Scene& scene, u32 clientID)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u64> relevant;

        if (m_InterestScoping)
        {
            relevant = m_Interest.GetRelevantEntities(clientID, scene);
            // The interest manager's candidate set is every entity with an ID and a
            // transform. Narrow it to the replicated ones here — otherwise the
            // driver would happily "spawn" a purely local prop on every client.
            std::erase_if(relevant, [&scene](u64 uuid)
                          { return !IsReplicated(scene, uuid); });
        }
        else
        {
            auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
            for (auto handle : view)
            {
                Entity entity{ handle, &scene };
                if (entity.GetComponent<NetworkIdentityComponent>().IsReplicated)
                {
                    relevant.push_back(static_cast<u64>(entity.GetUUID()));
                }
            }
        }

        // Stable order so a delta's entity order does not churn between ticks
        // (the delta compares parsed maps, but the BASELINE bytes are compared as
        // whole records — an unstable order would make every tick look changed).
        std::sort(relevant.begin(), relevant.end());
        return relevant;
    }

    void ServerReplicationDriver::ReplicateToClient(Scene& scene, NetworkServer& server, u32 clientID,
                                                    ClientState& state)
    {
        OLO_PROFILE_FUNCTION();

        const std::vector<u64> relevant = ComputeRelevantSet(scene, clientID);

        // Spawns: everything newly in scope. The payload carries the entity's full
        // component state, so the client never renders a default-constructed frame.
        for (u64 const uuid : relevant)
        {
            if (state.Known.contains(uuid))
            {
                continue;
            }

            auto entityOpt = scene.TryGetEntityWithUUID(UUID(uuid));
            if (!entityOpt.has_value())
            {
                continue;
            }
            Entity entity = *entityOpt;

            NetworkSpawnParams params = EntityLifecycle::DescribeEntity(entity, LookupArchetype(uuid));

            const auto payload = EntityLifecycle::EncodeSpawn(params, EntitySnapshot::CaptureEntity(entity));
            server.SendMessageToClient(clientID, ENetworkMessageType::EntitySpawn, payload.data(),
                                       static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Reliable);
            state.Known.insert(uuid);
        }

        // Despawns: everything that dropped out of scope (moved away, stopped being
        // replicated, or was destroyed).
        std::vector<u64> stale;
        for (u64 const uuid : state.Known)
        {
            if (!std::binary_search(relevant.begin(), relevant.end(), uuid))
            {
                stale.push_back(uuid);
            }
        }
        for (u64 const uuid : stale)
        {
            const auto payload = EntityLifecycle::EncodeDespawn(uuid);
            server.SendMessageToClient(clientID, ENetworkMessageType::EntityDespawn, payload.data(),
                                       static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Reliable);
            state.Known.erase(uuid);
        }

        // State: delta against the newest state THIS connection confirmed applying.
        auto delta = EntitySnapshot::CaptureScopedDelta(scene, relevant, state.AckedBaseline);
        auto scopedFull = EntitySnapshot::CaptureScoped(scene, relevant);

        if (!delta.empty())
        {
            // Frame the server tick ahead of the snapshot bytes so the client can
            // time-base its interpolation on the server's clock and reject an
            // out-of-order arrival, without changing the snapshot format itself.
            std::vector<u8> payload;
            payload.reserve(sizeof(u32) + delta.size());
            {
                FMemoryWriter writer(payload);
                writer.ArIsNetArchive = true;
                u32 tick = m_Tick;
                writer << tick;
            }
            payload.insert(payload.end(), delta.begin(), delta.end());

            server.SendMessageToClient(clientID, ENetworkMessageType::DeltaSnapshot, payload.data(),
                                       static_cast<u32>(payload.size()), k_nSteamNetworkingSend_Unreliable);

            state.PendingSnapshots.emplace_back(m_Tick, std::move(scopedFull));
            if (state.PendingSnapshots.size() > kMaxPendingSnapshotsPerClient)
            {
                // The client has not acked for over a second and a half. Drop the
                // oldest pending state rather than growing without bound; the delta
                // stays correct because it is still computed against AckedBaseline.
                state.PendingSnapshots.erase(state.PendingSnapshots.begin());
            }
        }

        // Input ack: the tick this client's prediction may retire. Only sent when it
        // moves, so an idle client costs nothing.
        const u32 lastProcessed = m_InputHandler.GetLastProcessedTick(clientID);
        if (!state.HasSentAck || lastProcessed != state.LastAckSent)
        {
            std::vector<u8> ackPayload;
            {
                FMemoryWriter writer(ackPayload);
                writer.ArIsNetArchive = true;
                u32 tick = lastProcessed;
                writer << tick;
            }
            server.SendMessageToClient(clientID, ENetworkMessageType::InputAck, ackPayload.data(),
                                       static_cast<u32>(ackPayload.size()), k_nSteamNetworkingSend_Reliable);
            state.LastAckSent = lastProcessed;
            state.HasSentAck = true;
        }
    }

    void ServerReplicationDriver::Tick(Scene& scene, NetworkServer& server, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        // Apply script-requested spawns/despawns FIRST, at the safe point. Tick runs
        // after the scene's simulation step, so we are outside Scene::UpdateScripts
        // and these structural changes cannot invalidate an iterator a script
        // dispatch is holding. Swap the queues out before applying: a spawned
        // entity's own script may queue more work, which the next tick picks up
        // rather than mutating the container we are walking.
        if (!m_PendingSpawns.empty())
        {
            std::vector<PendingSpawn> spawns;
            spawns.swap(m_PendingSpawns);
            for (const auto& spawn : spawns)
            {
                (void)SpawnReplicatedWithUUID(scene, spawn.EntityUUID, spawn.Archetype, spawn.Name,
                                              spawn.OwnerClientID, spawn.Authority);
            }
        }
        if (!m_PendingDespawns.empty())
        {
            std::vector<u64> despawns;
            despawns.swap(m_PendingDespawns);
            for (u64 const uuid : despawns)
            {
                DespawnReplicated(scene, server, uuid);
            }
        }

        // Connection lifecycle next, and every frame rather than once per
        // replication tick — a client that connects and immediately sends input
        // must already have its state (and its pawn) by the time that input lands.
        for (const auto& event : server.DrainClientEvents())
        {
            if (event.Connected)
            {
                HandleClientConnected(scene, server, event.ClientID);
            }
            else
            {
                HandleClientDisconnected(scene, server, event.ClientID);
            }
        }

        if (!std::isfinite(dt) || dt < 0.0f)
        {
            dt = 0.0f;
        }
        m_Accumulator += dt;

        const f32 interval = 1.0f / static_cast<f32>(m_SnapshotRateHz);
        if (m_Accumulator < interval)
        {
            return;
        }

        // Consume whole intervals but emit at most ONE snapshot per call: a long
        // frame hitch must not burst a dozen snapshots onto the wire describing a
        // world that only ever had one state.
        while (m_Accumulator >= interval)
        {
            m_Accumulator -= interval;
        }

        ++m_Tick;

        // Unscoped history for lag compensation — a rewind has to be able to restore
        // entities no single client currently sees.
        m_History.Push(m_Tick, EntitySnapshot::Capture(scene));

        m_Interest.UpdateSpatialGrid(scene);

        // Observer position per client = its pawn's position. Without this every
        // client sits at the origin and distance culling is meaningless.
        for (const auto& [clientID, state] : m_Clients)
        {
            if (state.PlayerEntity == 0)
            {
                continue;
            }
            if (auto entityOpt = scene.TryGetEntityWithUUID(UUID(state.PlayerEntity)); entityOpt.has_value())
            {
                Entity player = *entityOpt;
                if (player.HasComponent<TransformComponent>())
                {
                    m_Interest.SetClientPosition(clientID, player.GetComponent<TransformComponent>().Translation);
                }
            }
        }

        // Only clients the transport still reports as connected — m_Clients can hold
        // an entry whose disconnect event has not been drained yet.
        for (u32 const clientID : server.GetConnectedClientIDs())
        {
            auto it = m_Clients.find(clientID);
            if (it == m_Clients.end())
            {
                continue;
            }
            ReplicateToClient(scene, server, clientID, it->second);
        }
    }

    void ServerReplicationDriver::HandleInputCommand(Scene& scene, u32 senderClientID, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_InputHandler.ProcessInput(scene, senderClientID, data, size))
        {
            OLO_CORE_TRACE("[ServerReplication] Rejected input from client {}", senderClientID);
        }
    }

    void ServerReplicationDriver::HandleRpc(Scene& scene, u32 senderClientID, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        RpcDispatcher::DecodedRpc call;
        if (!RpcDispatcher::Decode(data, size, call))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping malformed RPC payload from client {}", senderClientID);
            return;
        }

        (void)RpcDispatcher::ExecuteLocally(&scene, call, senderClientID, /*receivedOnServer*/ true);
    }

    void ServerReplicationDriver::HandleSnapshotAck(u32 senderClientID, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (data == nullptr || size < sizeof(u32))
        {
            return;
        }

        auto it = m_Clients.find(senderClientID);
        if (it == m_Clients.end())
        {
            return;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;
        u32 ackedTick = 0;
        reader << ackedTick;
        if (reader.IsError())
        {
            return;
        }

        ClientState& state = it->second;
        if (ackedTick <= state.AckedTick)
        {
            return; // Stale or duplicate ack.
        }

        // Adopt the acked state as the new baseline and retire everything up to it.
        // A client can only ack a tick we actually sent it; anything else is either
        // a forged or a badly-delayed ack and is ignored.
        bool found = false;
        for (auto& [tick, bytes] : state.PendingSnapshots)
        {
            if (tick == ackedTick)
            {
                state.AckedBaseline = std::move(bytes);
                state.AckedTick = ackedTick;
                found = true;
                break;
            }
        }

        if (!found)
        {
            return;
        }

        std::erase_if(state.PendingSnapshots, [ackedTick](const std::pair<u32, std::vector<u8>>& entry)
                      { return entry.first <= ackedTick; });
    }

    bool ServerReplicationDriver::InvokeRpc(Scene& scene, NetworkServer& server, const RpcDescriptor& descriptor,
                                            u64 entityUUID, u32 targetClientID, const RpcArgList& args)
    {
        OLO_PROFILE_FUNCTION();

        const i32 sendFlags = ReliabilityFlags(descriptor.Reliability);

        switch (descriptor.Target)
        {
            case ERpcTarget::Server:
            {
                // The server invoking a Server RPC just runs it — there is nobody
                // more authoritative to forward it to.
                RpcDispatcher::DecodedRpc call;
                call.Id = descriptor.Id;
                call.EntityUUID = entityUUID;
                call.Args = args;
                return RpcDispatcher::ExecuteLocally(&scene, call, /*senderClientID*/ 0, /*receivedOnServer*/ true);
            }

            case ERpcTarget::Client:
            {
                if (targetClientID == 0)
                {
                    OLO_CORE_WARN_TAG("Networking", "Client-target RPC '{}' invoked with no target client",
                                      descriptor.Name);
                    return false;
                }
                const auto payload = RpcDispatcher::Encode(descriptor.Id, entityUUID, targetClientID, args);
                return server.SendMessageToClient(targetClientID, ENetworkMessageType::RPC, payload.data(),
                                                  static_cast<u32>(payload.size()), sendFlags);
            }

            case ERpcTarget::Multicast:
            {
                const auto payload = RpcDispatcher::Encode(descriptor.Id, entityUUID, 0, args);
                server.BroadcastMessage(ENetworkMessageType::RPC, payload.data(), static_cast<u32>(payload.size()),
                                        sendFlags);

                // Multicast runs on the server too — it is "everyone", and the
                // server is part of everyone. Handlers see IsServer == true so a
                // handler that must only run client-side can opt out.
                //
                // Route through ExecuteLocally rather than calling the handler
                // directly, so the server-side execution gets the same descriptor
                // re-lookup, authority re-check and refusal logging as every other
                // path — one place decides whether a call may run.
                RpcDispatcher::DecodedRpc call;
                call.Id = descriptor.Id;
                call.EntityUUID = entityUUID;
                call.Args = args;
                (void)RpcDispatcher::ExecuteLocally(&scene, call, /*senderClientID*/ 0, /*receivedOnServer*/ true);
                return true;
            }
        }

        return false;
    }

    bool ServerReplicationDriver::PerformLagCompensatedCheck(Scene& scene, NetworkServer& server, u32 clientID,
                                                             const LagCompensator::RewindCallback& callback)
    {
        OLO_PROFILE_FUNCTION();

        if (m_SnapshotRateHz == 0)
        {
            return false;
        }

        // How far behind "now" this client's world actually was when they acted:
        // half the round trip for the command to reach us, plus the interpolation
        // delay their renderer runs behind the newest snapshot it holds.
        const i32 pingMs = server.GetClientPingMsById(clientID);

        const f32 halfRttSeconds = pingMs > 0 ? (static_cast<f32>(pingMs) * 0.5f / 1000.0f) : 0.0f;
        const f32 rewindSeconds = halfRttSeconds + m_ClientRenderDelay;
        const u32 rewindTicks = static_cast<u32>(std::lround(rewindSeconds * static_cast<f32>(m_SnapshotRateHz)));

        // PerformLagCompensatedCheck refuses target >= current, so a zero rewind
        // must still name a strictly older tick.
        const u32 effectiveRewind = std::max(1u, rewindTicks);
        if (m_Tick <= effectiveRewind)
        {
            OLO_CORE_TRACE("[ServerReplication] Not enough snapshot history to lag-compensate client {} yet", clientID);
            return false;
        }

        LagCompensationParams params;
        params.TargetTick = m_Tick - effectiveRewind;
        params.CurrentTick = m_Tick;
        params.TickRateHz = m_SnapshotRateHz;

        return m_LagCompensator.PerformLagCompensatedCheck(scene, m_History, params, callback);
    }
} // namespace OloEngine
