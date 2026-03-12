# OloEngine Networking Implementation Plan

## Overview

This document describes the full implementation plan for integrating multiplayer
networking into OloEngine using Valve's GameNetworkingSockets (GNS) library. The
plan is split into three sequential tasks, each building on the previous.

### Design Principles

- **Production-quality from the start** — GNS is chosen for its proven
  reliability in shipped titles, not as a prototype layer to be replaced later.
- **Use existing engine infrastructure** — FArchive (with `ArIsNetArchive`),
  the ported UE named-thread system (FNamedThreadManager, priority queues,
  cross-thread task dispatch), and the ECS component model are all leveraged
  directly.
- **ECS-idiomatic** — Networking state lives in components
  (`NetworkIdentityComponent`), not in external registries.
- **Dual scripting parity** — Both C# (Mono) and Lua (Sol2) bindings are
  provided for all user-facing networking APIs.

### Architecture Summary

```
┌─────────────────────────────────────────────────────────┐
│ OloEditor                                               │
│  ├── NetworkDebugPanel (connection stats, peer list)    │
│  └── SceneHierarchyPanel (NetworkIdentityComponent UI)  │
├─────────────────────────────────────────────────────────┤
│ OloEngine                                               │
│  ├── Networking/Core/                                   │
│  │    ├── NetworkManager (static Init/Shutdown)         │
│  │    ├── NetworkThread (named thread integration)      │
│  │    └── NetworkMessage (header + type enum)           │
│  ├── Networking/Transport/                              │
│  │    ├── NetworkConnection (GNS handle wrapper)        │
│  │    ├── NetworkServer (listen socket + poll groups)   │
│  │    └── NetworkClient (connect + state tracking)      │
│  ├── Networking/Replication/                            │
│  │    ├── ComponentReplicator (FArchive-based)          │
│  │    └── EntitySnapshot (capture + apply)              │
│  └── Scene/Components.h (NetworkIdentityComponent)      │
├─────────────────────────────────────────────────────────┤
│ Scripting                                               │
│  ├── C# — ScriptGlue.cpp + InternalCalls.cs            │
│  └── Lua — LuaScriptGlue.cpp (Sol2 usertype)           │
├─────────────────────────────────────────────────────────┤
│ Tests                                                   │
│  └── Networking/ (GoogleTest)                           │
└─────────────────────────────────────────────────────────┘
```

---

## Task 1 — Dependencies, Directory Structure, and NetworkManager

This task integrates the GameNetworkingSockets library and establishes the core
`NetworkManager` subsystem following OloEngine's static `Init`/`Shutdown`
pattern.

### 1.1 Add GameNetworkingSockets Dependency

**File:** `OloEngine/vendor/CMakeLists.txt`

Add a `FetchContent_Declare` for GameNetworkingSockets. Use **OpenSSL** as the
crypto backend (primary choice). If OpenSSL proves too painful to integrate via
CMake, fall back to **libsodium** (`USE_CRYPTO25519=libsodium`).

```cmake
# --- GameNetworkingSockets (Valve networking library) ---
FetchContent_Declare(
    GameNetworkingSockets
    GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
```

Configure build options to disable tests, examples, and unnecessary features:

```cmake
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
```

Add `GameNetworkingSockets` to the appropriate `FetchContent_MakeAvailable()`
call. GNS bundles its own protobuf — check whether it conflicts with any
existing dependency. If OpenSSL is not found by CMake's `find_package`, add a
`FetchContent_Declare` for OpenSSL or point to a pre-built distribution.

**File:** `OloEngine/CMakeLists.txt`

Add include directories and link the library:

```cmake
target_include_directories(OloEngine PUBLIC
    vendor/GameNetworkingSockets-src/include
)

target_link_libraries(OloEngine
    GameNetworkingSockets::shared    # or ::static depending on build config
)
```

Configure folder organization for IDE display:

```cmake
if(TARGET GameNetworkingSockets)
    set_target_properties(GameNetworkingSockets PROPERTIES FOLDER "Vendor")
endif()
```

### 1.2 Create Directory Structure

Create the networking source directories:

```
OloEngine/src/OloEngine/Networking/
OloEngine/src/OloEngine/Networking/Core/
OloEngine/src/OloEngine/Networking/Transport/
OloEngine/src/OloEngine/Networking/Replication/
```

### 1.3 Register Source Files in CMake

**File:** `OloEngine/src/CMakeLists.txt`

OloEngine uses **explicit file listing** (no globbing). Every new `.h` and
`.cpp` file must be manually added to the `set(SOURCES ...)` block. Add all
networking files in a clearly grouped section:

```cmake
# Networking subsystem
"OloEngine/Networking/Core/NetworkManager.h"
"OloEngine/Networking/Core/NetworkManager.cpp"
```

Additional files are listed in the tasks below. After each task, update this
list with every new file created.

Also register the FArchive files that exist but are not yet in the CMake sources
(they will be needed for network serialization):

```cmake
# Serialization (existing files not yet registered)
"OloEngine/Serialization/Archive.h"
"OloEngine/Serialization/MemoryLayout.h"
```

### 1.4 Implement NetworkManager

**Files:**
- `OloEngine/src/OloEngine/Networking/Core/NetworkManager.h`
- `OloEngine/src/OloEngine/Networking/Core/NetworkManager.cpp`

Follow the static `Init`/`Shutdown` pattern established by `AudioEngine` and
`Renderer`:

```cpp
class NetworkManager
{
  public:
    static bool Init();
    static void Shutdown();

    static bool IsInitialized();

  private:
    static bool s_Initialized;
};
```

**`Init()` implementation:**
- Call `GameNetworkingSockets_Init()` with a null allocator (use default).
- Register a GNS debug output callback that routes messages to the engine's
  logging system via `OLO_CORE_TRACE` / `OLO_CORE_WARN` / `OLO_CORE_ERROR`
  based on the GNS detail level.
- Wrap the body with `OLO_PROFILE_FUNCTION()`.
- Return `true` on success, `false` on failure.

**`Shutdown()` implementation:**
- Call `GameNetworkingSockets_Kill()` to clean up the library.
- Wrap with `OLO_PROFILE_FUNCTION()`.

### 1.5 Integrate into Application Lifecycle

**File:** `OloEngine/src/OloEngine/Core/Application.cpp`

Insert `NetworkManager::Init()` **after `AudioEngine::Init()`** and **before
`ScriptEngine::Init()`**. This ordering ensures:
- The renderer and audio are available (networking doesn't depend on them, but
  it matches the subsystem layering).
- Scripts can call networking functions during their initialization, since
  `NetworkManager` is already up.

**Constructor (in the `try` block, after the AudioEngine success check):**

```cpp
if (!NetworkManager::Init())
{
    OLO_CORE_CRITICAL("Failed to initialize NetworkManager!");
    AudioEngine::Shutdown();
    Renderer::Shutdown();
#ifdef OLO_DEBUG
    ShaderDebugger::GetInstance().Shutdown();
    GPUResourceInspector::GetInstance().Shutdown();
#endif
    m_Window.reset();
    s_Instance = nullptr;
    throw std::runtime_error("NetworkManager initialization failed");
}
```

**Destructor (reverse order, before `AudioEngine::Shutdown()`):**

```cpp
NetworkManager::Shutdown();
```

Also add `NetworkManager::Shutdown()` to the `catch` block for exception safety.

### 1.6 Update Umbrella Header

**File:** `OloEngine/src/OloEngine.h`

Add the networking public header:

```cpp
// Networking subsystem
#include "OloEngine/Networking/Core/NetworkManager.h"
```

### 1.7 Tests — Task 1

**Files:**
- `OloEngine/tests/Networking/NetworkManagerTest.cpp`

**File:** `OloEngine/tests/CMakeLists.txt` — add the test file.

Tests:
- `NetworkManagerTest.InitShutdownLifecycle` — Verify `Init()` returns true,
  `IsInitialized()` returns true, `Shutdown()` succeeds, `IsInitialized()`
  returns false.
- `NetworkManagerTest.DoubleInitIsIdempotent` — Calling `Init()` twice should
  not crash; second call returns true without re-initializing.
- `NetworkManagerTest.ShutdownWithoutInitIsHarmless` — Calling `Shutdown()`
  before `Init()` should not crash.

---

## Task 2 — Connection Management, Network Thread, and Message Transport

This task implements the connection lifecycle for client/server modes,
establishes the network thread using the engine's ported UE task system, and
builds the message transport layer using FArchive.

### 2.1 Connection Management

**Files:**
- `OloEngine/src/OloEngine/Networking/Transport/NetworkConnection.h`
- `OloEngine/src/OloEngine/Networking/Transport/NetworkConnection.cpp`

Wrap `HSteamNetConnection` handles with connection state tracking:

```cpp
enum class EConnectionState : u8
{
    None,
    Connecting,
    Connected,
    ClosedByPeer,
    ProblemDetectedLocally,
    FindingRoute
};

class NetworkConnection
{
  public:
    explicit NetworkConnection(HSteamNetConnection handle);

    [[nodiscard]] HSteamNetConnection GetHandle() const;
    [[nodiscard]] EConnectionState GetState() const;
    [[nodiscard]] u32 GetClientID() const;

    bool Send(const void* data, u32 size, i32 sendFlags);
    void Close(i32 reason = 0, const char* debug = "Closing");

  private:
    HSteamNetConnection m_Handle;
    EConnectionState m_State = EConnectionState::None;
    u32 m_ClientID = 0;
};
```

**Files:**
- `OloEngine/src/OloEngine/Networking/Transport/NetworkServer.h`
- `OloEngine/src/OloEngine/Networking/Transport/NetworkServer.cpp`

```cpp
class NetworkServer
{
  public:
    bool Start(u16 port);
    void Stop();
    void PollMessages();

    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] const auto& GetConnections() const;

  private:
    HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
    ISteamNetworkingSockets* m_Interface = nullptr;
    std::unordered_map<HSteamNetConnection, NetworkConnection> m_Connections;
};
```

**Files:**
- `OloEngine/src/OloEngine/Networking/Transport/NetworkClient.h`
- `OloEngine/src/OloEngine/Networking/Transport/NetworkClient.cpp`

```cpp
class NetworkClient
{
  public:
    bool Connect(const std::string& address, u16 port);
    void Disconnect();
    void PollMessages();

    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] EConnectionState GetState() const;

  private:
    HSteamNetConnection m_Connection = k_HSteamNetConnection_Invalid;
    ISteamNetworkingSockets* m_Interface = nullptr;
    EConnectionState m_State = EConnectionState::None;
};
```

In `NetworkManager`, implement a static connection status callback for
`SteamNetConnectionStatusChangedCallback_t` that dispatches events to the active
`NetworkServer` or `NetworkClient` instance.

### 2.2 Network Thread

**Files:**
- `OloEngine/src/OloEngine/Networking/Core/NetworkThread.h`
- `OloEngine/src/OloEngine/Networking/Core/NetworkThread.cpp`

Use the engine's **ported UE named-thread system** rather than a bare
`FThread` + `atomic_queue`:

**Step 1 — Register `NetworkThread` as a named thread:**

**File:** `OloEngine/src/OloEngine/Task/NamedThreads.h`

Add a new entry to the `ENamedThread` enum:

```cpp
enum class ENamedThread : i32
{
    GameThread = 0,
    RenderThread = 1,
    AudioThread = 2,
    NetworkThread = 3,   // <-- NEW

    Count,
    Invalid = -1
};
```

**Step 2 — Implement `NetworkThread`:**

```cpp
class NetworkThread
{
  public:
    static void Start(u32 tickRateHz = 60);
    static void Stop();
    static bool IsRunning();

  private:
    static void ThreadFunc();

    static FThread s_Thread;
    static std::atomic<bool> s_Running;
    static u32 s_TickRateHz;
};
```

In `ThreadFunc()`:
1. Call `FNamedThreadManager::Get().AttachToThread(ENamedThread::NetworkThread)`
   at the start of the thread to register it with the named thread system.
2. Enter a loop that:
   - Calls `RunCallbacks()` on the GNS interface to process connection events.
   - Drains incoming messages via `ReceiveMessagesOnPollGroup()` (server) or
     `ReceiveMessagesOnConnection()` (client).
   - Processes tasks queued to the network named thread via
     `FNamedThreadManager`.
   - Sleeps for the remainder of the tick interval.
3. Call `FNamedThreadManager::Get().DetachFromThread(ENamedThread::NetworkThread)`
   on exit.

Wrap inner loop body and message processing with
`OLO_PROFILE_SCOPE("NetworkThread::Tick")`.

**Step 3 — Inter-thread communication via named-thread task dispatch:**

Use the engine's named-thread priority queues for all cross-thread
communication. This is the same mechanism used by AudioEngine,
SoundGraphCache, and MeshColliderCache — tasks enqueued on a named thread's
queue are processed by that thread during its `ProcessTasks()` call.

**Network thread → Game thread:** When the network thread receives a message
or connection event, it enqueues a task on the game thread:

```cpp
// On the network thread, after receiving a message:
Tasks::EnqueueGameThreadTask(
    [clientID, payload = std::move(data)]()
    {
        NetworkManager::HandleIncomingMessage(clientID, payload);
    },
    "NetworkMsg::Incoming", /*bHighPriority=*/false);
```

**Game thread → Network thread:** When game code wants to send a message, it
enqueues a task on the network thread:

```cpp
// On the game thread, in NetworkManager::Send():
Tasks::EnqueueNetworkThreadTask(
    [connHandle, buffer = std::move(serializedData), flags]()
    {
        // Actual GNS send call runs on the network thread
        SteamNetworkingSockets()->SendMessageToConnection(
            connHandle, buffer.data(), static_cast<u32>(buffer.size()),
            flags, nullptr);
    },
    "NetworkMsg::Send");
```

The `EnqueueNetworkThreadTask` helper is defined alongside the existing
`EnqueueGameThreadTask` and `EnqueueAudioThreadTask` in `NamedThreads.h`
(see Step 4 below).

This approach has several advantages over a custom lock-free queue:
- **Proven** — the same dispatch mechanism is used by physics cooking,
  SoundGraph loading, and audio thread communication.
- **Priority support** — high-priority network events (e.g., disconnect) can
  use `bHighPriority = true` to jump ahead of normal-priority tasks.
- **No fixed-size limitation** — tasks are `std::function` wrappers stored
  in `TDeque`, so variable-size payloads are naturally supported via captures.
- **No separate drain loop** — game thread already calls
  `FNamedThreadManager::Get().ProcessTasks()` every frame in
  `Application::Run()`.

**Step 4 — Extend `EExtendedTaskPriority` and add dispatch helper:**

**File:** `OloEngine/src/OloEngine/Task/ExtendedTaskPriority.h`

Add `NetworkThread` priority entries alongside the existing `GameThread` and
`RenderThread` variants:

```cpp
enum class EExtendedTaskPriority : i8
{
    // ... existing entries ...

    // NetworkThread priorities
    NetworkThreadNormalPri,
    NetworkThreadHiPri,
    NetworkThreadNormalPriLocalQueue,
    NetworkThreadHiPriLocalQueue,

    Count
};
```

**File:** `OloEngine/src/OloEngine/Task/NamedThreads.h`

Update `GetNamedThread()` to handle the new priority values:

```cpp
case EExtendedTaskPriority::NetworkThreadNormalPri:
case EExtendedTaskPriority::NetworkThreadHiPri:
case EExtendedTaskPriority::NetworkThreadNormalPriLocalQueue:
case EExtendedTaskPriority::NetworkThreadHiPriLocalQueue:
    return ENamedThread::NetworkThread;
```

Update `IsHighPriority()` and `IsLocalQueue()` analogously.

Add a convenience dispatch helper alongside `EnqueueGameThreadTask` and
`EnqueueAudioThreadTask`:

```cpp
template<typename TaskBody>
void EnqueueNetworkThreadTask(TaskBody&& Task,
                              const char* DebugName = "NetworkThreadTask",
                              bool bHighPriority = false)
{
    EExtendedTaskPriority Priority = bHighPriority
        ? EExtendedTaskPriority::NetworkThreadHiPri
        : EExtendedTaskPriority::NetworkThreadNormalPri;
    FNamedThreadManager::Get().EnqueueTask(
        Priority, Forward<TaskBody>(Task), DebugName);
}
```

Integrate the thread lifecycle with `NetworkManager::Init()` and
`NetworkManager::Shutdown()` — start the thread during Init, join during
Shutdown.

### 2.3 Message Transport

**Files:**
- `OloEngine/src/OloEngine/Networking/Core/NetworkMessage.h`

Define a message header and type enumeration:

```cpp
enum class ENetworkMessageType : u16
{
    None = 0,
    Connect,
    Disconnect,
    Ping,
    Pong,
    EntitySnapshot,
    RPC,

    // Range for user-defined messages
    UserMessage = 1000
};

struct NetworkMessageHeader
{
    ENetworkMessageType Type = ENetworkMessageType::None;
    u32 Size = 0;  // Payload size (excluding header)
    u8 Flags = 0;  // Reliability, channel, etc.
};
```

**Serialization using FArchive:**

Use `FMemoryWriter` and `FMemoryReader` from `Archive.h` for message
serialization. These already support all primitive types, std::string, enums,
and byte-order handling via `operator<<`:

```cpp
// Sending:
std::vector<u8> buffer;
FMemoryWriter writer(buffer);
writer.ArIsNetArchive = true;  // Enable network-specific serialization
writer << header;
writer << payloadData;
connection.Send(buffer.data(), static_cast<u32>(buffer.size()), flags);

// Receiving:
FMemoryReader reader(receivedData, receivedSize);
reader.ArIsNetArchive = true;
NetworkMessageHeader header;
reader << header;
// Dispatch based on header.Type...
```

The `ArIsNetArchive` flag allows serialization code to branch on network context
(e.g., skipping editor-only data, applying bandwidth optimizations).

On `NetworkConnection`, implement send methods that wrap
`SendMessageToConnection` with configurable reliability flags
(`k_nSteamNetworkingSend_Reliable`, `k_nSteamNetworkingSend_Unreliable`, etc.).

Implement receive processing that deserializes incoming `NetworkMessageHeader`,
validates the type and size, then dispatches to registered per-type handlers.

### 2.4 Update CMake Sources

**File:** `OloEngine/src/CMakeLists.txt` — add all new files:

```cmake
"OloEngine/Networking/Core/NetworkThread.h"
"OloEngine/Networking/Core/NetworkThread.cpp"
"OloEngine/Networking/Core/NetworkMessage.h"
"OloEngine/Networking/Transport/NetworkConnection.h"
"OloEngine/Networking/Transport/NetworkConnection.cpp"
"OloEngine/Networking/Transport/NetworkServer.h"
"OloEngine/Networking/Transport/NetworkServer.cpp"
"OloEngine/Networking/Transport/NetworkClient.h"
"OloEngine/Networking/Transport/NetworkClient.cpp"
```

### 2.5 Tests — Task 2

**Files:**
- `OloEngine/tests/Networking/NetworkMessageTest.cpp`
- `OloEngine/tests/Networking/NetworkThreadDispatchTest.cpp`

**File:** `OloEngine/tests/CMakeLists.txt` — add the test files.

Tests:
- `NetworkMessageTest.HeaderSerializationRoundtrip` — Write a
  `NetworkMessageHeader` to `FMemoryWriter`, read it back with `FMemoryReader`,
  verify all fields match.
- `NetworkMessageTest.PrimitivePayloadRoundtrip` — Serialize a struct with
  mixed types (u32, f32, std::string) through FArchive, verify roundtrip.
- `NetworkMessageTest.EmptyPayload` — Zero-size message roundtrip.
- `NetworkMessageTest.MaxSizePayload` — Verify behavior at boundary sizes.
- `NetworkThreadDispatchTest.EnqueueNetworkThreadTask` — Enqueue a task to
  `ENamedThread::NetworkThread`, verify it executes on the correct thread.
- `NetworkThreadDispatchTest.EnqueueGameThreadFromNetwork` — Simulate a
  network thread dispatching a callback to the game thread via
  `EnqueueGameThreadTask`, verify it runs during `ProcessTasks()`.

---

## Task 3 — Entity Replication, Editor Integration, and Scripting Bindings

This task establishes the component-based entity replication system, adds editor
UI, and exposes networking to both C# and Lua scripts.

### 3.1 NetworkIdentityComponent

**File:** `OloEngine/src/OloEngine/Scene/Components.h`

Add the component. The existing `IDComponent::UUID` already serves as the
globally unique entity identifier. `NetworkIdentityComponent` adds only the
multiplayer-specific data: **who owns the entity** and **who has authority**.

```cpp
enum class ENetworkAuthority : u8
{
    Server = 0,  // Server is authoritative (default)
    Client,      // Owning client is authoritative
    Shared       // Both can modify (cooperative)
};

struct NetworkIdentityComponent
{
    u32 OwnerClientID = 0;
    ENetworkAuthority Authority = ENetworkAuthority::Server;
    bool IsReplicated = true;

    NetworkIdentityComponent() = default;
    NetworkIdentityComponent(const NetworkIdentityComponent&) = default;
};
```

Note: No separate "network entity ID" is introduced. The existing
`IDComponent::UUID` (a `u64`) is used as the network identifier directly,
avoiding redundant IDs.

**File:** `OloEngine/src/OloEngine/Scene/Components.h` — add to `AllComponents`:

```cpp
using AllComponents = ComponentGroup<
    TransformComponent,
    // ... existing components ...
    StreamingVolumeComponent,
    NetworkIdentityComponent>;  // <-- NEW
```

**File:** `OloEngine/src/OloEngine/Scene/Scene.cpp` — add the
`OnComponentAdded` specialization:

```cpp
template<>
void Scene::OnComponentAdded<NetworkIdentityComponent>(
    Entity entity, NetworkIdentityComponent& component) {}
```

### 3.2 Scene Serialization

**File:** `OloEngine/src/OloEngine/Scene/SceneSerializer.cpp`

Add serialization following the existing pattern:

**Serialize (in `SerializeEntity`):**

```cpp
if (entity.HasComponent<NetworkIdentityComponent>())
{
    out << YAML::Key << "NetworkIdentityComponent";
    out << YAML::BeginMap;

    auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
    out << YAML::Key << "OwnerClientID" << YAML::Value << nic.OwnerClientID;
    out << YAML::Key << "Authority" << YAML::Value
        << static_cast<int>(nic.Authority);
    out << YAML::Key << "IsReplicated" << YAML::Value << nic.IsReplicated;

    out << YAML::EndMap;
}
```

**Deserialize (in `DeserializeEntities`):**

```cpp
if (const auto& networkIdentityComponent =
        entity["NetworkIdentityComponent"])
{
    auto& nic = deserializedEntity
                    .AddComponent<NetworkIdentityComponent>();
    TrySet(nic.OwnerClientID,
           networkIdentityComponent["OwnerClientID"]);
    TrySetEnum(nic.Authority,
               networkIdentityComponent["Authority"]);
    TrySet(nic.IsReplicated,
           networkIdentityComponent["IsReplicated"]);
}
```

### 3.3 Component Replication Serialization

**Files:**
- `OloEngine/src/OloEngine/Networking/Replication/ComponentReplicator.h`
- `OloEngine/src/OloEngine/Networking/Replication/ComponentReplicator.cpp`

Implement FArchive-based component serialization for network transport. Use
`FMemoryWriter` / `FMemoryReader` with `ArIsNetArchive = true`:

```cpp
class ComponentReplicator
{
  public:
    // Serialize a component into the archive
    template<typename T>
    static void Serialize(FArchive& ar, T& component);

    // Register replication serializers for built-in components
    static void RegisterDefaults();
};
```

Create replication serializers for core components:
- `TransformComponent` — Translation, Rotation, Scale
- `Rigidbody2DComponent` — Body type, velocity (runtime state)
- `Rigidbody3DComponent` — Body type, mass, velocity (runtime state)

Branching on `ArIsNetArchive` allows these serializers to skip data that is only
relevant for disk persistence (e.g., editor-only metadata).

**Initial implementation uses full-state serialization.** Delta/dirty-flag
tracking is deferred to a future optimization pass — get full snapshots working
first.

### 3.4 Entity Snapshot System

**Files:**
- `OloEngine/src/OloEngine/Networking/Replication/EntitySnapshot.h`
- `OloEngine/src/OloEngine/Networking/Replication/EntitySnapshot.cpp`

```cpp
class EntitySnapshot
{
  public:
    // Capture: iterate all entities with NetworkIdentityComponent,
    // serialize their replicated components into a buffer.
    static std::vector<u8> Capture(Scene& scene);

    // Apply: deserialize snapshot data and update entity components.
    static void Apply(Scene& scene, const std::vector<u8>& data);
};
```

**Capture** iterates the EnTT registry view for
`NetworkIdentityComponent` + `TransformComponent` (and other replicated
components), serializing each entity's UUID followed by its component data using
`FMemoryWriter`.

**Apply** reads the buffer with `FMemoryReader`, looks up each entity by UUID
via `Scene::GetEntityByUUID()`, and writes component values back.

Add `ENetworkMessageType::EntitySnapshot` handling to the message dispatch
system from Task 2.

### 3.5 C# Scripting Bindings

**File:** `OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp`

Add internal call implementations:

```cpp
static bool Network_IsServer()
{
    return NetworkManager::IsServer();
}

static bool Network_IsClient()
{
    return NetworkManager::IsClient();
}

static bool Network_IsConnected()
{
    return NetworkManager::IsConnected();
}

static bool Network_Connect(MonoString* address, u16 port)
{
    std::string addr = Utils::MonoStringToString(address);
    return NetworkManager::Connect(addr, port);
}

static void Network_Disconnect()
{
    NetworkManager::Disconnect();
}

static bool Network_StartServer(u16 port)
{
    return NetworkManager::StartServer(port);
}

static void Network_StopServer()
{
    NetworkManager::StopServer();
}
```

Register in `ScriptGlue::RegisterFunctions()`:

```cpp
///////////////////////////////////////////////////////////////
// Networking ////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
OLO_ADD_INTERNAL_CALL(Network_IsServer);
OLO_ADD_INTERNAL_CALL(Network_IsClient);
OLO_ADD_INTERNAL_CALL(Network_IsConnected);
OLO_ADD_INTERNAL_CALL(Network_Connect);
OLO_ADD_INTERNAL_CALL(Network_Disconnect);
OLO_ADD_INTERNAL_CALL(Network_StartServer);
OLO_ADD_INTERNAL_CALL(Network_StopServer);
```

**File:** `OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs`

Add extern declarations:

```csharp
#region Networking
[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static bool Network_IsServer();

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static bool Network_IsClient();

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static bool Network_IsConnected();

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static bool Network_Connect(string address, ushort port);

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static void Network_Disconnect();

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static bool Network_StartServer(ushort port);

[MethodImplAttribute(MethodImplOptions.InternalCall)]
internal extern static void Network_StopServer();
#endregion
```

**File:** `OloEngine-ScriptCore/src/OloEngine/NetworkManager.cs`

Create wrapper class:

```csharp
namespace OloEngine
{
    public static class NetworkManager
    {
        public static bool IsServer => InternalCalls.Network_IsServer();
        public static bool IsClient => InternalCalls.Network_IsClient();
        public static bool IsConnected => InternalCalls.Network_IsConnected();

        public static bool Connect(string address, ushort port)
            => InternalCalls.Network_Connect(address, port);

        public static void Disconnect()
            => InternalCalls.Network_Disconnect();

        public static bool StartServer(ushort port)
            => InternalCalls.Network_StartServer(port);

        public static void StopServer()
            => InternalCalls.Network_StopServer();
    }
}
```

### 3.6 Lua Scripting Bindings

**File:** `OloEngine/src/OloEngine/Scripting/Lua/LuaScriptGlue.cpp`

Add bindings in `LuaScriptGlue::RegisterAllTypes()` following the existing Sol2
usertype pattern:

```cpp
// --- NetworkIdentityComponent ---
lua.new_usertype<NetworkIdentityComponent>("NetworkIdentityComponent",
    "ownerClientID", &NetworkIdentityComponent::OwnerClientID,
    "authority", &NetworkIdentityComponent::Authority,
    "isReplicated", &NetworkIdentityComponent::IsReplicated);

// --- NetworkManager (static functions as table) ---
auto network = lua.create_named_table("Network");
network.set_function("isServer", &NetworkManager::IsServer);
network.set_function("isClient", &NetworkManager::IsClient);
network.set_function("isConnected", &NetworkManager::IsConnected);
network.set_function("connect", &NetworkManager::Connect);
network.set_function("disconnect", &NetworkManager::Disconnect);
network.set_function("startServer", &NetworkManager::StartServer);
network.set_function("stopServer", &NetworkManager::StopServer);
```

### 3.7 Editor Integration — SceneHierarchyPanel

**File:** `OloEditor/src/Panels/SceneHierarchyPanel.cpp`

Add a `DrawComponent` call for `NetworkIdentityComponent` following the existing
pattern:

```cpp
DrawComponent<NetworkIdentityComponent>(
    "Network Identity", entity, [](auto& component)
    {
        // Owner Client ID (read-only at runtime, editable in editor)
        ImGui::DragScalar("Owner Client ID", ImGuiDataType_U32,
                          &component.OwnerClientID);

        // Authority dropdown
        const char* authorityStrings[] = { "Server", "Client", "Shared" };
        int currentAuthority = static_cast<int>(component.Authority);
        if (ImGui::Combo("Authority", &currentAuthority,
                         authorityStrings, IM_ARRAYSIZE(authorityStrings)))
        {
            component.Authority =
                static_cast<ENetworkAuthority>(currentAuthority);
        }

        // Replicated checkbox
        ImGui::Checkbox("Is Replicated", &component.IsReplicated);
    });
```

### 3.8 Editor Integration — NetworkDebugPanel

**Files:**
- `OloEditor/src/Panels/NetworkDebugPanel.h`
- `OloEditor/src/Panels/NetworkDebugPanel.cpp`

Create a debug panel showing:
- Connection state (Disconnected / Connecting / Connected / Server Listening)
- Local mode (Server / Client / None)
- Connected peers list with client IDs and ping
- Buttons: Start Server, Connect, Disconnect
- Network statistics: messages sent/received per second, bandwidth usage

**File:** `OloEditor/src/EditorLayer.h`

Add panel include, member, and toggle:

```cpp
#include "Panels/NetworkDebugPanel.h"

// In class members:
NetworkDebugPanel m_NetworkDebugPanel;
bool m_ShowNetworkDebug = false;
```

**File:** `OloEditor/src/EditorLayer.cpp`

Add menu item in `UI_MenuBar()` and render call in `UI_ChildPanels()`:

```cpp
// In UI_MenuBar, under a "Networking" or existing "View" menu:
if (ImGui::MenuItem("Network Debug", nullptr, m_ShowNetworkDebug))
    m_ShowNetworkDebug = !m_ShowNetworkDebug;

// In UI_ChildPanels:
if (m_ShowNetworkDebug)
    m_NetworkDebugPanel.OnImGuiRender();
```

### 3.9 Update CMake Sources

**File:** `OloEngine/src/CMakeLists.txt` — add all new files:

```cmake
"OloEngine/Networking/Replication/ComponentReplicator.h"
"OloEngine/Networking/Replication/ComponentReplicator.cpp"
"OloEngine/Networking/Replication/EntitySnapshot.h"
"OloEngine/Networking/Replication/EntitySnapshot.cpp"
```

**File:** `OloEditor/src/CMakeLists.txt` (or equivalent) — add editor files:

```cmake
"Panels/NetworkDebugPanel.h"
"Panels/NetworkDebugPanel.cpp"
```

### 3.10 Tests — Task 3

**Files:**
- `OloEngine/tests/Networking/ComponentReplicatorTest.cpp`
- `OloEngine/tests/Networking/EntitySnapshotTest.cpp`
- `OloEngine/tests/Networking/NetworkIdentityComponentTest.cpp`

**File:** `OloEngine/tests/CMakeLists.txt` — add the test files.

Tests:
- `ComponentReplicatorTest.TransformRoundtrip` — Serialize a
  `TransformComponent` with known values via `FMemoryWriter`, deserialize via
  `FMemoryReader`, verify all fields match.
- `ComponentReplicatorTest.ArIsNetArchiveFlag` — Verify that the archive has
  `ArIsNetArchive == true` during network serialization.
- `EntitySnapshotTest.CaptureAndApplyRoundtrip` — Create a scene with
  entities that have `NetworkIdentityComponent` + `TransformComponent`, capture
  a snapshot, modify the transform, apply the snapshot, verify restoration.
- `EntitySnapshotTest.EmptySceneProducesEmptySnapshot` — Snapshot of a scene
  with no networked entities produces minimal data.
- `EntitySnapshotTest.OnlyReplicatedEntities` — Entities with
  `IsReplicated = false` are excluded from snapshots.
- `NetworkIdentityComponentTest.DefaultValues` — Verify default construction.
- `NetworkIdentityComponentTest.CopySemantics` — Verify copy constructor.

---

## Cross-Cutting Concerns

### Tracy Profiling

All public methods in `NetworkManager`, `NetworkServer`, `NetworkClient`,
`NetworkThread`, `ComponentReplicator`, and `EntitySnapshot` must be wrapped
with `OLO_PROFILE_FUNCTION()`. The network thread loop body should use
`OLO_PROFILE_SCOPE("NetworkThread::Tick")`.

### Pre-Commit

After each task's implementation, run `pre-commit run --all-files` before
committing to ensure style compliance and avoid CI failures.

### File Summary

All files created or modified across the three tasks:

**New Engine Files (OloEngine/src/OloEngine/):**

| File | Task |
|------|------|
| Networking/Core/NetworkManager.h | 1 |
| Networking/Core/NetworkManager.cpp | 1 |
| Networking/Core/NetworkThread.h | 2 |
| Networking/Core/NetworkThread.cpp | 2 |
| Networking/Core/NetworkMessage.h | 2 |
| Networking/Transport/NetworkConnection.h | 2 |
| Networking/Transport/NetworkConnection.cpp | 2 |
| Networking/Transport/NetworkServer.h | 2 |
| Networking/Transport/NetworkServer.cpp | 2 |
| Networking/Transport/NetworkClient.h | 2 |
| Networking/Transport/NetworkClient.cpp | 2 |
| Networking/Replication/ComponentReplicator.h | 3 |
| Networking/Replication/ComponentReplicator.cpp | 3 |
| Networking/Replication/EntitySnapshot.h | 3 |
| Networking/Replication/EntitySnapshot.cpp | 3 |

**New Editor Files (OloEditor/src/):**

| File | Task |
|------|------|
| Panels/NetworkDebugPanel.h | 3 |
| Panels/NetworkDebugPanel.cpp | 3 |

**New Scripting Files:**

| File | Task |
|------|------|
| OloEngine-ScriptCore/src/OloEngine/NetworkManager.cs | 3 |

**New Test Files (OloEngine/tests/):**

| File | Task |
|------|------|
| Networking/NetworkManagerTest.cpp | 1 |
| Networking/NetworkMessageTest.cpp | 2 |
| Networking/NetworkThreadDispatchTest.cpp | 2 |
| Networking/ComponentReplicatorTest.cpp | 3 |
| Networking/EntitySnapshotTest.cpp | 3 |
| Networking/NetworkIdentityComponentTest.cpp | 3 |

**Modified Files:**

| File | Task | Change |
|------|------|--------|
| OloEngine/vendor/CMakeLists.txt | 1 | Add GNS FetchContent |
| OloEngine/CMakeLists.txt | 1 | Link GNS library |
| OloEngine/src/CMakeLists.txt | 1-3 | Register all new source files |
| OloEngine/src/OloEngine.h | 1 | Add networking include |
| OloEngine/src/OloEngine/Core/Application.cpp | 1 | Init/Shutdown calls |
| OloEngine/src/OloEngine/Task/NamedThreads.h | 2 | Add NetworkThread enum + EnqueueNetworkThreadTask helper |
| OloEngine/src/OloEngine/Task/ExtendedTaskPriority.h | 2 | Add NetworkThread priority entries |
| OloEngine/src/OloEngine/Scene/Components.h | 3 | Add component + AllComponents |
| OloEngine/src/OloEngine/Scene/Scene.cpp | 3 | OnComponentAdded specialization |
| OloEngine/src/OloEngine/Scene/SceneSerializer.cpp | 3 | Serialize/Deserialize |
| OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp | 3 | Internal calls |
| OloEngine/src/OloEngine/Scripting/Lua/LuaScriptGlue.cpp | 3 | Sol2 bindings |
| OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs | 3 | Extern declarations |
| OloEditor/src/EditorLayer.h | 3 | Panel member |
| OloEditor/src/EditorLayer.cpp | 3 | Panel toggle + render |
| OloEditor/src/Panels/SceneHierarchyPanel.cpp | 3 | Component properties UI |
| OloEngine/tests/CMakeLists.txt | 1-3 | Register test files |
