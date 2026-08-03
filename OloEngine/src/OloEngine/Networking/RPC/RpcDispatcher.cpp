#include "OloEnginePCH.h"
#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // A single RPC payload is small by construction (gameplay events, not bulk
        // data). Cap the argument count so a forged header cannot make the decoder
        // reserve an enormous vector before the truncation check catches it.
        constexpr u16 kMaxRpcArgs = 64;

        // Same reasoning for a string argument: std::string's operator<< resizes to
        // the wire length BEFORE reading, so an attacker-supplied 2 GB length would
        // allocate 2 GB even though the read then fails. Decode strings by hand
        // against the remaining buffer instead.
        constexpr i32 kMaxRpcStringLength = 64 * 1024;

        void WriteArg(FArchive& ar, const RpcArg& arg)
        {
            u8 type = static_cast<u8>(arg.Type);
            ar << type;

            switch (arg.Type)
            {
                case ERpcArgType::Bool:
                {
                    u8 v = arg.AsBool ? 1u : 0u;
                    ar << v;
                    break;
                }
                case ERpcArgType::Int:
                {
                    i64 v = arg.AsInt;
                    ar << v;
                    break;
                }
                case ERpcArgType::Float:
                {
                    f64 v = arg.AsFloat;
                    ar << v;
                    break;
                }
                case ERpcArgType::String:
                {
                    // Truncate to the same bound Decode enforces. Sending a longer
                    // string would produce a payload every receiver silently drops,
                    // with nothing logged on the sending side.
                    sizet byteCount = arg.AsString.size();
                    if (byteCount > static_cast<sizet>(kMaxRpcStringLength))
                    {
                        OLO_CORE_WARN_TAG("Networking",
                                          "RPC string argument is {} bytes; truncating to the {} the wire allows",
                                          byteCount, kMaxRpcStringLength);
                        byteCount = static_cast<sizet>(kMaxRpcStringLength);
                    }
                    i32 length = static_cast<i32>(byteCount);
                    ar << length;
                    if (length > 0)
                    {
                        ar.Serialize(const_cast<char*>(arg.AsString.data()), length);
                    }
                    break;
                }
                case ERpcArgType::Vec3:
                {
                    f32 x = arg.AsVec3.x;
                    f32 y = arg.AsVec3.y;
                    f32 z = arg.AsVec3.z;
                    ar << x << y << z;
                    break;
                }
                case ERpcArgType::Entity:
                {
                    u64 v = arg.AsEntity;
                    ar << v;
                    break;
                }
            }
        }

        [[nodiscard]] bool ReadArg(FArchive& ar, RpcArg& out)
        {
            u8 rawType = 0;
            ar << rawType;
            if (ar.IsError())
            {
                return false;
            }

            switch (static_cast<ERpcArgType>(rawType))
            {
                case ERpcArgType::Bool:
                {
                    u8 v = 0;
                    ar << v;
                    out = RpcArg::MakeBool(v != 0);
                    break;
                }
                case ERpcArgType::Int:
                {
                    i64 v = 0;
                    ar << v;
                    out = RpcArg::MakeInt(v);
                    break;
                }
                case ERpcArgType::Float:
                {
                    f64 v = 0.0;
                    ar << v;
                    // Untrusted wire float — a NaN/inf that reaches gameplay code
                    // poisons every downstream computation silently.
                    if (!std::isfinite(v))
                    {
                        v = 0.0;
                    }
                    out = RpcArg::MakeFloat(v);
                    break;
                }
                case ERpcArgType::String:
                {
                    i32 length = 0;
                    ar << length;
                    if (ar.IsError() || length < 0 || length > kMaxRpcStringLength ||
                        ar.Tell() + static_cast<i64>(length) > ar.TotalSize())
                    {
                        return false;
                    }
                    std::string value;
                    value.resize(static_cast<sizet>(length));
                    if (length > 0)
                    {
                        ar.Serialize(value.data(), length);
                    }
                    out = RpcArg::MakeString(std::move(value));
                    break;
                }
                case ERpcArgType::Vec3:
                {
                    f32 x = 0.0f;
                    f32 y = 0.0f;
                    f32 z = 0.0f;
                    ar << x << y << z;
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                    {
                        x = 0.0f;
                        y = 0.0f;
                        z = 0.0f;
                    }
                    out = RpcArg::MakeVec3({ x, y, z });
                    break;
                }
                case ERpcArgType::Entity:
                {
                    u64 v = 0;
                    ar << v;
                    out = RpcArg::MakeEntity(v);
                    break;
                }
                default:
                    // An arg type this build does not know. Unlike the snapshot
                    // format (which length-prefixes each component and can skip an
                    // unknown one), an unknown arg makes the rest of the payload
                    // unparseable — drop the whole call.
                    return false;
            }

            return !ar.IsError();
        }
    } // namespace

    std::vector<u8> RpcDispatcher::Encode(u32 rpcId, u64 entityUUID, u32 targetClientID, const RpcArgList& args)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        // Clamp rather than narrow: a silent u16 truncation of the count would
        // write N arguments and declare N mod 65536, desynchronising the reader for
        // the rest of the payload. Dropping the tail is loud and recoverable.
        sizet argCountRaw = args.size();
        if (argCountRaw > static_cast<sizet>(kMaxRpcArgs))
        {
            OLO_CORE_WARN_TAG("Networking", "RPC {} passed {} arguments; truncating to the {} the wire allows", rpcId,
                              argCountRaw, kMaxRpcArgs);
            argCountRaw = static_cast<sizet>(kMaxRpcArgs);
        }

        u32 id = rpcId;
        u64 uuid = entityUUID;
        u32 target = targetClientID;
        u16 argCount = static_cast<u16>(argCountRaw);
        writer << id << uuid << target << argCount;

        for (u16 i = 0; i < argCount; ++i)
        {
            WriteArg(writer, args[i]);
        }

        return buffer;
    }

    bool RpcDispatcher::Decode(const u8* data, u32 size, DecodedRpc& out)
    {
        OLO_PROFILE_FUNCTION();

        if (data == nullptr || size < sizeof(u32) + sizeof(u64) + sizeof(u32) + sizeof(u16))
        {
            return false;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;

        u32 id = 0;
        u64 uuid = 0;
        u32 target = 0;
        u16 argCount = 0;
        reader << id << uuid << target << argCount;
        if (reader.IsError() || argCount > kMaxRpcArgs)
        {
            return false;
        }

        RpcArgList args;
        args.reserve(argCount);
        for (u16 i = 0; i < argCount; ++i)
        {
            RpcArg arg;
            if (!ReadArg(reader, arg))
            {
                return false;
            }
            args.push_back(std::move(arg));
        }

        out.Id = id;
        out.EntityUUID = uuid;
        out.TargetClientID = target;
        out.Args = std::move(args);
        return true;
    }

    bool RpcDispatcher::ExecuteLocally(Scene* scene, const DecodedRpc& call, u32 senderClientID, bool receivedOnServer)
    {
        OLO_PROFILE_FUNCTION();

        auto descriptor = RpcRegistry::FindById(call.Id);
        if (!descriptor.has_value())
        {
            OLO_CORE_WARN_TAG("Networking", "Dropping RPC with unregistered id {} (from client {})", call.Id,
                              senderClientID);
            return false;
        }

        // Authority routing, re-checked against the direction the call arrived
        // from. The sending side already refuses an illegal invocation; this is the
        // half that a forged payload cannot get around.
        //
        // `senderClientID == 0` on the server means the SERVER originated the call,
        // not that some client did. That distinction is what makes this check safe
        // to apply to the server's own local execution of a Multicast (which is a
        // legitimate "everyone includes me") while still refusing a client that
        // forges the same payload.
        if (receivedOnServer)
        {
            if (senderClientID != 0 && descriptor->Target != ERpcTarget::Server)
            {
                OLO_CORE_WARN_TAG("Networking",
                                  "Client {} tried to push '{}' to the server, but it is a {} RPC — refused",
                                  senderClientID, descriptor->Name,
                                  descriptor->Target == ERpcTarget::Client ? "Client" : "Multicast");
                return false;
            }
        }
        else if (descriptor->Target == ERpcTarget::Server)
        {
            OLO_CORE_WARN_TAG("Networking", "Server pushed Server-target RPC '{}' to a client — refused",
                              descriptor->Name);
            return false;
        }

        // Entity ownership: the same NetworkIdentityComponent rule ServerInputHandler
        // enforces for input commands. Only meaningful server-side — a client has no
        // authority to check anything against.
        //
        // senderClientID == 0 means the SERVER itself originated the call, so there
        // is no client authority to validate. Without that exclusion a server-side
        // InvokeRpc on an entity owned by any client was refused by its own
        // ownership check — the server being unable to act on a player's own pawn.
        if (receivedOnServer && senderClientID != 0 && descriptor->RequiresOwnership && call.EntityUUID != 0)
        {
            if (scene == nullptr)
            {
                OLO_CORE_WARN_TAG("Networking", "Refusing entity-bound RPC '{}' — no active scene to validate against",
                                  descriptor->Name);
                return false;
            }

            auto entityOpt = scene->TryGetEntityWithUUID(UUID(call.EntityUUID));
            if (!entityOpt.has_value())
            {
                OLO_CORE_WARN_TAG("Networking", "Client {} invoked '{}' on unknown entity {}", senderClientID,
                                  descriptor->Name, call.EntityUUID);
                return false;
            }

            Entity entity = *entityOpt;
            if (!entity.HasComponent<NetworkIdentityComponent>())
            {
                OLO_CORE_WARN_TAG("Networking", "Client {} invoked '{}' on entity {} with no NetworkIdentityComponent",
                                  senderClientID, descriptor->Name, call.EntityUUID);
                return false;
            }

            if (auto const& nic = entity.GetComponent<NetworkIdentityComponent>(); nic.OwnerClientID != senderClientID)
            {
                OLO_CORE_WARN_TAG("Networking", "Client {} invoked '{}' on entity {} owned by client {} — refused",
                                  senderClientID, descriptor->Name, call.EntityUUID, nic.OwnerClientID);
                return false;
            }
        }

        if (!descriptor->Handler)
        {
            // Registered for routing but with no local body — legitimate on a
            // dedicated server for a Client-target RPC it only ever forwards.
            return true;
        }

        RpcContext context;
        context.ActiveScene = scene;
        context.SenderClientID = senderClientID;
        context.EntityUUID = call.EntityUUID;
        context.IsServer = receivedOnServer;

        descriptor->Handler(context, call.Args);
        return true;
    }
} // namespace OloEngine
