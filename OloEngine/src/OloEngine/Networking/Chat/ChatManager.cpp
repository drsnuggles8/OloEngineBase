#include "OloEnginePCH.h"
#include "ChatManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OloEngine
{
    u32 ChatManager::CreateChannel(EChatChannelType type, const std::string& name)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        u32 channelID = m_NextChannelID++;
        m_Channels.emplace(channelID, ChatChannel(channelID, type, name));
        OLO_CORE_TRACE("[ChatManager] Created channel '{}' (ID={}, type={})", name, channelID, static_cast<int>(type));
        return channelID;
    }

    void ChatManager::DestroyChannel(u32 channelID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_Channels.erase(channelID);
    }

    ChatChannel* ChatManager::GetChannel(u32 channelID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        auto it = m_Channels.find(channelID);
        if (it == m_Channels.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    bool ChatManager::JoinChannel(u32 channelID, u32 clientID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        auto it = m_Channels.find(channelID);
        if (it == m_Channels.end())
        {
            return false;
        }
        it->second.Join(clientID);
        return true;
    }

    void ChatManager::LeaveChannel(u32 channelID, u32 clientID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        auto it = m_Channels.find(channelID);
        if (it != m_Channels.end())
        {
            it->second.Leave(clientID);
        }
    }

    void ChatManager::RemoveClientFromAllChannels(u32 clientID)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        for (auto& [id, channel] : m_Channels)
        {
            channel.Leave(clientID);
        }
    }

    std::vector<u32> ChatManager::RouteMessage(const ChatMessage& message)
    {
        TUniqueLock<FMutex> lock(m_Mutex);

        // Apply message filter
        if (m_Filter && !m_Filter(message))
        {
            return {};
        }

        auto it = m_Channels.find(message.ChannelID);
        if (it == m_Channels.end())
        {
            return {};
        }

        m_TotalMessagesRouted++;

        // Return all subscribers as recipients
        const auto& subscribers = it->second.GetSubscribers();
        return { subscribers.begin(), subscribers.end() };
    }

    void ChatManager::SetMessageFilter(MessageFilter filter)
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_Filter = std::move(filter);
    }

    std::vector<u32> ChatManager::GetChannelsByType(EChatChannelType type) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
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
        TUniqueLock<FMutex> lock(m_Mutex);
        return static_cast<u32>(m_Channels.size());
    }

    u64 ChatManager::GetTotalMessagesRouted() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_TotalMessagesRouted;
    }
} // namespace OloEngine
