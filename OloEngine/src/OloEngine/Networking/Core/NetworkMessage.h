#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    enum class ENetworkMessageType : u16
    {
        None = 0,
        Connect,
        Disconnect,
        Ping,
        Pong,
        EntitySnapshot,
        RPC,

        // Range for user-defined messages
        UserMessage = 1000
    };

    struct NetworkMessageHeader
    {
        ENetworkMessageType Type = ENetworkMessageType::None;
        u32 Size = 0; // Payload size (excluding header)
        u8 Flags = 0; // Reliability, channel, etc.
    };
} // namespace OloEngine
