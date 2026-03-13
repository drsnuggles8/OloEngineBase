#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace OloEngine
{
    // Types of messages that can be sent between zones.
    enum class EInterZoneMessageType : u8
    {
        PlayerHandoff = 0,
        ChatRelay,
        WorldEvent,
        AdminCommand
    };

    // A message passed between zone servers via the inter-zone bus.
    struct InterZoneMessage
    {
        EInterZoneMessageType Type = EInterZoneMessageType::PlayerHandoff;
        u32 SourceZoneID = 0;
        u32 TargetZoneID = 0; // 0 = broadcast to all zones
        std::vector<u8> Payload;
    };

    // Thread-safe message queue for cross-zone communication.
    // Producers push messages from any zone thread; consumers drain them
    // during the main update loop or from the target zone thread.
    class InterZoneMessageBus
    {
      public:
        InterZoneMessageBus() = default;

        // Push a message onto the bus (thread-safe).
        void Push(InterZoneMessage message)
        {
            std::lock_guard lock(m_Mutex);
            m_Queue.push(std::move(message));
        }

        // Drain all pending messages. Returns them in FIFO order.
        [[nodiscard]] std::vector<InterZoneMessage> DrainAll()
        {
            std::lock_guard lock(m_Mutex);
            std::vector<InterZoneMessage> result;
            result.reserve(m_Queue.size());
            while (!m_Queue.empty())
            {
                result.push_back(std::move(m_Queue.front()));
                m_Queue.pop();
            }
            return result;
        }

        // Drain messages targeted at a specific zone (or broadcast messages with targetZoneID=0).
        [[nodiscard]] std::vector<InterZoneMessage> DrainForZone(u32 zoneID)
        {
            std::lock_guard lock(m_Mutex);
            std::vector<InterZoneMessage> result;
            std::queue<InterZoneMessage> remaining;
            while (!m_Queue.empty())
            {
                auto& msg = m_Queue.front();
                if (msg.TargetZoneID == zoneID || msg.TargetZoneID == 0)
                {
                    result.push_back(std::move(msg));
                }
                else
                {
                    remaining.push(std::move(msg));
                }
                m_Queue.pop();
            }
            m_Queue = std::move(remaining);
            return result;
        }

        // Check if the bus has any pending messages.
        [[nodiscard]] bool HasMessages() const
        {
            std::lock_guard lock(m_Mutex);
            return !m_Queue.empty();
        }

        // Get the number of pending messages.
        [[nodiscard]] u32 GetPendingCount() const
        {
            std::lock_guard lock(m_Mutex);
            return static_cast<u32>(m_Queue.size());
        }

      private:
        mutable std::mutex m_Mutex;
        std::queue<InterZoneMessage> m_Queue;
    };
} // namespace OloEngine
