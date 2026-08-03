#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkThread.h"
#include "OloEngine/Networking/RPC/RpcDispatcher.h"
#include "OloEngine/Networking/RPC/RpcRegistry.h"
#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Networking/Replication/EntityLifecycle.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Memory/Platform.h" // OLO_ASAN_ENABLED
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
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
    Scene* NetworkManager::s_ActiveScene = nullptr;
    bool NetworkManager::s_ServerSceneChanged = false;
    ServerReplicationDriver NetworkManager::s_ServerDriver;
    ClientReplicationDriver NetworkManager::s_ClientDriver;
    NetworkSession NetworkManager::s_Session;
    NetworkLobby NetworkManager::s_Lobby;
    NetworkPeerMesh NetworkManager::s_PeerMesh;

    namespace
    {
        // Which (client, scene) pair the client driver's dispatcher handlers are
        // currently bound to. Connect() can happen before a scene exists and a scene
        // can be swapped under a live connection, so the binding is refreshed from
        // Tick() rather than done once at connect time. Game-thread only, like
        // Tick() itself.
        NetworkClient* s_AttachedClient = nullptr;
        Scene* s_AttachedScene = nullptr;
    } // namespace

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
        NetworkSpawnRegistry::RegisterDefaults();

        // Wire LAN-discovery responses to the live player count. The provider is
        // invoked from NetworkLobby::PollDiscovery when a host answers a probe;
        // it reads the same server connection count / session player count the
        // server console (CmdPlayers) and debug panel already read from the game
        // thread. Prefer the transport truth (a running dedicated server), and
        // fall back to the session roster (e.g. a P2P/lobby host with no server).
        s_Lobby.SetPlayerCountProvider(
            []() -> u32
            {
                if (s_Server)
                {
                    return s_Server->GetConnectionCount();
                }
                return s_Session.GetPlayerCount();
            });

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
        s_ActiveScene = nullptr;
        s_Initialized = false;
        OLO_CORE_INFO("NetworkManager shut down");
    }

    bool NetworkManager::IsInitialized()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_Initialized;
    }

    void NetworkManager::RegisterServerHandlers()
    {
        // Caller holds s_Mutex and has just constructed s_Server.
        auto& dispatcher = s_Server->GetDispatcher();

        // Every one of these runs from PollMessages(), which Tick() calls on the
        // game thread — so they may touch the scene freely.
        dispatcher.RegisterHandler(ENetworkMessageType::InputCommand,
                                   [](u32 senderClientID, const u8* data, u32 size)
                                   {
                                       if (Scene* scene = GetActiveScene(); scene != nullptr)
                                       {
                                           s_ServerDriver.HandleInputCommand(*scene, senderClientID, data, size);
                                       }
                                   });

        dispatcher.RegisterHandler(ENetworkMessageType::RPC,
                                   [](u32 senderClientID, const u8* data, u32 size)
                                   {
                                       if (Scene* scene = GetActiveScene(); scene != nullptr)
                                       {
                                           s_ServerDriver.HandleRpc(*scene, senderClientID, data, size);
                                       }
                                   });

        dispatcher.RegisterHandler(ENetworkMessageType::SnapshotAck,
                                   [](u32 senderClientID, const u8* data, u32 size)
                                   { s_ServerDriver.HandleSnapshotAck(senderClientID, data, size); });

        // NOTE: no SetClientDisconnectedCallback here, deliberately. GNS raises the
        // disconnect on the NETWORK thread, so pruning the input handler from that
        // callback would mutate m_LastProcessedTicks while the game thread is reading
        // and writing it in HandleInputCommand/ReplicateToClient — a data race on
        // exactly the boundary this file's threading contract draws. The pruning
        // still happens, one drain later: ServerReplicationDriver::Tick pops the
        // queued disconnect event on the game thread and calls RemoveClient there.
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

        s_ServerDriver.Reset();
        RegisterServerHandlers();
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
        s_ServerDriver.Reset();
        // Reset() is the stronger operation — it drops the connections too — so a
        // pending scene-swap rebuild has nothing left to do.
        s_ServerSceneChanged = false;
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

        // The client driver's handlers are bound from Tick(): a scene may not exist
        // yet at connect time, and the handlers need the scene they will write to.
        s_ClientDriver.Reset();
        s_AttachedClient = nullptr;
        s_AttachedScene = nullptr;
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

        // Drop this session's client-side replication state so a later reconnect
        // never mixes stale session-A snapshots/inputs with session-B.
        s_ClientDriver.Reset();
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
        // NETWORK THREAD. Transport bookkeeping only — the ECS-visible consequences
        // (spawning/despawning a client's pawn) are queued and applied from Tick().
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
        s_ServerDriver.SetSnapshotRate(hz);
        s_ClientDriver.GetInterpolator().SetServerTickRate(hz);
    }

    u32 NetworkManager::GetSnapshotRate()
    {
        return s_ServerDriver.GetSnapshotRate();
    }

    void NetworkManager::SetActiveScene(Scene* scene)
    {
        TUniqueLock<FMutex> lock(s_Mutex);

        // Remember that the server driver's state now describes a scene that is no
        // longer the active one, so the next Tick can rebuild it. Recorded as a flag
        // rather than by comparing scene pointers in Tick, for two reasons: a swap
        // usually goes through nullptr (the editor's Stop-then-Play, and every host's
        // teardown ordering), which a pointer comparison in Tick never observes
        // because Tick early-outs on a null scene; and the outgoing Scene is freed
        // immediately, so a retained pointer could compare EQUAL to a freshly
        // allocated one at the same address and silently skip the rebuild.
        //
        // Only a transition away from a real scene counts — the initial nullptr to
        // first-scene bind has nothing to rebuild.
        if (s_ActiveScene != nullptr && s_ActiveScene != scene)
        {
            s_ServerSceneChanged = true;
        }

        // The CLIENT half needs the same treatment, and for a sharper reason. Tick
        // re-attaches only when `s_AttachedScene != scene`, so if the outgoing Scene
        // is freed and the incoming one is allocated at the same address, that test
        // compares EQUAL, the re-attach is skipped, and the client driver's
        // dispatcher handlers keep holding a reference to the destroyed Scene — a
        // use-after-free, not merely stale state. Forgetting the binding here forces
        // Tick to rebuild it. (Connect() clears the same pair for the same reason.)
        if (s_ActiveScene != scene)
        {
            s_AttachedScene = nullptr;
            s_AttachedClient = nullptr;
        }

        s_ActiveScene = scene;
    }

    Scene* NetworkManager::GetActiveScene()
    {
        TUniqueLock<FMutex> lock(s_Mutex);
        return s_ActiveScene;
    }

    void NetworkManager::Tick(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        // Snapshot the shared members once, then release the lock: everything below
        // touches the Scene and must not hold a lock a GNS callback can block on.
        // Safe because the lifetime-changing calls (StartServer/StopServer/
        // Connect/Disconnect/SetActiveScene) are game-thread-only, like this.
        NetworkServer* server = nullptr;
        NetworkClient* client = nullptr;
        Scene* scene = nullptr;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            if (!s_Initialized)
            {
                return;
            }
            server = s_Server.get();
            client = s_Client.get();
            scene = s_ActiveScene;
        }

        if (scene == nullptr)
        {
            return;
        }

        const bool hostingServer = server != nullptr && server->IsRunning();

        // Consume the pending swap only now that a scene actually exists — the flag
        // has to survive the null-scene frames between a teardown and the next bind.
        bool sceneChanged = false;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            sceneChanged = s_ServerSceneChanged;
            s_ServerSceneChanged = false;
        }

        if (hostingServer)
        {
            // Rebind the driver to the new scene before anything reads from it. The
            // same lazy-rebind shape as the client attach below, and for the same
            // reason: a host can swap the runtime scene under a LIVE server — the
            // dedicated server's `reload`, the editor's Stop-then-Play — and
            // everything the driver holds about the old scene (baselines, known-entity
            // sets, history, archetypes) then names entities that no longer exist,
            // while the connections themselves must survive. OloServerApp does this
            // for its own console command; doing it here covers every other host.
            if (sceneChanged)
            {
                s_ServerDriver.ResetForSceneSwap(*scene, *server);
            }

            // Poll first so this frame's inputs and RPCs are applied to the
            // simulation BEFORE the snapshot that reports its state.
            server->PollMessages();
            s_ServerDriver.Tick(*scene, *server, dt);
        }

        if (client != nullptr)
        {
            // Listen server: this process is both ends and they share one Scene,
            // which the server has already made authoritative. The client half must
            // not also write replicated state into it — see
            // ClientReplicationDriver::SetSharedSceneWithServer.
            s_ClientDriver.SetSharedSceneWithServer(hostingServer);

            // Attach lazily: Connect() can happen before a scene exists, and the
            // handlers capture the scene they will write to.
            if (s_AttachedScene != scene || s_AttachedClient != client)
            {
                s_ClientDriver.AttachTo(*client, *scene);
                s_AttachedScene = scene;
                s_AttachedClient = client;
            }
            s_ClientDriver.Tick(*scene, *client, dt);
        }
    }

    ServerReplicationDriver& NetworkManager::GetServerDriver()
    {
        return s_ServerDriver;
    }

    ClientReplicationDriver& NetworkManager::GetClientDriver()
    {
        return s_ClientDriver;
    }

    SnapshotBuffer& NetworkManager::GetSnapshotBuffer()
    {
        return s_ServerDriver.GetHistory();
    }

    SnapshotInterpolator& NetworkManager::GetClientInterpolator()
    {
        return s_ClientDriver.GetInterpolator();
    }

    u32 NetworkManager::GetCurrentTick()
    {
        return s_ServerDriver.GetCurrentTick();
    }

    void NetworkManager::SendInput(u64 entityUUID, std::vector<u8> inputData)
    {
        OLO_PROFILE_FUNCTION();

        NetworkClient* client = nullptr;
        Scene* scene = nullptr;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            client = s_Client.get();
            scene = s_ActiveScene;
        }

        if (client == nullptr || !client->IsConnected())
        {
            OLO_CORE_WARN("SendInput: not connected to a server");
            return;
        }
        if (scene == nullptr)
        {
            OLO_CORE_WARN("SendInput: no active scene registered with the NetworkManager");
            return;
        }

        s_ClientDriver.SendInput(*scene, *client, entityUUID, std::move(inputData));
    }

    void NetworkManager::SetInputApplyCallback(InputApplyCallback callback)
    {
        s_ClientDriver.SetInputApplyCallback(callback);
        s_ServerDriver.GetInputHandler().SetInputApplyCallback(std::move(callback));
    }

    ClientPrediction& NetworkManager::GetClientPrediction()
    {
        return s_ClientDriver.GetPrediction();
    }

    ServerInputHandler& NetworkManager::GetServerInputHandler()
    {
        return s_ServerDriver.GetInputHandler();
    }

    u64 NetworkManager::SpawnReplicated(std::string_view archetype, const std::string& name, u32 ownerClientID,
                                        ENetworkAuthority authority)
    {
        if (!IsServer())
        {
            // Refusing beats queueing: the queue is only drained by the SERVER half
            // of Tick(), so a client-side call would leave the spawn parked forever
            // while handing the caller a UUID for an entity that never appears.
            OLO_CORE_WARN_TAG("Networking", "SpawnReplicated: not running a server; call ignored");
            return 0;
        }

        if (GetActiveScene() == nullptr)
        {
            OLO_CORE_WARN_TAG("Networking", "SpawnReplicated: no active scene");
            return 0;
        }

        // Pre-allocate the id and queue the creation for the next Tick. A script may
        // be calling this from inside its OnUpdate, i.e. while Scene::UpdateScripts
        // is iterating the script pools — creating the entity here would invalidate
        // that iteration.
        const UUID uuid;
        s_ServerDriver.QueueSpawn(uuid, std::string(archetype), name, ownerClientID, authority);
        return static_cast<u64>(uuid);
    }

    void NetworkManager::DespawnReplicated(u64 entityUUID)
    {
        if (!IsServer())
        {
            OLO_CORE_WARN_TAG("Networking", "DespawnReplicated: not running a server; call ignored");
            return;
        }

        if (GetActiveScene() == nullptr)
        {
            OLO_CORE_WARN_TAG("Networking", "DespawnReplicated: no active scene");
            return;
        }

        // Deferred for the same reason as SpawnReplicated. The despawn messaging
        // needs a live server, which the drain checks when it runs.
        s_ServerDriver.QueueDespawn(entityUUID);
    }

    void NetworkManager::RegisterRPC(RpcDescriptor descriptor)
    {
        RpcRegistry::Register(std::move(descriptor));
    }

    bool NetworkManager::InvokeRPC(std::string_view name, u64 entityUUID, const RpcArgList& args, u32 targetClientID)
    {
        OLO_PROFILE_FUNCTION();

        auto descriptor = RpcRegistry::FindByName(name);
        if (!descriptor.has_value())
        {
            OLO_CORE_WARN_TAG("Networking", "InvokeRPC: '{}' is not registered", name);
            return false;
        }

        NetworkServer* server = nullptr;
        NetworkClient* client = nullptr;
        Scene* scene = nullptr;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            server = s_Server.get();
            client = s_Client.get();
            scene = s_ActiveScene;
        }

        // A host that is running a server acts as the authority even if it also has
        // a client connection (listen-server topology).
        if (server != nullptr && server->IsRunning())
        {
            if (scene == nullptr)
            {
                OLO_CORE_WARN_TAG("Networking", "InvokeRPC: no active scene");
                return false;
            }
            return s_ServerDriver.InvokeRpc(*scene, *server, *descriptor, entityUUID, targetClientID, args);
        }

        if (client != nullptr && client->IsConnected())
        {
            return s_ClientDriver.InvokeRpc(*client, *descriptor, entityUUID, args);
        }

        OLO_CORE_WARN_TAG("Networking", "InvokeRPC: '{}' has no transport to travel on", name);
        return false;
    }

    i32 NetworkManager::GetClientPingMs(u32 clientID)
    {
        NetworkServer* server = nullptr;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            server = s_Server.get();
        }

        if (server == nullptr || !server->IsRunning())
        {
            return -1;
        }

        // GetClientPingMsById resolves the handle and queries the transport without
        // nesting two acquisitions of the server's mutex — the previous shape here
        // (a GetClientPingMs call from inside a ForEachConnection lambda) deadlocked
        // on the first client whose ping was asked for.
        return server->GetClientPingMsById(clientID);
    }

    LagCompensator& NetworkManager::GetLagCompensator()
    {
        return s_ServerDriver.GetLagCompensator();
    }

    bool NetworkManager::PerformLagCompensatedCheck(u32 clientID, const LagCompensator::RewindCallback& callback)
    {
        NetworkServer* server = nullptr;
        Scene* scene = nullptr;
        {
            TUniqueLock<FMutex> lock(s_Mutex);
            server = s_Server.get();
            scene = s_ActiveScene;
        }

        // `!IsRunning()` matters as much as the null check: the rewind reads per-client
        // connection state (RTT) from the transport, which a stopped server no longer
        // has. Same guard GetClientPingMs uses.
        if (server == nullptr || !server->IsRunning() || scene == nullptr)
        {
            return false;
        }
        return s_ServerDriver.PerformLagCompensatedCheck(*scene, *server, clientID, callback);
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
