#include "OloEnginePCH.h"
#include "OloEngine/Networking/Transport/NetworkConnection.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    NetworkConnection::NetworkConnection(HSteamNetConnection handle)
        : m_Handle(handle)
    {
    }

    NetworkConnection::~NetworkConnection()
    {
        if (m_State != EConnectionState::None && m_Handle != k_HSteamNetConnection_Invalid)
        {
            Close();
        }
    }

    NetworkConnection::NetworkConnection(NetworkConnection&& other) noexcept
        : m_Handle(other.m_Handle), m_State(other.m_State), m_ClientID(other.m_ClientID)
    {
        other.m_Handle = k_HSteamNetConnection_Invalid;
        other.m_State = EConnectionState::None;
        other.m_ClientID = 0;
    }

    NetworkConnection& NetworkConnection::operator=(NetworkConnection&& other) noexcept
    {
        if (this != &other)
        {
            if (m_State != EConnectionState::None && m_Handle != k_HSteamNetConnection_Invalid)
            {
                Close();
            }
            m_Handle = other.m_Handle;
            m_State = other.m_State;
            m_ClientID = other.m_ClientID;
            other.m_Handle = k_HSteamNetConnection_Invalid;
            other.m_State = EConnectionState::None;
            other.m_ClientID = 0;
        }
        return *this;
    }

    HSteamNetConnection NetworkConnection::GetHandle() const
    {
        return m_Handle;
    }

    EConnectionState NetworkConnection::GetState() const
    {
        return m_State;
    }

    u32 NetworkConnection::GetClientID() const
    {
        return m_ClientID;
    }

    void NetworkConnection::SetState(EConnectionState state)
    {
        m_State = state;
    }

    void NetworkConnection::SetClientID(u32 clientID)
    {
        m_ClientID = clientID;
    }

    bool NetworkConnection::Send(const void* data, u32 size, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (!pInterface)
        {
            return false;
        }

        EResult result = pInterface->SendMessageToConnection(
            m_Handle, data, size, sendFlags, nullptr);

        return result == k_EResultOK;
    }

    void NetworkConnection::Close(i32 reason, const char* debug)
    {
        OLO_PROFILE_FUNCTION();

        ISteamNetworkingSockets* pInterface = SteamNetworkingSockets();
        if (pInterface)
        {
            pInterface->CloseConnection(m_Handle, reason, debug, false);
        }

        m_State = EConnectionState::None;
    }
} // namespace OloEngine
