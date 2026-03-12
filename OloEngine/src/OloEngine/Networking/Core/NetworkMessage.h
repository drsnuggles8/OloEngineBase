#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @enum ENetworkMessageType
    // @brief Identifies the payload carried by a NetworkMessageHeader.
    enum class ENetworkMessageType : u16
    {
        None = 0,
        Connect,
        Disconnect,
        Ping,
        Pong,
        EntitySnapshot,
        RPC,

        // Range reserved for user-defined messages (game-specific)
        UserMessage = 1000
    };

    // @struct NetworkMessageHeader
    // @brief Prefixed to every network message payload.
    //
    // Layout (std layout, 8 bytes total):
    //   Type  : 2 bytes
    //   Size  : 4 bytes  (payload bytes excluding header)
    //   Flags : 1 byte
    //   _pad  : 1 byte
    struct NetworkMessageHeader
    {
        ENetworkMessageType Type  = ENetworkMessageType::None;
        u32                 Size  = 0;
        u8                  Flags = 0;
        u8                  _pad  = 0; // explicit padding to 8 bytes

        NetworkMessageHeader() = default;
        NetworkMessageHeader(ENetworkMessageType type, u32 payloadSize, u8 flags = 0)
            : Type(type), Size(payloadSize), Flags(flags), _pad(0)
        {
        }
    };
    static_assert(sizeof(NetworkMessageHeader) == 8, "NetworkMessageHeader must be 8 bytes");

} // namespace OloEngine
