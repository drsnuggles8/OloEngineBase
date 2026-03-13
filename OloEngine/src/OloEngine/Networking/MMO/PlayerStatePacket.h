#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/MMO/ZoneDefinition.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    // Serializable snapshot of a player's state for zone handoff.
    // Contains everything needed to reconstruct a player in a new zone.
    struct PlayerStatePacket
    {
        u32 ClientID = 0;
        u64 EntityUUID = 0;
        ZoneID SourceZoneID = 0;
        ZoneID TargetZoneID = 0;

        // Transform
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f };
        glm::vec3 Scale{ 1.0f };

        // Network identity
        u32 OwnerClientID = 0;
        bool IsReplicated = true;

        // Game-specific state (inventory, buffs, quest progress, etc.)
        std::vector<u8> GameStateBlob;

        // Serialize to byte buffer
        [[nodiscard]] std::vector<u8> Serialize() const;

        // Deserialize from byte buffer
        static PlayerStatePacket Deserialize(const u8* data, i64 size);
    };

    // Tracks the state of an in-progress zone handoff.
    enum class EHandoffState : u8
    {
        None = 0,
        Requested,  // Source zone sent HandoffRequest
        Ready,      // Target zone accepted, spawned entity
        Completing, // Source zone removing entity, redirecting client
        Completed,  // Handoff finished
        Rejected    // Target zone rejected (full, etc.)
    };

    struct HandoffTransaction
    {
        u32 TransactionID = 0;
        u32 ClientID = 0;
        ZoneID SourceZoneID = 0;
        ZoneID TargetZoneID = 0;
        EHandoffState State = EHandoffState::None;
        f32 ElapsedTime = 0.0f; // Time since handoff was initiated
        PlayerStatePacket PlayerState;
    };
} // namespace OloEngine
