#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/ClientReplicationDriver.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Every snapshot-bearing message is framed [serverTick: u32][snapshot bytes].
        // The tick lives in the MESSAGE, not in the snapshot buffer, so the snapshot
        // wire format stays exactly what EntitySnapshot::Parse already reads — and a
        // client can order and time-base its interpolation off the server's own
        // clock instead of counting arrivals (which drifts the moment one is lost).
        constexpr u32 kSnapshotTickPrefixSize = sizeof(u32);
    } // namespace

    ClientReplicationDriver::ClientReplicationDriver() = default;

    void ClientReplicationDriver::AttachTo(NetworkClient& client, Scene& scene)
    {
        auto& dispatcher = client.GetDispatcher();
        Scene* scenePtr = &scene;

        dispatcher.RegisterHandler(ENetworkMessageType::Connect,
                                   [this](u32, const u8* data, u32 size)
                                   { HandleConnectAssignment(data, size); });

        dispatcher.RegisterHandler(ENetworkMessageType::EntitySpawn,
                                   [this, scenePtr](u32, const u8* data, u32 size)
                                   { HandleSpawn(*scenePtr, data, size); });

        dispatcher.RegisterHandler(ENetworkMessageType::EntityDespawn,
                                   [this, scenePtr](u32, const u8* data, u32 size)
                                   { HandleDespawn(*scenePtr, data, size); });

        NetworkClient* clientPtr = &client;
        dispatcher.RegisterHandler(ENetworkMessageType::EntitySnapshot,
                                   [this, clientPtr](u32, const u8* data, u32 size)
                                   { HandleSnapshot(data, size, clientPtr); });

        dispatcher.RegisterHandler(ENetworkMessageType::DeltaSnapshot,
                                   [this, clientPtr](u32, const u8* data, u32 size)
                                   { HandleSnapshot(data, size, clientPtr); });

        dispatcher.RegisterHandler(ENetworkMessageType::InputAck,
                                   [this, scenePtr](u32, const u8* data, u32 size)
                                   { HandleInputAck(*scenePtr, data, size); });

        dispatcher.RegisterHandler(ENetworkMessageType::RPC,
                                   [this, scenePtr](u32, const u8* data, u32 size)
                                   { HandleRpc(*scenePtr, data, size); });
    }

    void ClientReplicationDriver::Tick(Scene& scene, NetworkClient& client, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        // Polling dispatches every queued handler synchronously, so spawns,
        // despawns and RPC handlers all run here — on the game thread — rather than
        // wherever the transport happened to receive them.
        client.PollMessages();

        if (m_SharedSceneWithServer)
        {
            return;
        }

        m_Interpolator.SetLocalClientID(m_LocalClientID);
        m_Interpolator.Interpolate(scene, dt);
    }

    void ClientReplicationDriver::SetSharedSceneWithServer(bool shared)
    {
        m_SharedSceneWithServer = shared;
    }

    bool ClientReplicationDriver::IsSharedSceneWithServer() const
    {
        return m_SharedSceneWithServer;
    }

    void ClientReplicationDriver::SetInputApplyCallback(InputApplyCallback callback)
    {
        m_Prediction.SetInputApplyCallback(callback);
        m_ApplyCallback = std::move(callback);
    }

    u32 ClientReplicationDriver::GetLocalClientID() const
    {
        return m_LocalClientID;
    }

    u64 ClientReplicationDriver::FindLocalPlayerEntity(Scene& scene) const
    {
        if (m_LocalClientID == 0)
        {
            return 0;
        }

        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
        for (auto handle : view)
        {
            Entity entity{ handle, &scene };
            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            if (nic.OwnerClientID == m_LocalClientID && nic.Authority != ENetworkAuthority::Server)
            {
                return static_cast<u64>(entity.GetUUID());
            }
        }
        return 0;
    }

    SnapshotInterpolator& ClientReplicationDriver::GetInterpolator()
    {
        return m_Interpolator;
    }

    ClientPrediction& ClientReplicationDriver::GetPrediction()
    {
        return m_Prediction;
    }

    u32 ClientReplicationDriver::GetLastReceivedServerTick() const
    {
        return m_LastServerTick;
    }

    u32 ClientReplicationDriver::GetCurrentInputTick() const
    {
        return m_InputTick;
    }

    const std::unordered_set<u64>& ClientReplicationDriver::GetLocallySpawnedEntities() const
    {
        return m_LocallySpawned;
    }

    void ClientReplicationDriver::Reset()
    {
        m_LocalClientID = 0;
        m_InputTick = 0;
        m_LastServerTick = 0;
        m_Authoritative.clear();
        m_LocallySpawned.clear();
        m_Interpolator.Reset();
        m_Prediction.ResetSession();
    }

    void ClientReplicationDriver::SendInput(Scene& scene, NetworkClient& client, u64 entityUUID,
                                            std::vector<u8> inputData)
    {
        OLO_PROFILE_FUNCTION();

        ++m_InputTick;
        const u32 tick = m_InputTick;

        // Listen server: the server half will apply this very command to the very
        // same scene when it arrives, so predicting it here would apply it twice —
        // and the replay buffer would never drain, because HandleInputAck skips
        // reconciliation in shared-scene mode. Send only.
        if (!m_SharedSceneWithServer)
        {
            m_Prediction.RecordInput(tick, entityUUID, inputData);

            // Predict: apply the input to the local simulation NOW rather than
            // waiting a round trip. Recording without applying (which is what the
            // facade used to do) is not prediction — it only buys a replay buffer
            // for a correction that never had anything to correct.
            if (m_ApplyCallback)
            {
                m_ApplyCallback(scene, entityUUID, inputData.data(), static_cast<u32>(inputData.size()));
            }
        }

        std::vector<u8> payload;
        {
            FMemoryWriter writer(payload);
            writer.ArIsNetArchive = true;
            u32 wireTick = tick;
            u64 wireEntity = entityUUID;
            writer << wireTick;
            writer << wireEntity;
            if (!inputData.empty())
            {
                writer.Serialize(inputData.data(), static_cast<i64>(inputData.size()));
            }
        }

        client.SendMessage(ENetworkMessageType::InputCommand, payload.data(), static_cast<u32>(payload.size()),
                           k_nSteamNetworkingSend_Reliable);
    }

    bool ClientReplicationDriver::InvokeRpc(NetworkClient& client, const RpcDescriptor& descriptor, u64 entityUUID,
                                            const RpcArgList& args)
    {
        OLO_PROFILE_FUNCTION();

        if (descriptor.Target != ERpcTarget::Server)
        {
            OLO_CORE_WARN_TAG("Networking", "Client refused to invoke '{}': only the server may originate a {} RPC",
                              descriptor.Name,
                              descriptor.Target == ERpcTarget::Client ? "Client-target" : "Multicast");
            return false;
        }

        const auto payload = RpcDispatcher::Encode(descriptor.Id, entityUUID, 0, args);
        const i32 sendFlags = descriptor.Reliability == ERpcReliability::Reliable ? k_nSteamNetworkingSend_Reliable
                                                                                  : k_nSteamNetworkingSend_Unreliable;
        return client.SendMessage(ENetworkMessageType::RPC, payload.data(), static_cast<u32>(payload.size()),
                                  sendFlags);
    }

    void ClientReplicationDriver::HandleConnectAssignment(const u8* data, u32 size)
    {
        if (data == nullptr || size < sizeof(u32))
        {
            OLO_CORE_WARN_TAG("Networking", "Ignoring malformed client-id assignment");
            return;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;
        u32 assignedID = 0;
        reader << assignedID;
        if (reader.IsError())
        {
            return;
        }

        m_LocalClientID = assignedID;
        m_Interpolator.SetLocalClientID(assignedID);
        OLO_CORE_INFO("[ClientReplication] Assigned client id {}", assignedID);
    }

    void ClientReplicationDriver::HandleSpawn(Scene& scene, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (m_SharedSceneWithServer)
        {
            return; // The server already created it in this very scene.
        }

        NetworkSpawnParams params;
        SnapshotEntity comps;
        if (!EntityLifecycle::DecodeSpawn(data, size, params, comps))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping malformed entity-spawn payload");
            return;
        }

        bool created = false;
        Entity entity = EntityLifecycle::ApplySpawn(scene, params, comps, created);
        if (!entity)
        {
            return;
        }

        if (created)
        {
            m_LocallySpawned.insert(params.EntityUUID);
        }

        // Seed the authoritative map so the next reassembled snapshot already
        // carries this entity — otherwise it would be missing from the interpolation
        // bracket until the server happened to send a delta covering it.
        m_Authoritative[params.EntityUUID] = std::move(comps);
    }

    void ClientReplicationDriver::HandleDespawn(Scene& scene, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (m_SharedSceneWithServer)
        {
            return; // The server already destroyed it in this very scene.
        }

        u64 uuid = 0;
        if (!EntityLifecycle::DecodeDespawn(data, size, uuid))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping malformed entity-despawn payload");
            return;
        }

        m_Authoritative.erase(uuid);

        // Only destroy what we created. A scene-authored entity leaving relevance
        // must stay in the level — it simply stops receiving updates.
        if (m_LocallySpawned.erase(uuid) == 0)
        {
            return;
        }

        if (auto entityOpt = scene.TryGetEntityWithUUID(UUID(uuid)); entityOpt.has_value())
        {
            scene.DestroyEntityAndChildren(*entityOpt);
        }
    }

    void ClientReplicationDriver::HandleSnapshot(const u8* data, u32 size, NetworkClient* client)
    {
        OLO_PROFILE_FUNCTION();

        if (m_SharedSceneWithServer || data == nullptr || size < kSnapshotTickPrefixSize)
        {
            return;
        }

        u32 serverTick = 0;
        {
            FMemoryReader reader(data, static_cast<i64>(size));
            reader.ArIsNetArchive = true;
            reader << serverTick;
            if (reader.IsError())
            {
                return;
            }
        }

        // Unreliable transport: an out-of-order (older) snapshot must not roll the
        // authoritative state backwards.
        if (serverTick <= m_LastServerTick)
        {
            return;
        }

        const std::vector<u8> body(data + kSnapshotTickPrefixSize, data + size);
        ParsedSnapshot parsed = EntitySnapshot::Parse(body);
        for (auto& [uuid, comps] : parsed)
        {
            m_Authoritative[uuid] = std::move(comps);
        }

        m_LastServerTick = serverTick;
        PushReassembledSnapshot(serverTick);

        // Confirm the baseline. Reliable, because a lost ack would strand the
        // server on an older baseline and cost a full resend every tick.
        if (client != nullptr)
        {
            std::vector<u8> ack;
            {
                FMemoryWriter writer(ack);
                writer.ArIsNetArchive = true;
                u32 tick = serverTick;
                writer << tick;
            }
            client->SendMessage(ENetworkMessageType::SnapshotAck, ack.data(), static_cast<u32>(ack.size()),
                                k_nSteamNetworkingSend_Reliable);
        }
    }

    void ClientReplicationDriver::PushReassembledSnapshot(u32 serverTick)
    {
        OLO_PROFILE_FUNCTION();

        // Entity order here follows the map's iteration order and is therefore not
        // deterministic — which is fine, and deliberately different from the SERVER
        // side. The server compares baseline bytes, so its order must be stable; the
        // interpolator parses this straight back into a UUID-keyed map, so ordering
        // carries no meaning. (Sorting would cost a copy per snapshot for nothing.)
        std::vector<u8> reassembled;
        for (const auto& [uuid, comps] : m_Authoritative)
        {
            EntitySnapshot::AppendEntityRecord(reassembled, uuid, comps);
        }
        m_Interpolator.PushSnapshot(serverTick, std::move(reassembled));
    }

    void ClientReplicationDriver::SnapOwnedEntitiesToAuthoritative(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (m_LocalClientID == 0)
        {
            return;
        }

        for (const auto& [uuid, comps] : m_Authoritative)
        {
            auto entityOpt = scene.TryGetEntityWithUUID(UUID(uuid));
            if (!entityOpt.has_value())
            {
                continue;
            }
            Entity entity = *entityOpt;
            if (!entity.HasComponent<NetworkIdentityComponent>())
            {
                continue;
            }
            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            if (nic.Authority == ENetworkAuthority::Server || nic.OwnerClientID != m_LocalClientID)
            {
                continue;
            }

            EntitySnapshot::ApplyEntityRecord(entity, comps, /*ensureComponents*/ false);
        }
    }

    void ClientReplicationDriver::HandleInputAck(Scene& scene, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (m_SharedSceneWithServer || data == nullptr || size < sizeof(u32))
        {
            return;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;
        u32 lastProcessedTick = 0;
        reader << lastProcessedTick;
        if (reader.IsError())
        {
            return;
        }

        // Order matters: rewind to the server's truth for the entities we predict,
        // THEN replay the inputs the server has not confirmed yet. Reconciling
        // without the rewind replays on top of the client's own prediction, so the
        // correction never lands and the divergence compounds.
        SnapOwnedEntitiesToAuthoritative(scene);
        m_Prediction.Reconcile(scene, lastProcessedTick);
    }

    void ClientReplicationDriver::HandleRpc(Scene& scene, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        RpcDispatcher::DecodedRpc call;
        if (!RpcDispatcher::Decode(data, size, call))
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping malformed RPC payload from server");
            return;
        }

        // A Client-target RPC addressed to a different client should never reach us,
        // but the check is cheap and keeps a mis-routed send from firing a handler
        // with someone else's arguments.
        if (call.TargetClientID != 0 && m_LocalClientID != 0 && call.TargetClientID != m_LocalClientID)
        {
            return;
        }

        (void)RpcDispatcher::ExecuteLocally(&scene, call, /*senderClientID*/ 0, /*receivedOnServer*/ false);
    }
} // namespace OloEngine
