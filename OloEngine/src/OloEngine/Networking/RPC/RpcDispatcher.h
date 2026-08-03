#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/RPC/RpcTypes.h"

#include <vector>

namespace OloEngine
{
    class Scene;

    // Marshalling + authority routing for registered RPCs.
    //
    // Wire format of an ENetworkMessageType::RPC payload:
    //   [rpcId: u32][entityUUID: u64][targetClientID: u32][argCount: u16]
    //   argCount × { [argType: u8][type-specific payload] }
    //
    // The payload carries the id, never the name — the receiver resolves it in its
    // own RpcRegistry, so an unregistered id is dropped rather than guessed at.
    // `targetClientID` is meaningful only for ERpcTarget::Client (server → one
    // client) and is echoed back as 0 in every other direction.
    class RpcDispatcher
    {
      public:
        struct DecodedRpc
        {
            u32 Id = 0;
            u64 EntityUUID = 0;
            u32 TargetClientID = 0;
            RpcArgList Args;
        };

        // Serialize an invocation. Never fails — argument counts and string lengths
        // come from local data.
        [[nodiscard]] static std::vector<u8> Encode(u32 rpcId, u64 entityUUID, u32 targetClientID,
                                                    const RpcArgList& args);

        // Parse a payload received off the wire. Returns false (leaving `out`
        // unspecified) on any truncation, unknown arg type, or a string length that
        // would run past the buffer — every field here is attacker-controlled.
        [[nodiscard]] static bool Decode(const u8* data, u32 size, DecodedRpc& out);

        // Run a decoded call's handler locally, after re-checking the descriptor's
        // authority rules against the direction it arrived from.
        //
        //   receivedOnServer — true when this is the authoritative server handling a
        //                      call from `senderClientID`; false on a client handling
        //                      a call pushed by the server.
        //
        // Returns false (and logs) when the call is refused: unknown id, a target
        // that cannot legally travel in this direction, or — for an entity-bound
        // Server RPC with RequiresOwnership — a sender that does not own the entity.
        static bool ExecuteLocally(Scene* scene, const DecodedRpc& call, u32 senderClientID, bool receivedOnServer);
    };
} // namespace OloEngine
