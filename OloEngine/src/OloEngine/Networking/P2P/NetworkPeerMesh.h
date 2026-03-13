#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Represents a connected peer in a P2P mesh.
    struct PeerInfo
    {
        u32 PeerID = 0;
        std::string Address;
        u16 Port = 0;
        bool IsHost = false;
    };

    // Callback invoked when a message is received from a peer.
    using PeerMessageCallback = std::function<void(u32 senderPeerID, ENetworkMessageType type,
                                                   const u8* data, u32 size)>;

    // Manages a full-mesh P2P network topology.
    // Each peer connects directly to every other peer via GNS (direct or relay for NAT).
    class NetworkPeerMesh
    {
      public:
        NetworkPeerMesh();

        // Create a new P2P session. This peer becomes the host.
        void CreateSession(u32 localPeerID);

        // Join an existing P2P session.
        void JoinSession(u32 localPeerID, const std::string& hostAddress, u16 hostPort);

        // Leave the current session.
        void LeaveSession();

        // Get the local peer ID.
        [[nodiscard]] u32 GetLocalPeerID() const;

        // Get all connected peers (including self).
        [[nodiscard]] const std::unordered_map<u32, PeerInfo>& GetPeers() const;

        // Check if this peer is the host.
        [[nodiscard]] bool IsHost() const;

        // Check if a session is active.
        [[nodiscard]] bool IsInSession() const;

        // Send data to a specific peer.
        void SendToPeer(u32 peerID, ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags);

        // Broadcast to all peers (except self).
        void BroadcastToPeers(ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags);

        // Set the message received callback.
        void SetMessageCallback(PeerMessageCallback callback);

        // --- Host Migration ---

        // Trigger host migration. The lowest-ID remaining peer becomes the new host.
        void PerformHostMigration();

        // Get the current host peer ID.
        [[nodiscard]] u32 GetHostPeerID() const;

      private:
        u32 m_LocalPeerID = 0;
        u32 m_HostPeerID = 0;
        bool m_InSession = false;
        std::unordered_map<u32, PeerInfo> m_Peers;
        PeerMessageCallback m_MessageCallback;
    };
} // namespace OloEngine
