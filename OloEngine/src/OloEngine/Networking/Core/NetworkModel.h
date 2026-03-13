#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // The networking model used by the current session.
    // Selected at session creation time; determines which subsystems are active.
    enum class ENetworkModel : u8
    {
        None = 0,
        ClientServerAuthoritative, // FPS/ARPG: server-authoritative with client prediction
        Lockstep,                  // RTS: all peers advance in sync
        PeerToPeer,                // Fighting/racing: P2P with rollback
        TurnBased,                 // Strategy: turn-based with validated moves
        MMO                        // Persistent world: zone-based architecture
    };
} // namespace OloEngine
