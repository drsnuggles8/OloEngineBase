#include "OloEnginePCH.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    bool NetworkClient::Connect(const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_ERROR("[NetworkClient] SteamNetworkingSockets interface not available");
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        if (!serverAddr.ParseString(address.c_str()))
        {
            OLO_CORE_ERROR("[NetworkClient] Failed to parse address: {}", address);
            return false;
        }
        serverAddr.m_port = port;

        m_Connection = m_Interface->ConnectByIPAddress(serverAddr, 0, nullptr);
        if (m_Connection == k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_ERROR("[NetworkClient] Failed to connect to {}:{}", address, port);
            return false;
        }

        m_State = EConnectionState::Connecting;
        OLO_CORE_INFO("[NetworkClient] Connecting to {}:{}", address, port);
        return true;
    }

    void NetworkClient::Disconnect()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Interface && m_Connection != k_HSteamNetConnection_Invalid)
        {
            m_Interface->CloseConnection(m_Connection, 0, "Client disconnect", false);
            m_Connection = k_HSteamNetConnection_Invalid;
        }

        m_State = EConnectionState::None;
    }

    void NetworkClient::PollMessages()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Interface || m_Connection == k_HSteamNetConnection_Invalid)
        {
            return;
        }

        ISteamNetworkingMessage* pIncomingMsg = nullptr;
        i32 numMsgs = m_Interface->ReceiveMessagesOnConnection(m_Connection, &pIncomingMsg, 1);
        while (numMsgs > 0)
        {
            // Process message - for now just release it
            // Future: deserialize NetworkMessageHeader and dispatch
            pIncomingMsg->Release();

            numMsgs = m_Interface->ReceiveMessagesOnConnection(m_Connection, &pIncomingMsg, 1);
        }
    }

    bool NetworkClient::IsConnected() const
    {
        return m_State == EConnectionState::Connected;
    }

    EConnectionState NetworkClient::GetState() const
    {
        return m_State;
    }

    HSteamNetConnection NetworkClient::GetConnectionHandle() const
    {
        return m_Connection;
    }

    bool NetworkClient::Send(const void* data, u32 size, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Interface || m_Connection == k_HSteamNetConnection_Invalid)
        {
            return false;
        }

        EResult result = m_Interface->SendMessageToConnection(
            m_Connection, data, size, sendFlags, nullptr);

        return result == k_EResultOK;
    }

    void NetworkClient::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        switch (pInfo->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_Connected:
                m_State = EConnectionState::Connected;
                OLO_CORE_INFO("[NetworkClient] Connected to server");
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                m_State = EConnectionState::ClosedByPeer;
                OLO_CORE_INFO("[NetworkClient] Disconnected by server: {}", pInfo->m_info.m_szEndDebug);
                m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                m_Connection = k_HSteamNetConnection_Invalid;
                break;

            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                m_State = EConnectionState::ProblemDetectedLocally;
                OLO_CORE_WARN("[NetworkClient] Connection problem: {}", pInfo->m_info.m_szEndDebug);
                m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                m_Connection = k_HSteamNetConnection_Invalid;
                break;

            default:
                break;
        }
    }
} // namespace OloEngine
