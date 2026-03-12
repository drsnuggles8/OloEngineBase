#include "OloEnginePCH.h"
#include "NetworkServer.h"
#include "OloEngine/Debug/Profiler.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    // Maximum number of messages drained per PollMessages() call
    static constexpr i32 k_MaxIncomingMessages = 128;

    bool NetworkServer::Start(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (IsRunning())
        {
            OLO_CORE_WARN("[NetworkServer] Already running.");
            return true;
        }

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_ERROR("[NetworkServer] SteamNetworkingSockets() returned null.");
            return false;
        }

        SteamNetworkingIPAddr serverLocalAddr;
        serverLocalAddr.Clear();
        serverLocalAddr.m_port = port;

        SteamNetworkingConfigValue_t opt;
        opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        m_ListenSocket = m_Interface->CreateListenSocketIP(serverLocalAddr, 1, &opt);
        if (m_ListenSocket == k_HSteamListenSocket_Invalid)
        {
            OLO_CORE_ERROR("[NetworkServer] Failed to create listen socket on port {}.", port);
            return false;
        }

        m_PollGroup = m_Interface->CreatePollGroup();
        if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            OLO_CORE_ERROR("[NetworkServer] Failed to create poll group.");
            m_Interface->CloseListenSocket(m_ListenSocket);
            m_ListenSocket = k_HSteamListenSocket_Invalid;
            return false;
        }

        OLO_CORE_TRACE("[NetworkServer] Listening on port {}.", port);
        return true;
    }

    void NetworkServer::Stop()
    {
        OLO_PROFILE_FUNCTION();

        if (!IsRunning())
        {
            return;
        }

        OLO_CORE_TRACE("[NetworkServer] Stopping.");

        // Close all client connections
        for (auto& [handle, conn] : m_Connections)
        {
            conn.Close(0, "Server shutting down");
        }
        m_Connections.clear();

        if (m_Interface)
        {
            if (m_PollGroup != k_HSteamNetPollGroup_Invalid)
            {
                m_Interface->DestroyPollGroup(m_PollGroup);
                m_PollGroup = k_HSteamNetPollGroup_Invalid;
            }
            m_Interface->CloseListenSocket(m_ListenSocket);
        }

        m_ListenSocket = k_HSteamListenSocket_Invalid;
        m_Interface    = nullptr;
    }

    void NetworkServer::PollMessages()
    {
        OLO_PROFILE_FUNCTION();

        if (!IsRunning())
        {
            return;
        }

        ISteamNetworkingMessage* pIncomingMsg[k_MaxIncomingMessages];
        i32 numMsgs = m_Interface->ReceiveMessagesOnPollGroup(
            m_PollGroup, pIncomingMsg, k_MaxIncomingMessages);

        for (i32 i = 0; i < numMsgs; ++i)
        {
            ISteamNetworkingMessage* pMsg = pIncomingMsg[i];

            // TODO: dispatch to message handler based on NetworkMessageHeader
            OLO_CORE_TRACE("[NetworkServer] Received {} bytes from connection {}.",
                           pMsg->m_cbSize, static_cast<u32>(pMsg->m_conn));

            pMsg->Release();
        }
    }

    void NetworkServer::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(pInfo, "Status change callback received null info");

        switch (pInfo->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_None:
                break;

            case k_ESteamNetworkingConnectionState_Connecting:
            {
                // Accept the incoming connection
                if (m_Interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
                {
                    OLO_CORE_WARN("[NetworkServer] Failed to accept connection {}.",
                                  static_cast<u32>(pInfo->m_hConn));
                    m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    break;
                }

                if (!m_Interface->SetConnectionPollGroup(pInfo->m_hConn, m_PollGroup))
                {
                    OLO_CORE_WARN("[NetworkServer] Failed to add connection to poll group.");
                    m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    break;
                }

                u32 clientID = m_NextClientID++;
                auto [it, ok] = m_Connections.emplace(
                    pInfo->m_hConn, NetworkConnection(pInfo->m_hConn, clientID));
                if (ok)
                {
                    it->second.SetState(EConnectionState::Connecting);
                }

                OLO_CORE_TRACE("[NetworkServer] Client {} connecting (handle {}).",
                               clientID, static_cast<u32>(pInfo->m_hConn));
                break;
            }

            case k_ESteamNetworkingConnectionState_Connected:
            {
                auto it = m_Connections.find(pInfo->m_hConn);
                if (it != m_Connections.end())
                {
                    it->second.SetState(EConnectionState::Connected);
                    OLO_CORE_TRACE("[NetworkServer] Client {} connected.",
                                   it->second.GetClientID());
                }
                break;
            }

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            {
                auto it = m_Connections.find(pInfo->m_hConn);
                if (it != m_Connections.end())
                {
                    OLO_CORE_TRACE("[NetworkServer] Client {} disconnected (reason: {}).",
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

} // namespace OloEngine
