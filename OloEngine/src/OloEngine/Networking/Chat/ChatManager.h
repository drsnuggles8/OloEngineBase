#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Chat/ChatChannel.h"
#include "OloEngine/Threading/Mutex.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    using MessageFilter = std::function<bool(const ChatMessage&)>;

    // Central chat coordinator. Owns channels, routes messages, and handles cross-zone messaging.
    class ChatManager
    {
      public:
        ChatManager() = default;

        // Create a new channel. Returns the channel ID.
        u32 CreateChannel(EChatChannelType type, const std::string& name);

        // Destroy a channel.
        void DestroyChannel(u32 channelID);

        // Get a channel by ID.
        [[nodiscard]] ChatChannel* GetChannel(u32 channelID);

        // Join a client to a channel.
        bool JoinChannel(u32 channelID, u32 clientID);

        // Remove a client from a channel.
        void LeaveChannel(u32 channelID, u32 clientID);

        // Remove a client from all channels.
        void RemoveClientFromAllChannels(u32 clientID);

        // Send a message to a channel. Returns list of recipients.
        std::vector<u32> RouteMessage(const ChatMessage& message);

        // Set a message filter. Return false from the filter to block a message.
        void SetMessageFilter(MessageFilter filter);

        // Get all channels of a specific type.
        [[nodiscard]] std::vector<u32> GetChannelsByType(EChatChannelType type) const;

        // Get total number of active channels.
        [[nodiscard]] u32 GetChannelCount() const;

        // Get total messages routed (for debug stats).
        [[nodiscard]] u64 GetTotalMessagesRouted() const;

      private:
        std::unordered_map<u32, ChatChannel> m_Channels;
        MessageFilter m_Filter;
        u32 m_NextChannelID = 1;
        u64 m_TotalMessagesRouted = 0;
        mutable FMutex m_Mutex;
    };
} // namespace OloEngine
