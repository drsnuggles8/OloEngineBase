#include "OloEnginePCH.h"
#include "NetworkPeerMesh.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    NetworkPeerMesh::NetworkPeerMesh() = default;

    void NetworkPeerMesh::CreateSession(u32 localPeerID)
    {
        OLO_PROFILE_FUNCTION();

        m_LocalPeerID = localPeerID;
        m_HostPeerID = localPeerID;
        m_InSession = true;

        PeerInfo self;
        self.PeerID = localPeerID;
        self.IsHost = true;
        m_Peers[localPeerID] = self;

        OLO_CORE_INFO("[NetworkPeerMesh] Created session, local peer {} is host", localPeerID);
    }

    void NetworkPeerMesh::JoinSession(u32 localPeerID, const std::string& hostAddress, u16 hostPort)
    {
        OLO_PROFILE_FUNCTION();

        m_LocalPeerID = localPeerID;
        m_InSession = true;

        PeerInfo self;
        self.PeerID = localPeerID;
        self.IsHost = false;
        m_Peers[localPeerID] = self;

        // Record the host
        PeerInfo host;
        host.Address = hostAddress;
        host.Port = hostPort;
        host.IsHost = true;
        // Host peer ID will be learned during connection handshake
        // For now mark as 0 (unknown until PeerIntroduction is received)
        m_HostPeerID = 0;

        OLO_CORE_INFO("[NetworkPeerMesh] Joining session at {}:{}", hostAddress, hostPort);
    }

    void NetworkPeerMesh::LeaveSession()
    {
        OLO_PROFILE_FUNCTION();

        CloseTransport();

        m_Peers.clear();
        m_InSession = false;
        m_LocalPeerID = 0;
        m_HostPeerID = 0;

        OLO_CORE_INFO("[NetworkPeerMesh] Left session");
    }

    u32 NetworkPeerMesh::GetLocalPeerID() const
    {
        return m_LocalPeerID;
    }

    const std::unordered_map<u32, PeerInfo>& NetworkPeerMesh::GetPeers() const
    {
        return m_Peers;
    }

    bool NetworkPeerMesh::IsHost() const
    {
        return m_InSession && m_LocalPeerID == m_HostPeerID;
    }

    bool NetworkPeerMesh::IsInSession() const
    {
        return m_InSession;
    }

    // ── GNS Transport ────────────────────────────────────────────────

    bool NetworkPeerMesh::StartListening(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        m_Interface = SteamNetworkingSockets();
        if (!m_Interface)
        {
            OLO_CORE_WARN("[NetworkPeerMesh] GNS not available — transport disabled");
            return false;
        }

        SteamNetworkingIPAddr addr;
        addr.Clear();
        addr.m_port = port;

        m_ListenSocket = m_Interface->CreateListenSocketIP(addr, 0, nullptr);
        if (m_ListenSocket == k_HSteamListenSocket_Invalid)
        {
            OLO_CORE_ERROR("[NetworkPeerMesh] Failed to listen on port {}", port);
            return false;
        }

        m_PollGroup = m_Interface->CreatePollGroup();
        if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            m_Interface->CloseListenSocket(m_ListenSocket);
            m_ListenSocket = k_HSteamListenSocket_Invalid;
            OLO_CORE_ERROR("[NetworkPeerMesh] Failed to create poll group");
            return false;
        }

        OLO_CORE_INFO("[NetworkPeerMesh] Listening for peers on port {}", port);
        return true;
    }

    bool NetworkPeerMesh::ConnectToPeer(u32 peerID, const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Interface)
        {
            m_Interface = SteamNetworkingSockets();
        }
        if (!m_Interface)
        {
            OLO_CORE_WARN("[NetworkPeerMesh] GNS not available — cannot connect to peer {}", peerID);
            return false;
        }

        // Ensure a poll group exists for receiving messages
        if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            m_PollGroup = m_Interface->CreatePollGroup();
            if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
            {
                return false;
            }
        }

        SteamNetworkingIPAddr peerAddr;
        peerAddr.Clear();
        if (!peerAddr.ParseString(address.c_str()))
        {
            OLO_CORE_ERROR("[NetworkPeerMesh] Invalid address for peer {}: {}", peerID, address);
            return false;
        }
        peerAddr.m_port = port;

        HSteamNetConnection conn = m_Interface->ConnectByIPAddress(peerAddr, 0, nullptr);
        if (conn == k_HSteamNetConnection_Invalid)
        {
            OLO_CORE_ERROR("[NetworkPeerMesh] Failed to connect to peer {} at {}:{}", peerID, address, port);
            return false;
        }

        m_Interface->SetConnectionPollGroup(conn, m_PollGroup);

        TUniqueLock<FMutex> lock(m_Mutex);
        m_PeerConnections[peerID] = conn;
        m_ConnectionToPeer[conn] = peerID;

        // Ensure peer is in the topology
        if (m_Peers.find(peerID) == m_Peers.end())
        {
            PeerInfo info;
            info.PeerID = peerID;
            info.Address = address;
            info.Port = port;
            m_Peers[peerID] = info;
        }

        OLO_CORE_INFO("[NetworkPeerMesh] Connecting to peer {} at {}:{}", peerID, address, port);
        return true;
    }

    void NetworkPeerMesh::PollMessages()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        if (!m_Interface || m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            return;
        }

        ISteamNetworkingMessage* pIncomingMsg = nullptr;
        i32 numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
        while (numMsgs > 0)
        {
            u32 const msgSize = pIncomingMsg->m_cbSize;

            if (msgSize >= NetworkMessageHeader::kSerializedSize)
            {
                auto const* rawData = static_cast<const u8*>(pIncomingMsg->m_pData);
                FMemoryReader reader(rawData, static_cast<i64>(msgSize));
                reader.ArIsNetArchive = true;

                NetworkMessageHeader header;
                reader << header.Type;
                reader << header.Size;
                reader << header.Flags;

                if (!reader.IsError())
                {
                    u32 const payloadOffset = static_cast<u32>(reader.Tell());
                    const u8* payload = rawData + payloadOffset;
                    u32 const payloadSize = msgSize - payloadOffset;

                    // Resolve sender peer ID from the connection handle
                    u32 senderPeerID = 0;
                    if (auto it = m_ConnectionToPeer.find(pIncomingMsg->m_conn); it != m_ConnectionToPeer.end())
                    {
                        senderPeerID = it->second;
                    }

                    if (m_MessageCallback)
                    {
                        m_MessageCallback(senderPeerID, header.Type, payload, payloadSize);
                    }
                }
            }

            pIncomingMsg->Release();
            numMsgs = m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &pIncomingMsg, 1);
        }
    }

    void NetworkPeerMesh::SendToPeer(u32 peerID, ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Peers.find(peerID) == m_Peers.end())
        {
            OLO_CORE_WARN("[NetworkPeerMesh] Peer {} not found", peerID);
            return;
        }

        TUniqueLock<FMutex> lock(m_Mutex);

        auto connIt = m_PeerConnections.find(peerID);
        if (connIt == m_PeerConnections.end() || !m_Interface)
        {
            OLO_CORE_WARN("[NetworkPeerMesh] No transport connection to peer {}", peerID);
            return;
        }

        // Build framed message: NetworkMessageHeader + payload
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;

        NetworkMessageHeader header;
        header.Type = type;
        header.Size = size;
        writer << header.Type;
        writer << header.Size;
        writer << header.Flags;

        if (data && size > 0)
        {
            buffer.insert(buffer.end(), data, data + size);
        }

        m_Interface->SendMessageToConnection(connIt->second, buffer.data(), static_cast<u32>(buffer.size()), sendFlags,
                                             nullptr);
    }

    void NetworkPeerMesh::BroadcastToPeers(ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags)
    {
        for (auto const& [peerID, info] : m_Peers)
        {
            if (peerID != m_LocalPeerID)
            {
                SendToPeer(peerID, type, data, size, sendFlags);
            }
        }
    }

    void NetworkPeerMesh::SetMessageCallback(PeerMessageCallback callback)
    {
        m_MessageCallback = std::move(callback);
    }

    void NetworkPeerMesh::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);

        // Only handle connections belonging to our listen socket or already tracked
        if (pInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
            pInfo->m_info.m_hListenSocket != m_ListenSocket)
        {
            return;
        }
        if (pInfo->m_info.m_hListenSocket == k_HSteamListenSocket_Invalid &&
            m_ConnectionToPeer.find(pInfo->m_hConn) == m_ConnectionToPeer.end() &&
            m_PeerConnections.empty())
        {
            return;
        }

        switch (pInfo->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_Connecting:
            {
                // Accept incoming connections on our listen socket (host mode)
                if (m_Interface && m_ListenSocket != k_HSteamListenSocket_Invalid &&
                    pInfo->m_info.m_hListenSocket == m_ListenSocket)
                {
                    if (m_Interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
                    {
                        m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                        OLO_CORE_WARN("[NetworkPeerMesh] Failed to accept peer connection");
                        break;
                    }
                    if (!m_Interface->SetConnectionPollGroup(pInfo->m_hConn, m_PollGroup))
                    {
                        m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                        OLO_CORE_WARN("[NetworkPeerMesh] Failed to assign connection to poll group");
                        break;
                    }
                    OLO_CORE_INFO("[NetworkPeerMesh] Peer connecting (handle {})", pInfo->m_hConn);
                }
                break;
            }

            case k_ESteamNetworkingConnectionState_Connected:
            {
                OLO_CORE_INFO("[NetworkPeerMesh] Peer connection established (handle {})", pInfo->m_hConn);
                break;
            }

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            {
                if (auto it = m_ConnectionToPeer.find(pInfo->m_hConn); it != m_ConnectionToPeer.end())
                {
                    u32 const peerID = it->second;
                    OLO_CORE_INFO("[NetworkPeerMesh] Peer {} disconnected: {}", peerID, pInfo->m_info.m_szEndDebug);
                    m_Peers.erase(peerID);
                    m_PeerConnections.erase(peerID);
                    m_ConnectionToPeer.erase(it);
                }
                if (m_Interface)
                {
                    m_Interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                }
                break;
            }

            default:
                break;
        }
    }

    // ── Host Migration ───────────────────────────────────────────────

    void NetworkPeerMesh::PerformHostMigration()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Peers.empty())
        {
            return;
        }

        // Find the lowest peer ID as the new host
        u32 newHostID = UINT32_MAX;
        for (auto const& [peerID, info] : m_Peers)
        {
            if (peerID < newHostID)
            {
                newHostID = peerID;
            }
        }

        // Update host status
        for (auto& [peerID, info] : m_Peers)
        {
            info.IsHost = (peerID == newHostID);
        }

        m_HostPeerID = newHostID;
        OLO_CORE_INFO("[NetworkPeerMesh] Host migrated to peer {}", newHostID);
    }

    u32 NetworkPeerMesh::GetHostPeerID() const
    {
        return m_HostPeerID;
    }

    // ── Internal ─────────────────────────────────────────────────────

    void NetworkPeerMesh::CloseTransport()
    {
        TUniqueLock<FMutex> lock(m_Mutex);

        if (m_Interface)
        {
            for (auto const& [peerID, conn] : m_PeerConnections)
            {
                m_Interface->CloseConnection(conn, 0, "Leaving session", false);
            }

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
        }

        m_PeerConnections.clear();
        m_ConnectionToPeer.clear();
        m_Interface = nullptr;
    }
} // namespace OloEngine
