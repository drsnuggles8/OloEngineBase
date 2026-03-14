#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    enum class EChatChannelType : u8
    {
        Zone = 0,
        Global,
        Party,
        Guild,
        Whisper,
        System,
        Trade,
        Custom
    };

    struct ChatMessage
    {
        u32 SenderClientID = 0;
        std::string SenderName;
        EChatChannelType Type = EChatChannelType::Zone;
        u32 ChannelID = 0;
        std::string Content;
        u64 Timestamp = 0;
    };

    // A single chat channel with subscribers.
    class ChatChannel
    {
      public:
        ChatChannel() = default;
        ChatChannel(u32 channelID, EChatChannelType type, const std::string& name);

        // Subscribe a client to this channel.
        void Join(u32 clientID);

        // Unsubscribe a client from this channel.
        void Leave(u32 clientID);

        // Check if a client is subscribed.
        [[nodiscard]] bool HasSubscriber(u32 clientID) const;

        // Get all subscribers.
        [[nodiscard]] const std::unordered_set<u32>& GetSubscribers() const;

        // Get subscriber count.
        [[nodiscard]] u32 GetSubscriberCount() const;

        // Get channel info.
        [[nodiscard]] u32 GetID() const;
        [[nodiscard]] EChatChannelType GetType() const;
        [[nodiscard]] const std::string& GetName() const;

      private:
        u32 m_ChannelID = 0;
        EChatChannelType m_Type = EChatChannelType::Zone;
        std::string m_Name;
        std::unordered_set<u32> m_Subscribers;
    };
} // namespace OloEngine
