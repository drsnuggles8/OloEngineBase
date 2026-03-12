#include "OloEnginePCH.h"
#include "NetworkClient.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    static constexpr i32 k_MaxClientIncomingMessages = 64;

    bool NetworkClient::Connect(const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Connection != k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_WARN("[NetworkClient] Already connected or connecting.");
            return false;
        }

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_ERROR("[NetworkClient] SteamNetworkingSockets() returned null.");
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        serverAddr.ParseString(address.c_str());
        serverAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        m_Connection = m_Interface->ConnectByIPAddress(serverAddr, 1, &opt);
        if (m_Connection == k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_ERROR("[NetworkClient] Failed to create connection to {}:{}.", address, port);
            m_Interface = nullptr;
            return false;
        }

        m_State = EConnectionState::Connecting;
        OLO_CORE_TRACE("[NetworkClient] Connecting to {}:{}.", address, port);
        return true;
    }

    void NetworkClient::Disconnect()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Connection == k_HSteamNetConnection_Invalid)
        {
            return;
        }

        if (m_Interface)
        {
            m_Interface->CloseConnection(m_Connection, 0, "Client disconnecting", false);
        }

        m_Connection = k_HSteamNetConnection_Invalid;
        m_Interface  = nullptr;
        m_State      = EConnectionState::None;

        OLO_CORE_TRACE("[NetworkClient] Disconnected.");
    }

    void NetworkClient::PollMessages()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Connection == k_HSteamNetConnection_Invalid || !m_Interface)
        {
            return;
        }

        ISteamNetworkingMessage* pIncomingMsg[k_MaxClientIncomingMessages];
        i32 numMsgs = m_Interface->ReceiveMessagesOnConnection(
            m_Connection, pIncomingMsg, k_MaxClientIncomingMessages);

        for (i32 i = 0; i < numMsgs; ++i)
        {
            ISteamNetworkingMessage* pMsg = pIncomingMsg[i];

            // TODO: dispatch to message handler based on NetworkMessageHeader
            OLO_CORE_TRACE("[NetworkClient] Received {} bytes from server.", pMsg->m_cbSize);

            pMsg->Release();
        }
    }

    bool NetworkClient::Send(const void* data, u32 size, i32 sendFlags) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Connection == k_HSteamNetConnection_Invalid || !m_Interface)
        {
            OLO_CORE_WARN("[NetworkClient] Send called while not connected.");
            return false;
        }

        EResult result = m_Interface->SendMessageToConnection(
            m_Connection, data, size, sendFlags, nullptr);

        if (result != k_EResultOK)
        {
            OLO_CORE_WARN("[NetworkClient] SendMessageToConnection failed with result {}.",
                          static_cast<i32>(result));
            return false;
        }

        return true;
    }

    void NetworkClient::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        if (pInfo->m_hConn != m_Connection)
        {
            return;
        }

        switch (pInfo->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_Connected:
                m_State = EConnectionState::Connected;
                OLO_CORE_TRACE("[NetworkClient] Connected to server.");
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
                m_State = EConnectionState::ClosedByPeer;
                OLO_CORE_TRACE("[NetworkClient] Connection closed by peer: {}.",
                               pInfo->m_info.m_szEndDebug);
                m_Connection = k_HSteamNetConnection_Invalid;
                m_Interface  = nullptr;
                break;

            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                m_State = EConnectionState::ProblemDetectedLocally;
                OLO_CORE_WARN("[NetworkClient] Connection problem: {}.",
                              pInfo->m_info.m_szEndDebug);
                m_Connection = k_HSteamNetConnection_Invalid;
                m_Interface  = nullptr;
                break;

            case k_ESteamNetworkingConnectionState_FindingRoute:
                m_State = EConnectionState::FindingRoute;
                break;

            case k_ESteamNetworkingConnectionState_Connecting:
                m_State = EConnectionState::Connecting;
                break;

            default:
                break;
        }
    }

} // namespace OloEngine
