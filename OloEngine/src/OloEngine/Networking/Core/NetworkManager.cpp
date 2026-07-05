#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkThread.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Memory/Platform.h" // OLO_ASAN_ENABLED
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
    NetworkPeerMesh NetworkManager::s_PeerMesh;

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

        // Set debug output and connection-status callback BEFORE Init so that
        // the networking thread spawned by Init already sees the configured
        // spew level, avoiding a data race on g_eDefaultGroupSpewLevel.
        SteamNetworkingUtils()->SetDebugOutputFunction(
            k_ESteamNetworkingSocketsDebugOutputType_Msg,
            GNSDebugOutput);

        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            GNSConnectionStatusCallback);

#if OLO_ASAN_ENABLED
        // GameNetworkingSockets_Init spawns an internal service thread whose
        // startup routine trips a stack-buffer-overflow under MSVC AddressSanitizer:
        // a 48-byte read out of a 24-byte `info` stack object during thread
        // bootstrap, inside vendored GNS code on the GNS-spawned thread — not
        // OloEngine code, and clean under Linux ASan/UBSan. (In Release the
        // internal frames get ICF-folded onto the nearest exported symbols, so CI
        // stacks misreport it as SteamNetworkingSockets_* / pugi::xpath_*.)
        // Skip the live GNS init under ASan so the rest of OloEngine's networking
        // stack — the NetworkThread, task dispatch, message serialization and
        // replication — still runs under the sanitizer. Tests that need a live
        // socket (NetworkIntegrationTest) stay excluded from the ASan job; the
        // debug-output/connection callbacks set above are harmless no-ops without
        // a running service thread. Non-ASan production builds are unaffected.
        // See issue #317; upstream GNS bug filed as
        // ValveSoftware/GameNetworkingSockets#418 — remove this gate once a fixed
        // GNS is vendored.
        OLO_CORE_WARN("NetworkManager: skipping GameNetworkingSockets_Init under AddressSanitizer (issue #317)");
#else
        if (SteamDatagramErrMsg errMsg; !GameNetworkingSockets_Init(nullptr, errMsg))
        {
            OLO_CORE_ERROR("GameNetworkingSockets_Init failed: {}", errMsg);
            return false;
        }
#endif

        NetworkThread::Start(60);

        ComponentReplicator::RegisterDefaults();
        ComponentInterpolationRegistry::RegisterDefaults();

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

#if !OLO_ASAN_ENABLED
        // Matches the Init() gate above: GameNetworkingSockets was never started
        // under ASan, so there is nothing to tear down. See issue #317.
        GameNetworkingSockets_Kill();
#endif

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
                                                      if (!s_ServerInputHandler.ProcessInput(*s_ActiveScene, senderClientID,
                                                                                             data, size))
                                                      {
                                                          OLO_CORE_WARN_TAG("Networking", "Server rejected input from client {}", senderClientID);
                                                      }
                                                  });

        // Prune the per-client last-processed-tick entry on disconnect so it
        // doesn't grow without bound over the lifetime of a long-running server.
        s_Server->SetClientDisconnectedCallback([](u32 clientID)
                                                { s_ServerInputHandler.RemoveClient(clientID); });

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

        // Drop this session's client-side snapshot/prediction state so a later
        // reconnect never mixes stale session-A snapshots/inputs with session-B
        // (the interpolator/prediction buffers and tick counter would otherwise
        // survive the reset above, which only clears s_Client).
        s_ClientInterpolator.Reset();
        s_ClientPrediction.ResetSession();
        s_ClientReceivedTick = 0;
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

        // Route to P2P mesh if it has an active session
        if (s_PeerMesh.IsInSession())
        {
            s_PeerMesh.OnConnectionStatusChanged(pInfo);
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

    std::optional<NetworkStats> NetworkManager::GetStats()
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        if (s_Server)
        {
            return s_Server->GetStats();
        }
        if (s_Client)
        {
            return s_Client->GetStats();
        }
        return std::nullopt;
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
            // Rate-limit snapshot broadcasts: accumulate time from network thread tick
            f32 const dt = 1.0f / static_cast<f32>(NetworkThread::GetTickRate());
            s_SnapshotAccumulator += dt;

            f32 const snapshotInterval = 1.0f / static_cast<f32>(s_SnapshotRateHz);
            if (s_SnapshotAccumulator < snapshotInterval)
            {
                return;
            }
            s_SnapshotAccumulator -= snapshotInterval;

            ++s_TickCounter;

            // Capture a full snapshot and store in the buffer
            auto snapshot = EntitySnapshot::Capture(*s_ActiveScene);
            if (!snapshot.empty())
            {
                // Try delta against baseline
                if (const auto* baseline = s_SnapshotBuffer.GetLatest(); baseline)
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

        i32 pingMs = -1;
        s_Server->ForEachConnection([&clientID, &pingMs](HSteamNetConnection connHandle, const NetworkConnection& conn)
                                    {
            if (conn.GetClientID() == clientID)
            {
                pingMs = s_Server->GetClientPingMs(connHandle);
            } });
        return pingMs;
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

    NetworkPeerMesh& NetworkManager::GetPeerMesh()
    {
        return s_PeerMesh;
    }
} // namespace OloEngine
