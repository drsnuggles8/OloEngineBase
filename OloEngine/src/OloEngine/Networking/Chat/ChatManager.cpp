#include "OloEnginePCH.h"
#include "ChatManager.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    u32 ChatManager::CreateChannel(EChatChannelType type, const std::string& name)
    {
        u32 channelID = m_NextChannelID++;
        m_Channels.emplace(channelID, ChatChannel(channelID, type, name));
        OLO_CORE_TRACE("[ChatManager] Created channel '{}' (ID={}, type={})", name, channelID, static_cast<int>(type));
        return channelID;
    }

    void ChatManager::DestroyChannel(u32 channelID)
    {
        m_Channels.erase(channelID);
    }

    ChatChannel* ChatManager::GetChannel(u32 channelID)
    {
        auto it = m_Channels.find(channelID);
        if (it == m_Channels.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    bool ChatManager::JoinChannel(u32 channelID, u32 clientID)
    {
        auto* channel = GetChannel(channelID);
        if (!channel)
        {
            return false;
        }
        channel->Join(clientID);
        return true;
    }

    void ChatManager::LeaveChannel(u32 channelID, u32 clientID)
    {
        auto* channel = GetChannel(channelID);
        if (channel)
        {
            channel->Leave(clientID);
        }
    }

    void ChatManager::RemoveClientFromAllChannels(u32 clientID)
    {
        for (auto& [id, channel] : m_Channels)
        {
            channel.Leave(clientID);
        }
    }

    std::vector<u32> ChatManager::RouteMessage(const ChatMessage& message)
    {
        // Apply message filter
        if (m_Filter && !m_Filter(message))
        {
            return {};
        }

        auto* channel = GetChannel(message.ChannelID);
        if (!channel)
        {
            return {};
        }

        m_TotalMessagesRouted++;

        // Return all subscribers as recipients
        const auto& subscribers = channel->GetSubscribers();
        return { subscribers.begin(), subscribers.end() };
    }

    void ChatManager::SetMessageFilter(MessageFilter filter)
    {
        m_Filter = std::move(filter);
    }

    std::vector<u32> ChatManager::GetChannelsByType(EChatChannelType type) const
    {
        std::vector<u32> result;
        for (auto const& [id, channel] : m_Channels)
        {
            if (channel.GetType() == type)
            {
                result.push_back(id);
            }
        }
        return result;
    }

    u32 ChatManager::GetChannelCount() const
    {
        return static_cast<u32>(m_Channels.size());
    }

    u64 ChatManager::GetTotalMessagesRouted() const
    {
        return m_TotalMessagesRouted;
    }
} // namespace OloEngine
