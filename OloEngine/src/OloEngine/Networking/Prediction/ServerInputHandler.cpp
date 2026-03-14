#include "OloEnginePCH.h"
#include "ServerInputHandler.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    ServerInputHandler::ServerInputHandler() = default;

    bool ServerInputHandler::ProcessInput(Scene& scene, u32 senderClientID, const u8* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (size < sizeof(u32) + sizeof(u64))
        {
            OLO_CORE_WARN("[ServerInputHandler] Input command too small from client {}", senderClientID);
            return false;
        }

        // Deserialize: tick (u32) + entityUUID (u64) + input data (remainder)
        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;

        u32 tick = 0;
        u64 entityUUID = 0;
        reader << tick;
        reader << entityUUID;

        if (reader.IsError())
        {
            return false;
        }

        // Reject out-of-order or duplicate ticks
        if (auto it = m_LastProcessedTicks.find(senderClientID); it != m_LastProcessedTicks.end())
        {
            if (tick <= it->second)
            {
                OLO_CORE_TRACE("[ServerInputHandler] Ignoring stale tick {} from client {} (last processed: {})",
                               tick, senderClientID, it->second);
                return false;
            }
        }

        // Authority enforcement: verify the sender owns this entity
        auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID));
        if (!entityOpt.has_value())
        {
            OLO_CORE_WARN("[ServerInputHandler] Input for unknown entity {} from client {}", entityUUID,
                          senderClientID);
            return false;
        }

        Entity entity = entityOpt.value();

        if (!entity.HasComponent<NetworkIdentityComponent>())
        {
            OLO_CORE_WARN("[ServerInputHandler] Entity {} has no NetworkIdentityComponent", entityUUID);
            return false;
        }

        auto const& nic = entity.GetComponent<NetworkIdentityComponent>();

        // Only accept inputs for entities the client owns
        if (nic.OwnerClientID != senderClientID)
        {
            OLO_CORE_WARN("[ServerInputHandler] Client {} tried to control entity {} owned by client {}",
                          senderClientID, entityUUID, nic.OwnerClientID);
            return false;
        }

        // Only accept inputs for client-authoritative or shared entities
        if (nic.Authority == ENetworkAuthority::Server)
        {
            OLO_CORE_WARN("[ServerInputHandler] Client {} tried to control server-authoritative entity {}",
                          senderClientID, entityUUID);
            return false;
        }

        // Apply the input to the authoritative sim
        if (m_ApplyCallback)
        {
            u32 const payloadOffset = static_cast<u32>(reader.Tell());
            u32 const payloadSize = size - payloadOffset;
            m_ApplyCallback(scene, entityUUID, data + payloadOffset, payloadSize);
        }

        // Track last processed tick for this client
        m_LastProcessedTicks[senderClientID] = tick;
        return true;
    }

    void ServerInputHandler::SetInputApplyCallback(InputApplyCallback callback)
    {
        m_ApplyCallback = std::move(callback);
    }

    u32 ServerInputHandler::GetLastProcessedTick(u32 clientID) const
    {
        if (auto it = m_LastProcessedTicks.find(clientID); it != m_LastProcessedTicks.end())
        {
            return it->second;
        }
        return 0;
    }

    const std::unordered_map<u32, u32>& ServerInputHandler::GetAllLastProcessedTicks() const
    {
        return m_LastProcessedTicks;
    }
} // namespace OloEngine
