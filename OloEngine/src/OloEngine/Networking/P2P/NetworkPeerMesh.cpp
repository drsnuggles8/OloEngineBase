#include "OloEnginePCH.h"
#include "NetworkPeerMesh.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

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

    void NetworkPeerMesh::SendToPeer(u32 peerID, ENetworkMessageType /*type*/, const u8* /*data*/, u32 /*size*/,
                                     i32 /*sendFlags*/)
    {
        if (m_Peers.find(peerID) == m_Peers.end())
        {
            OLO_CORE_WARN("[NetworkPeerMesh] Peer {} not found", peerID);
            return;
        }

        // TODO: actual GNS send via peer's connection handle
        // This requires establishing HSteamNetConnection per peer pair,
        // which will be done when real GNS relay is integrated.
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
} // namespace OloEngine
