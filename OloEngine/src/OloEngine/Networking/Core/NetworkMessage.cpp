#include "OloEnginePCH.h"
#include "NetworkMessage.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OloEngine
{
    void NetworkMessageDispatcher::RegisterHandler(ENetworkMessageType type, NetworkMessageHandler handler)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_Handlers[type] = std::move(handler);
    }

    void NetworkMessageDispatcher::Dispatch(u32 senderClientID, ENetworkMessageType type, const u8* data,
                                            u32 size) const
    {
        NetworkMessageHandler handler;
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            if (auto const it = m_Handlers.find(type); it != m_Handlers.end())
            {
                handler = it->second;
            }
        }
        if (handler)
        {
            handler(senderClientID, data, size);
        }
        else
        {
            OLO_CORE_WARN("[NetworkMessageDispatcher] No handler for message type {}", static_cast<u16>(type));
        }
    }

    bool NetworkMessageDispatcher::HasHandler(ENetworkMessageType type) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_Handlers.contains(type);
    }
} // namespace OloEngine
