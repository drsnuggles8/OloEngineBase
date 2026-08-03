#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    class FArchive;
    class Scene;

    // Where a registered RPC is allowed to run, and therefore who may invoke it.
    //
    //   Server    — client → server. The server executes it against the
    //               authoritative simulation. A server that invokes it locally
    //               simply runs the handler.
    //   Client    — server → ONE client (TargetClientID). A client that tries to
    //               invoke it is an authority violation and is refused.
    //   Multicast — server → every client, and also locally on the server. A
    //               client that tries to invoke it is refused.
    //
    // The refusal is deliberately at BOTH ends: the invoking side never puts an
    // illegal call on the wire, and the receiving side re-checks the descriptor's
    // target against the direction it arrived from. A client can forge a payload;
    // it cannot forge the server's own copy of the registry.
    enum class ERpcTarget : u8
    {
        Server = 0,
        Client,
        Multicast
    };

    // Reliability the dispatcher requests from the transport. Reliable RPCs are
    // ordered and guaranteed (gameplay events); unreliable ones are fire-and-forget
    // (cosmetic / high-frequency effects that a later call supersedes anyway).
    enum class ERpcReliability : u8
    {
        Reliable = 0,
        Unreliable
    };

    // The argument types the RPC marshaller can carry over FArchive. Deliberately
    // small: every one has an unambiguous representation in C++, Lua and C#, so a
    // signature never means different things on the two sides of the wire.
    enum class ERpcArgType : u8
    {
        Bool = 0,
        Int,    // i64 — Lua integers, C# long/int
        Float,  // f64 — Lua numbers, C# double/float
        String, // length-prefixed UTF-8
        Vec3,   // 3 × f32
        Entity  // u64 UUID
    };

    // One marshalled RPC argument. A tagged value rather than a variant so the
    // wire encoding, the Lua conversion and the Mono boxing all read the same way.
    struct RpcArg
    {
        ERpcArgType Type = ERpcArgType::Bool;
        bool AsBool = false;
        i64 AsInt = 0;
        f64 AsFloat = 0.0;
        std::string AsString;
        glm::vec3 AsVec3{ 0.0f };
        u64 AsEntity = 0;

        [[nodiscard]] static RpcArg MakeBool(bool v)
        {
            RpcArg a;
            a.Type = ERpcArgType::Bool;
            a.AsBool = v;
            return a;
        }
        [[nodiscard]] static RpcArg MakeInt(i64 v)
        {
            RpcArg a;
            a.Type = ERpcArgType::Int;
            a.AsInt = v;
            return a;
        }
        [[nodiscard]] static RpcArg MakeFloat(f64 v)
        {
            RpcArg a;
            a.Type = ERpcArgType::Float;
            a.AsFloat = v;
            return a;
        }
        [[nodiscard]] static RpcArg MakeString(std::string v)
        {
            RpcArg a;
            a.Type = ERpcArgType::String;
            a.AsString = std::move(v);
            return a;
        }
        [[nodiscard]] static RpcArg MakeVec3(const glm::vec3& v)
        {
            RpcArg a;
            a.Type = ERpcArgType::Vec3;
            a.AsVec3 = v;
            return a;
        }
        [[nodiscard]] static RpcArg MakeEntity(u64 v)
        {
            RpcArg a;
            a.Type = ERpcArgType::Entity;
            a.AsEntity = v;
            return a;
        }
    };

    using RpcArgList = std::vector<RpcArg>;

    // Everything a handler needs to know about *this* invocation. Scene is never
    // null when the dispatcher runs a handler (the dispatcher refuses to run
    // without an active scene) — a handler may still be registered before one
    // exists, which is why the pointer form is kept.
    struct RpcContext
    {
        Scene* ActiveScene = nullptr;
        // The client that sent this call, or 0 when the server invoked it locally
        // (Multicast) or the call arrived from the server on a client.
        u32 SenderClientID = 0;
        // The entity the call is bound to, or 0 for a global RPC.
        u64 EntityUUID = 0;
        // True when this handler is running on the authoritative server.
        bool IsServer = false;
    };

    using RpcHandler = std::function<void(const RpcContext&, const RpcArgList&)>;

    // A registered RPC. Both ends of the wire must have registered the same Name
    // (the Id is derived from it), because the payload carries only the Id — a
    // name the receiver never registered is dropped, not guessed at.
    struct RpcDescriptor
    {
        u32 Id = 0; // FNV-1a-32 of Name — filled in by RpcRegistry::Register
        std::string Name;
        ERpcTarget Target = ERpcTarget::Server;
        ERpcReliability Reliability = ERpcReliability::Reliable;

        // Server-target RPCs only: when true (the default) and the call carries a
        // non-zero EntityUUID, the server refuses it unless the sending client owns
        // that entity — the same NetworkIdentityComponent ownership rule
        // ServerInputHandler applies to input commands. Set false for entity-bound
        // calls a non-owner is legitimately allowed to make (e.g. "interact with
        // that door").
        bool RequiresOwnership = true;

        RpcHandler Handler;

        // Handlers owned by a script VM are dropped when that VM shuts down —
        // a std::function holding a sol::protected_function outlives its
        // sol::state otherwise, and calling it after teardown is a crash.
        bool ScriptOwned = false;
    };
} // namespace OloEngine
