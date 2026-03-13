#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    enum class ENetworkMessageType : u16
    {
        None = 0,
        Connect,
        Disconnect,
        Ping,
        Pong,
        EntitySnapshot,
        DeltaSnapshot,
        RPC,
        InputCommand,
        InputAck,
        EntitySpawn,
        EntityDespawn,

        // Lockstep
        LockstepInput,
        StateHash,
        ResyncRequest,
        ResyncResponse,

        // P2P
        HostMigration,
        PeerIntroduction,

        // Lobby / Session
        LobbyCreate,
        LobbyJoin,
        LobbyLeave,
        LobbyReady,
        LobbyStart,
        SessionStart,
        SessionEnd,

        // MMO — Zone Handoff
        ZoneHandoffRequest,
        ZoneHandoffReady,
        ZoneHandoffComplete,
        ZonePlayerCount,

        // MMO — Instancing
        InstanceCreate,
        InstanceDestroy,
        InstanceJoin,
        LayerAssign,
        LayerMerge,

        // MMO — Chat
        ChatSend,
        ChatReceive,
        ChatJoinChannel,
        ChatLeaveChannel,

        // MMO — Persistence
        PlayerLogin,
        PlayerLogout,
        WorldStateSync,

        // Range for user-defined messages
        UserMessage = 1000
    };

    // Distinguishes full vs delta snapshot payloads
    enum class ESnapshotType : u8
    {
        Full = 0,
        Delta
    };

    struct NetworkMessageHeader
    {
        ENetworkMessageType Type = ENetworkMessageType::None;
        u32 Size = 0; // Payload size (excluding header)
        u8 Flags = 0; // Reliability, channel, etc.

        // Serialized size (no struct padding): sizeof(Type) + sizeof(Size) + sizeof(Flags)
        static constexpr u32 kSerializedSize = sizeof(ENetworkMessageType) + sizeof(u32) + sizeof(u8);
    };

    // Handler receives: senderClientID (0 for server), payload data pointer, payload size
    using NetworkMessageHandler = std::function<void(u32 senderClientID, const u8* data, u32 size)>;

    // Dispatches incoming messages to registered per-type handlers
    class NetworkMessageDispatcher
    {
      public:
        void RegisterHandler(ENetworkMessageType type, NetworkMessageHandler handler);
        void Dispatch(u32 senderClientID, ENetworkMessageType type, const u8* data, u32 size) const;
        [[nodiscard]] bool HasHandler(ENetworkMessageType type) const;

      private:
        std::unordered_map<ENetworkMessageType, NetworkMessageHandler> m_Handlers;
    };

    // Tracks network I/O statistics over time
    struct NetworkStats
    {
        u64 TotalBytesSent = 0;
        u64 TotalBytesReceived = 0;
        u32 TotalMessagesSent = 0;
        u32 TotalMessagesReceived = 0;

        // Per-second rates (updated externally)
        f32 BytesSentPerSec = 0.0f;
        f32 BytesReceivedPerSec = 0.0f;
        f32 MessagesSentPerSec = 0.0f;
        f32 MessagesReceivedPerSec = 0.0f;

        // Snapshot for rate computation
        u64 PrevBytesSent = 0;
        u64 PrevBytesReceived = 0;
        u32 PrevMessagesSent = 0;
        u32 PrevMessagesReceived = 0;
        f32 AccumulatedTime = 0.0f;

        void RecordSend(u32 bytes)
        {
            TotalBytesSent += bytes;
            ++TotalMessagesSent;
        }

        void RecordReceive(u32 bytes)
        {
            TotalBytesReceived += bytes;
            ++TotalMessagesReceived;
        }

        void UpdateRates(f32 dt)
        {
            AccumulatedTime += dt;
            if (AccumulatedTime >= 1.0f)
            {
                BytesSentPerSec = static_cast<f32>(TotalBytesSent - PrevBytesSent) / AccumulatedTime;
                BytesReceivedPerSec = static_cast<f32>(TotalBytesReceived - PrevBytesReceived) / AccumulatedTime;
                MessagesSentPerSec = static_cast<f32>(TotalMessagesSent - PrevMessagesSent) / AccumulatedTime;
                MessagesReceivedPerSec = static_cast<f32>(TotalMessagesReceived - PrevMessagesReceived) / AccumulatedTime;

                PrevBytesSent = TotalBytesSent;
                PrevBytesReceived = TotalBytesReceived;
                PrevMessagesSent = TotalMessagesSent;
                PrevMessagesReceived = TotalMessagesReceived;
                AccumulatedTime = 0.0f;
            }
        }

        void Reset()
        {
            *this = NetworkStats{};
        }
    };
} // namespace OloEngine
