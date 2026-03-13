#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Threading/Mutex.h"

#include <steam/steamnetworkingsockets.h>

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

    // Manages a full-mesh P2P network topology with GNS transport.
    // Each peer connects directly to every other peer via GNS (direct IP or relay for NAT).
    //
    // Usage (host):
    //   mesh.CreateSession(localPeerID);
    //   mesh.StartListening(port);   // enable GNS transport
    //
    // Usage (joining peer):
    //   mesh.JoinSession(localPeerID, hostAddr, hostPort);
    //   mesh.ConnectToPeer(hostPeerID, hostAddr, hostPort);  // establish GNS link
    //
    // Transport is optional. If GNS is not initialised (e.g. in unit tests),
    // topology management still works; SendToPeer/BroadcastToPeers become no-ops.
    class NetworkPeerMesh
    {
      public:
        NetworkPeerMesh();

        // Create a new P2P session. This peer becomes the host.
        void CreateSession(u32 localPeerID);

        // Join an existing P2P session.
        void JoinSession(u32 localPeerID, const std::string& hostAddress, u16 hostPort);

        // Leave the current session, closing all GNS connections.
        void LeaveSession();

        // Get the local peer ID.
        [[nodiscard]] u32 GetLocalPeerID() const;

        // Get all connected peers (including self).
        [[nodiscard]] const std::unordered_map<u32, PeerInfo>& GetPeers() const;

        // Check if this peer is the host.
        [[nodiscard]] bool IsHost() const;

        // Check if a session is active.
        [[nodiscard]] bool IsInSession() const;

        // Send a framed message to a specific peer via GNS.
        void SendToPeer(u32 peerID, ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags);

        // Broadcast a framed message to all peers (except self) via GNS.
        void BroadcastToPeers(ENetworkMessageType type, const u8* data, u32 size, i32 sendFlags);

        // Set the message received callback.
        void SetMessageCallback(PeerMessageCallback callback);

        // --- Transport (GNS) ---

        // Start listening for incoming peer connections (host mode).
        // Returns false if GNS is unavailable.
        bool StartListening(u16 port);

        // Establish a GNS connection to a specific peer by address.
        bool ConnectToPeer(u32 peerID, const std::string& address, u16 port);

        // Poll the GNS poll group for incoming messages and dispatch via callback.
        void PollMessages();

        // GNS connection-status callback. Should be called from the global GNS handler.
        void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

        // --- Host Migration ---

        // Trigger host migration. The lowest-ID remaining peer becomes the new host.
        void PerformHostMigration();

        // Get the current host peer ID.
        [[nodiscard]] u32 GetHostPeerID() const;

      private:
        void CloseTransport();

        u32 m_LocalPeerID = 0;
        u32 m_HostPeerID = 0;
        bool m_InSession = false;
        std::unordered_map<u32, PeerInfo> m_Peers;
        PeerMessageCallback m_MessageCallback;

        // GNS transport state (all nullptr / invalid when GNS is unavailable)
        ISteamNetworkingSockets* m_Interface = nullptr;
        HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
        std::unordered_map<u32, HSteamNetConnection> m_PeerConnections;  // PeerID → GNS handle
        std::unordered_map<HSteamNetConnection, u32> m_ConnectionToPeer; // GNS handle → PeerID
        mutable FMutex m_Mutex;
    };
} // namespace OloEngine
