#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkThread.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Threading/UniqueLock.h"

#include "OloEngine/Serialization/Archive.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace OloEngine
{
    FMutex NetworkManager::s_Mutex;
    bool NetworkManager::s_Initialized = false;
    Scope<NetworkServer> NetworkManager::s_Server = nullptr;
    Scope<NetworkClient> NetworkManager::s_Client = nullptr;
    u32 NetworkManager::s_SnapshotRateHz = 20;
    u32 NetworkManager::s_TickCounter = 0;
    u32 NetworkManager::s_ClientReceivedTick = 0;
    f32 NetworkManager::s_SnapshotAccumulator = 0.0f;
    Scene* NetworkManager::s_ActiveScene = nullptr;
    SnapshotBuffer NetworkManager::s_SnapshotBuffer;
    SnapshotInterpolator NetworkManager::s_ClientInterpolator;
    ClientPrediction NetworkManager::s_ClientPrediction;
    ServerInputHandler NetworkManager::s_ServerInputHandler;
    LagCompensator NetworkManager::s_LagCompensator;
    NetworkSession NetworkManager::s_Session;
    NetworkLobby NetworkManager::s_Lobby;

    static void GNSDebugOutput(ESteamNetworkingSocketsDebugOutputType eType, char const* pszMsg)
    {
        switch (eType)
        {
            case k_ESteamNetworkingSocketsDebugOutputType_Bug:
            case k_ESteamNetworkingSocketsDebugOutputType_Error:
                OLO_CORE_ERROR("[GNS] {}", pszMsg);
                break;
            case k_ESteamNetworkingSocketsDebugOutputType_Important:
            case k_ESteamNetworkingSocketsDebugOutputType_Warning:
                OLO_CORE_WARN("[GNS] {}", pszMsg);
                break;
            default:
                OLO_CORE_TRACE("[GNS] {}", pszMsg);
                break;
        }
    }

    static void GNSConnectionStatusCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        NetworkManager::OnConnectionStatusChanged(pInfo);
    }

    bool NetworkManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Initialized)
        {
            OLO_CORE_WARN("NetworkManager::Init() called when already initialized");
            return true;
        }

        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            OLO_CORE_ERROR("GameNetworkingSockets_Init failed: {}", errMsg);
            return false;
        }

        SteamNetworkingUtils()->SetDebugOutputFunction(
            k_ESteamNetworkingSocketsDebugOutputType_Msg,
            GNSDebugOutput);

        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            GNSConnectionStatusCallback);

        NetworkThread::Start(60);

        s_Initialized = true;
        OLO_CORE_INFO("NetworkManager initialized (GameNetworkingSockets)");
        return true;
    }

    void NetworkManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        {
            TUniqueLock<FMutex> lock(s_Mutex);
            if (!s_Initialized)
            {
                return;
            }
        }

        StopServer();
        Disconnect();
        NetworkThread::Stop();

        GameNetworkingSockets_Kill();

        TUniqueLock<FMutex> lock(s_Mutex);
        s_Initialized = false;
        OLO_CORE_INFO("NetworkManager shut down");
    }

    bool NetworkManager::IsInitialized()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Initialized;
    }

    bool NetworkManager::StartServer(u16 port)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("NetworkManager not initialized");
            return false;
        }

        if (s_Server)
        {
            OLO_CORE_WARN("Server already running");
            return false;
        }

        s_Server = CreateScope<NetworkServer>();
        if (!s_Server->Start(port))
        {
            s_Server.reset();
            return false;
        }

        // Register server-side InputCommand handler
        s_Server->GetDispatcher().RegisterHandler(ENetworkMessageType::InputCommand,
                                                  [](u32 senderClientID, const u8* data, u32 size)
                                                  {
                                                      if (!s_ActiveScene)
                                                      {
                                                          return;
                                                      }
                                                      s_ServerInputHandler.ProcessInput(*s_ActiveScene, senderClientID,
                                                                                        data, size);
                                                  });

        return true;
    }

    void NetworkManager::StopServer()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Server)
        {
            s_Server->Stop();
            s_Server.reset();
        }
    }

    bool NetworkManager::IsServer()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Server != nullptr && s_Server->IsRunning();
    }

    bool NetworkManager::Connect(const std::string& address, u16 port)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (!s_Initialized)
        {
            OLO_CORE_ERROR("NetworkManager not initialized");
            return false;
        }

        if (s_Client)
        {
            OLO_CORE_WARN("Already connected or connecting");
            return false;
        }

        s_Client = CreateScope<NetworkClient>();
        if (!s_Client->Connect(address, port))
        {
            s_Client.reset();
            return false;
        }

        // Register default handlers for snapshot reception
        s_Client->GetDispatcher().RegisterHandler(ENetworkMessageType::EntitySnapshot,
                                                  [](u32 /*senderClientID*/, const u8* data, u32 size)
                                                  {
                                                      // Full snapshot — push into interpolator with client-side tick
                                                      std::vector<u8> snapshotData(data, data + size);
                                                      s_ClientInterpolator.PushSnapshot(s_ClientReceivedTick++,
                                                                                        std::move(snapshotData));
                                                  });

        s_Client->GetDispatcher().RegisterHandler(ENetworkMessageType::DeltaSnapshot,
                                                  [](u32 /*senderClientID*/, const u8* data, u32 size)
                                                  {
                                                      // Delta snapshot — push result with client-side tick
                                                      std::vector<u8> deltaData(data, data + size);
                                                      s_ClientInterpolator.PushSnapshot(s_ClientReceivedTick++,
                                                                                        std::move(deltaData));
                                                  });

        // Register InputAck handler — server sends these to confirm processed input tick
        s_Client->GetDispatcher().RegisterHandler(ENetworkMessageType::InputAck,
                                                  [](u32 /*senderClientID*/, const u8* data, u32 size)
                                                  {
                                                      if (size < sizeof(u32) || !s_ActiveScene)
                                                      {
                                                          return;
                                                      }
                                                      FMemoryReader reader(data, static_cast<i64>(size));
                                                      u32 lastProcessedTick = 0;
                                                      reader << lastProcessedTick;
                                                      s_ClientPrediction.Reconcile(*s_ActiveScene, lastProcessedTick);
                                                  });

        return true;
    }

    void NetworkManager::Disconnect()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Client)
        {
            s_Client->Disconnect();
            s_Client.reset();
        }
    }

    bool NetworkManager::IsClient()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Client != nullptr;
    }

    bool NetworkManager::IsConnected()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Client != nullptr && s_Client->IsConnected();
    }

    void NetworkManager::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Server)
        {
            s_Server->OnConnectionStatusChanged(pInfo);
        }

        if (s_Client)
        {
            s_Client->OnConnectionStatusChanged(pInfo);
        }
    }

    bool NetworkManager::SendNetworkMessage(ENetworkMessageType type, const u8* payload, u32 payloadSize,
                                            i32 sendFlags)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Client && s_Client->IsConnected())
        {
            return s_Client->SendMessage(type, payload, payloadSize, sendFlags);
        }

        if (s_Server && s_Server->IsRunning())
        {
            s_Server->BroadcastMessage(type, payload, payloadSize, sendFlags);
            return true;
        }

        return false;
    }

    void NetworkManager::BroadcastSnapshot(const u8* snapshotData, u32 snapshotSize)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Server && s_Server->IsRunning())
        {
            s_Server->BroadcastMessage(ENetworkMessageType::EntitySnapshot, snapshotData, snapshotSize,
                                       k_nSteamNetworkingSend_Unreliable);
        }
    }

    NetworkMessageDispatcher& NetworkManager::GetServerDispatcher()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        OLO_CORE_ASSERT(s_Server, "No server active");
        return s_Server->GetDispatcher();
    }

    NetworkMessageDispatcher& NetworkManager::GetClientDispatcher()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        OLO_CORE_ASSERT(s_Client, "No client active");
        return s_Client->GetDispatcher();
    }

    const NetworkStats* NetworkManager::GetStats()
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Server)
        {
            return &s_Server->GetStats();
        }
        if (s_Client)
        {
            return &s_Client->GetStats();
        }
        return nullptr;
    }

    NetworkServer* NetworkManager::GetServer()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Server.get();
    }

    NetworkClient* NetworkManager::GetClient()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Client.get();
    }

    void NetworkManager::SetSnapshotRate(u32 hz)
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        s_SnapshotRateHz = hz;
        s_ClientInterpolator.SetServerTickRate(hz);
    }

    u32 NetworkManager::GetSnapshotRate()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_SnapshotRateHz;
    }

    void NetworkManager::SetActiveScene(Scene* scene)
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        s_ActiveScene = scene;
    }

    void NetworkManager::TickSnapshots()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (!s_ActiveScene)
        {
            return;
        }

        // Server: capture and broadcast snapshots at the configured rate
        if (s_Server && s_Server->IsRunning())
        {
            ++s_TickCounter;

            // Capture a full snapshot and store in the buffer
            auto snapshot = EntitySnapshot::Capture(*s_ActiveScene);
            if (!snapshot.empty())
            {
                // Try delta against baseline
                const auto* baseline = s_SnapshotBuffer.GetLatest();
                if (baseline)
                {
                    auto delta = EntitySnapshot::CaptureDelta(*s_ActiveScene, baseline->Data);
                    if (!delta.empty())
                    {
                        // Send delta
                        s_Server->BroadcastMessage(ENetworkMessageType::DeltaSnapshot, delta.data(),
                                                   static_cast<u32>(delta.size()), k_nSteamNetworkingSend_Unreliable);
                    }
                    // else: nothing changed, no need to send
                }
                else
                {
                    // No baseline — send full snapshot
                    s_Server->BroadcastMessage(ENetworkMessageType::EntitySnapshot, snapshot.data(),
                                               static_cast<u32>(snapshot.size()), k_nSteamNetworkingSend_Unreliable);
                }

                // Always store the full snapshot in the buffer for future baselines
                s_SnapshotBuffer.Push(s_TickCounter, std::move(snapshot));
            }
        }
    }

    SnapshotBuffer& NetworkManager::GetSnapshotBuffer()
    {
        return s_SnapshotBuffer;
    }

    SnapshotInterpolator& NetworkManager::GetClientInterpolator()
    {
        return s_ClientInterpolator;
    }

    u32 NetworkManager::GetCurrentTick()
    {
        return s_TickCounter;
    }

    void NetworkManager::SendInput(u64 entityUUID, std::vector<u8> inputData)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(s_Mutex);

        if (!s_Client || !s_Client->IsConnected())
        {
            OLO_CORE_WARN("SendInput: not connected to a server");
            return;
        }

        u32 tick = s_TickCounter;

        // Record locally for prediction
        s_ClientPrediction.RecordInput(tick, entityUUID, inputData);

        // Serialize: tick(u32) + entityUUID(u64) + inputData
        std::vector<u8> payload;
        FMemoryWriter writer(payload);
        writer << tick;
        writer << entityUUID;
        writer.Serialize(inputData.data(), static_cast<i64>(inputData.size()));

        s_Client->SendMessage(ENetworkMessageType::InputCommand, payload.data(), static_cast<u32>(payload.size()),
                              k_nSteamNetworkingSend_Reliable);
    }

    void NetworkManager::SetInputApplyCallback(InputApplyCallback callback)
    {
        s_ClientPrediction.SetInputApplyCallback(callback);
        s_ServerInputHandler.SetInputApplyCallback(callback);
    }

    ClientPrediction& NetworkManager::GetClientPrediction()
    {
        return s_ClientPrediction;
    }

    ServerInputHandler& NetworkManager::GetServerInputHandler()
    {
        return s_ServerInputHandler;
    }

    i32 NetworkManager::GetClientPingMs(u32 clientID)
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        if (!s_Server || !s_Server->IsRunning())
        {
            return -1;
        }

        for (const auto& [connHandle, conn] : s_Server->GetConnections())
        {
            if (conn.GetClientID() == clientID)
            {
                return s_Server->GetClientPingMs(connHandle);
            }
        }
        return -1;
    }

    LagCompensator& NetworkManager::GetLagCompensator()
    {
        return s_LagCompensator;
    }

    NetworkSession* NetworkManager::GetSession()
    {
        return &s_Session;
    }

    NetworkLobby* NetworkManager::GetLobby()
    {
        return &s_Lobby;
    }
} // namespace OloEngine
