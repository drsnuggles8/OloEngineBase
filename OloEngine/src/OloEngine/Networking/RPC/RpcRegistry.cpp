#include "OloEnginePCH.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    FMutex RpcRegistry::s_Mutex;
    std::vector<RpcDescriptor> RpcRegistry::s_Descriptors;

    void RpcRegistry::Register(RpcDescriptor descriptor)
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        if (descriptor.Name.empty())
        {
            OLO_CORE_WARN_TAG("Networking", "RpcRegistry::Register ignored an RPC with an empty name");
            return;
        }

        descriptor.Id = HashName(descriptor.Name);

        for (auto& existing : s_Descriptors)
        {
            if (existing.Name == descriptor.Name)
            {
                existing = std::move(descriptor);
                return;
            }
            // Two different names hashing to the same id would make the wire id
            // ambiguous — the receiver resolves by id alone. Refuse the second one
            // loudly rather than silently routing its calls to the first.
            if (existing.Id == descriptor.Id)
            {
                OLO_CORE_ERROR_TAG("Networking",
                                   "RpcRegistry: '{}' collides with already-registered '{}' (id {}); registration refused",
                                   descriptor.Name, existing.Name, descriptor.Id);
                return;
            }
        }

        s_Descriptors.push_back(std::move(descriptor));
    }

    std::optional<RpcDescriptor> RpcRegistry::FindById(u32 id)
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        for (const auto& descriptor : s_Descriptors)
        {
            if (descriptor.Id == id)
            {
                return descriptor;
            }
        }
        return std::nullopt;
    }

    std::optional<RpcDescriptor> RpcRegistry::FindByName(std::string_view name)
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        for (const auto& descriptor : s_Descriptors)
        {
            if (descriptor.Name == name)
            {
                return descriptor;
            }
        }
        return std::nullopt;
    }

    sizet RpcRegistry::Size()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Descriptors.size();
    }

    void RpcRegistry::Clear()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        s_Descriptors.clear();
    }

    void RpcRegistry::ClearOwnedBy(ERpcOwner owner)
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        std::erase_if(s_Descriptors, [owner](const RpcDescriptor& d)
                      { return d.Owner == owner; });
    }
} // namespace OloEngine
