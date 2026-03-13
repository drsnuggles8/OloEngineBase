#include "OloEnginePCH.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    bool NetworkServer::Start(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_ERROR("[NetworkServer] SteamNetworkingSockets interface not available");
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        serverAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   reinterpret_cast<void*>(nullptr)); // Callback handled via NetworkManager

        m_ListenSocket = m_Interface->CreateListenSocketIP(serverAddr, 0, nullptr);
        if (m_ListenSocket == k_HSteamListenSocket_Invalid)
        {
            OLO_CORE_ERROR("[NetworkServer] Failed to create listen socket on port {}", port);
            return false;
        }

        m_PollGroup = m_Interface->CreatePollGroup();
        if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            OLO_CORE_ERROR("[NetworkServer] Failed to create poll group");
            m_Interface->CloseListenSocket(m_ListenSocket);
            m_ListenSocket = k_HSteamListenSocket_Invalid;
            return false;
        }

        OLO_CORE_INFO("[NetworkServer] Listening on port {}", port);
        return true;
    }

    void NetworkServer::Stop()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Interface)
        {
            return;
        }

        // Close all connections
        for (auto& [handle, connection] : m_Connections)
        {
            connection.Close(0, "Server shutting down");
        }
        m_Connections.clear();

        if (m_PollGroup != k_HSteamNetPollGroup_Invalid)
        {
            m_Interface->DestroyPollGroup(m_PollGroup);
            m_PollGroup = k_HSteamNetPollGroup_Invalid;
        }

        if (m_ListenSocket != k_HSteamListenSocket_Invalid)
        {
            m_Interface->CloseListenSocket(m_ListenSocket);
            m_ListenSocket = k_HSteamListenSocket_Invalid;
        }

        OLO_CORE_INFO("[NetworkServer] Stopped");
    }

    void NetworkServer::PollMessages()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Interface || m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            return;
        }

        ISteamNetworkingMessage* pIncomingMsg = nullptr;
        i32 numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
        while (numMsgs > 0)
        {
            // Process message - for now just release it
            // Future: deserialize NetworkMessageHeader and dispatch
            pIncomingMsg->Release();

            numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
        }
    }

    bool NetworkServer::IsRunning() const
    {
        return m_ListenSocket != k_HSteamListenSocket_Invalid;
    }

    const std::unordered_map<HSteamNetConnection, NetworkConnection>& NetworkServer::GetConnections() const
    {
        return m_Connections;
    }

    void NetworkServer::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        switch (pInfo->m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_Connecting:
        {
            if (m_Interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
            {
                m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                OLO_CORE_WARN("[NetworkServer] Failed to accept connection");
                break;
            }

            if (!m_Interface->SetConnectionPollGroup(pInfo->m_hConn, m_PollGroup))
            {
                m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                OLO_CORE_WARN("[NetworkServer] Failed to assign connection to poll group");
                break;
            }

            NetworkConnection conn(pInfo->m_hConn);
            conn.SetState(EConnectionState::Connecting);
            conn.SetClientID(m_NextClientID++);
            m_Connections.emplace(pInfo->m_hConn, conn);

            OLO_CORE_INFO("[NetworkServer] Client connecting (ID: {})", conn.GetClientID());
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected:
        {
            if (auto it = m_Connections.find(pInfo->m_hConn); it != m_Connections.end())
            {
                it->second.SetState(EConnectionState::Connected);
                OLO_CORE_INFO("[NetworkServer] Client connected (ID: {})", it->second.GetClientID());
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            if (auto it = m_Connections.find(pInfo->m_hConn); it != m_Connections.end())
            {
                OLO_CORE_INFO("[NetworkServer] Client disconnected (ID: {}, reason: {})",
                             it->second.GetClientID(), pInfo->m_info.m_szEndDebug);
                m_Connections.erase(it);
            }

            m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
            break;
        }

        default:
            break;
        }
    }
}
