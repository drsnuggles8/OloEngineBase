#include "OloEnginePCH.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    static std::vector<u8> BuildMessageBuffer(ENetworkMessageType type, const u8* payload, u32 payloadSize)
    {
        OLO_PROFILE_FUNCTION();
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        NetworkMessageHeader header;
        header.Type = type;
        header.Size = payloadSize;
        writer << header.Type;
        writer << header.Size;
        writer << header.Flags;
        writer << header.Version;

        if (payload && payloadSize > 0)
        {
            buffer.insert(buffer.end(), payload, payload + payloadSize);
        }

        return buffer;
    }

    bool NetworkServer::Start(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_ERROR("[NetworkServer] SteamNetworkingSockets interface not available");
            return false;
        }

        SteamNetworkingIPAddr serverAddr;
        serverAddr.Clear();
        serverAddr.m_port = port;

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

        TUniqueLock<FMutex> lock(m_Mutex);

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

        // Collect messages under lock, then dispatch outside to avoid deadlock
        // if callbacks call back into the server (e.g., SendTo, Broadcast).
        struct ReceivedMessage
        {
            u32 SenderClientID = 0;
            ENetworkMessageType Type = ENetworkMessageType::None;
            std::vector<u8> Payload;
        };
        std::vector<ReceivedMessage> pendingMessages;
        std::vector<std::pair<HSteamNetConnection, std::vector<u8>>> pendingPings;

        {
            TUniqueLock<FMutex> lock(m_Mutex);

            if (!m_Interface || m_PollGroup == k_HSteamNetPollGroup_Invalid)
            {
                return;
            }

            ISteamNetworkingMessage* pIncomingMsg = nullptr;
            i32 numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
            while (numMsgs > 0)
            {
                u32 msgSize = pIncomingMsg->m_cbSize;
                m_Stats.RecordReceive(msgSize);

                if (msgSize >= NetworkMessageHeader::kSerializedSize)
                {
                    const auto* rawData = static_cast<const u8*>(pIncomingMsg->m_pData);
                    FMemoryReader reader(rawData, static_cast<i64>(msgSize));
                    reader.ArIsNetArchive = true;

                    NetworkMessageHeader header;
                    reader << header.Type;
                    reader << header.Size;
                    reader << header.Flags;
                    reader << header.Version;

                    if (!reader.IsError())
                    {
                        u32 payloadOffset = static_cast<u32>(reader.Tell());
                        const u8* payload = rawData + payloadOffset;
                        u32 payloadSize = msgSize - payloadOffset;

                        // Resolve the sender's client ID from the connection handle
                        u32 senderClientID = 0;
                        if (auto it = m_Connections.find(pIncomingMsg->m_conn); it != m_Connections.end())
                        {
                            senderClientID = it->second.GetClientID();
                        }

                        if (header.Type == ENetworkMessageType::Ping)
                        {
                            pendingPings.push_back(
                                { pIncomingMsg->m_conn, std::vector<u8>(payload, payload + payloadSize) });
                        }
                        else
                        {
                            pendingMessages.push_back(
                                { senderClientID, header.Type, std::vector<u8>(payload, payload + payloadSize) });
                        }
                    }
                }

                pIncomingMsg->Release();
                numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
            }
        }

        // Dispatch outside lock
        for (auto& [conn, data] : pendingPings)
        {
            HandlePing(conn, data.data(), static_cast<u32>(data.size()));
        }
        for (auto& msg : pendingMessages)
        {
            m_Dispatcher.Dispatch(msg.SenderClientID, msg.Type, msg.Payload.data(), static_cast<u32>(msg.Payload.size()));
        }
    }

    bool NetworkServer::SendTo(HSteamNetConnection connection, const void* data, u32 size, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Interface)
        {
            return false;
        }

        EResult result = m_Interface->SendMessageToConnection(connection, data, size, sendFlags, nullptr);
        if (result == k_EResultOK)
        {
            m_Stats.RecordSend(size);
            return true;
        }
        return false;
    }

    void NetworkServer::Broadcast(const void* data, u32 size, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        // Collect connected handles under lock, then send outside to avoid
        // deadlock (SendTo also acquires m_Mutex).
        std::vector<HSteamNetConnection> connected;
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            for (auto& [handle, connection] : m_Connections)
            {
                if (connection.GetState() == EConnectionState::Connected)
                {
                    connected.push_back(handle);
                }
            }
        }
        for (HSteamNetConnection handle : connected)
        {
            SendTo(handle, data, size, sendFlags);
        }
    }

    bool NetworkServer::SendMessage(HSteamNetConnection connection, ENetworkMessageType type, const u8* payload,
                                    u32 payloadSize, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        auto buffer = BuildMessageBuffer(type, payload, payloadSize);
        return SendTo(connection, buffer.data(), static_cast<u32>(buffer.size()), sendFlags);
    }

    void NetworkServer::BroadcastMessage(ENetworkMessageType type, const u8* payload, u32 payloadSize, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        auto buffer = BuildMessageBuffer(type, payload, payloadSize);
        Broadcast(buffer.data(), static_cast<u32>(buffer.size()), sendFlags);
    }

    bool NetworkServer::IsRunning() const
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_ListenSocket != k_HSteamListenSocket_Invalid;
    }

    u32 NetworkServer::GetConnectionCount() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return static_cast<u32>(m_Connections.size());
    }

    NetworkMessageDispatcher& NetworkServer::GetDispatcher()
    {
        return m_Dispatcher;
    }

    NetworkStats NetworkServer::GetStats() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_Stats;
    }

    i32 NetworkServer::GetClientPingMs(HSteamNetConnection connection) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Interface)
        {
            return -1;
        }
        SteamNetConnectionRealTimeStatus_t status{};
        if (m_Interface->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK)
        {
            return -1;
        }
        return status.m_nPing;
    }

    void NetworkServer::HandlePing(HSteamNetConnection senderConn, const u8* data, u32 size)
    {
        // Respond with Pong, echoing the payload (typically a timestamp)
        SendMessage(senderConn, ENetworkMessageType::Pong, data, size, k_nSteamNetworkingSend_Reliable);
    }

    void NetworkServer::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        // Only handle connections that belong to our listen socket or are already tracked
        if (pInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid && pInfo->m_info.m_hListenSocket != m_ListenSocket)
        {
            return;
        }

        // Ignore connections that aren't incoming (not associated with any listen socket)
        // and aren't already tracked by this server
        if (pInfo->m_info.m_hListenSocket == k_HSteamListenSocket_Invalid && m_Connections.find(pInfo->m_hConn) == m_Connections.end())
        {
            return;
        }

        switch (pInfo->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_Connecting:
            {
                // Enforce max connection limit
                if (m_MaxConnections > 0 && static_cast<u32>(m_Connections.size()) >= m_MaxConnections)
                {
                    m_Interface->CloseConnection(pInfo->m_hConn, 0, "Server full", false);
                    OLO_CORE_WARN("[NetworkServer] Rejected connection: server full ({}/{})",
                                  m_Connections.size(), m_MaxConnections);
                    break;
                }

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
                u32 const clientID = m_NextClientID++;
                conn.SetClientID(clientID);
                m_Connections.emplace(pInfo->m_hConn, std::move(conn));

                OLO_CORE_INFO("[NetworkServer] Client connecting (ID: {})", clientID);
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

    void NetworkServer::SetMaxConnections(u32 maxConnections)
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_Mutex);
        m_MaxConnections = maxConnections;
    }

    u32 NetworkServer::GetMaxConnections() const
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_MaxConnections;
    }

    void NetworkServer::SetIdleTimeout(f32 timeoutSeconds)
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_Mutex);
        m_IdleTimeout = timeoutSeconds;
    }

    f32 NetworkServer::GetIdleTimeout() const
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_IdleTimeout;
    }
} // namespace OloEngine
