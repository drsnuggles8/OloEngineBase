#include "OloEnginePCH.h"
#include "ChatChannel.h"

namespace OloEngine
{
    ChatChannel::ChatChannel(u32 channelID, EChatChannelType type, const std::string& name)
        : m_ChannelID(channelID)
        , m_Type(type)
        , m_Name(name)
    {
    }

    void ChatChannel::Join(u32 clientID)
    {
        m_Subscribers.insert(clientID);
    }

    void ChatChannel::Leave(u32 clientID)
    {
        m_Subscribers.erase(clientID);
    }

    bool ChatChannel::HasSubscriber(u32 clientID) const
    {
        return m_Subscribers.contains(clientID);
    }

    const std::unordered_set<u32>& ChatChannel::GetSubscribers() const
    {
        return m_Subscribers;
    }

    u32 ChatChannel::GetSubscriberCount() const
    {
        return static_cast<u32>(m_Subscribers.size());
    }

    u32 ChatChannel::GetID() const
    {
        return m_ChannelID;
    }

    EChatChannelType ChatChannel::GetType() const
    {
        return m_Type;
    }

    const std::string& ChatChannel::GetName() const
    {
        return m_Name;
    }
} // namespace OloEngine
