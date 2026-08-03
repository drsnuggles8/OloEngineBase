#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/RPC/RpcTypes.h"
#include "OloEngine/Threading/Mutex.h"

#include <optional>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // Process-wide registry of remote procedure calls.
    //
    // Both ends of a connection must register the same Name with the same Target —
    // the wire payload carries only the FNV-1a-32 id, so a receiver that never
    // registered the name drops the call rather than guessing at it. That is also
    // what makes the registry the authority boundary: a client can forge any
    // payload it likes, but it cannot change the server's copy of the descriptor
    // that decides whether the call is legal.
    class RpcRegistry
    {
      public:
        // Register (or replace) an RPC by name. `descriptor.Id` is derived from the
        // name and any caller-supplied value is overwritten. Re-registering an
        // existing name replaces its handler — hot-reloading a script must be able
        // to rebind its RPCs without leaking the previous closure.
        static void Register(RpcDescriptor descriptor);

        // Lookups return a COPY, not a pointer into the registry. Scripts register
        // RPCs at runtime, so a borrowed pointer could dangle the moment another
        // registration reallocated the backing vector — and a dispatcher holds the
        // descriptor across the whole (re-entrant) handler call.
        [[nodiscard]] static std::optional<RpcDescriptor> FindById(u32 id);
        [[nodiscard]] static std::optional<RpcDescriptor> FindByName(std::string_view name);

        [[nodiscard]] static sizet Size();

        // Drop every registration. For test isolation.
        static void Clear();

        // Drop only the registrations owned by ONE VM. Called when that engine shuts
        // down (or reloads its assembly): a handler capturing a
        // sol::protected_function — or one that calls into a Mono app domain —
        // outlives its VM otherwise, and invoking it after teardown is a crash
        // rather than a no-op.
        //
        // Deliberately per-owner: a single "script-owned" flag made tearing down the
        // Lua state silently unregister every live C# RPC, and a C# assembly reload
        // do the same to Lua's.
        static void ClearOwnedBy(ERpcOwner owner);

        // FNV-1a-32 of an RPC name → its stable wire id. Matches
        // ComponentInterpolationRegistry::HashName so the two id spaces are
        // computed identically (they are separate namespaces — an id collision
        // ACROSS the two registries is meaningless and harmless).
        [[nodiscard]] static constexpr u32 HashName(std::string_view name) noexcept
        {
            u32 hash = 2166136261u;
            for (const char c : name)
            {
                hash ^= static_cast<u8>(c);
                hash *= 16777619u;
            }
            return hash;
        }

      private:
        static FMutex s_Mutex;
        static std::vector<RpcDescriptor> s_Descriptors;
    };
} // namespace OloEngine
