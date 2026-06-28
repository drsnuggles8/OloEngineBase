# Dead-code analysis (LOC reduction)

Working inventory of unused / dead / superseded code in OloEngine, produced
for the "project is way too large" LOC-reduction effort. **This document is
an analysis aid, not a deletion order** — each item carries a verdict and the
evidence behind it so a human can decide per-item.

> Key principle (per review): *dead ≠ deletable*. Some unused code is a
> deliberate **scaffold for a feature the engine/editor genuinely intends to
> ship** (e.g. event payloads that pair with a fully-implemented system). Those
> are marked **KEEP / FINISH-WIRE**, not REMOVE.

Generated on branch `refactor/dead-code-loc-reduction`, based on master
`c9420b84`.

---

## Status (as of pass 3)

**Removed so far — ≈ 5,200 LOC** (build green, full suite 2766/2766):

- **Pass 1 / 1b — native cruft (committed `a116130b`+`dc4d5259`, ≈ 1,000 LOC):**
  10 empty stub files (`Asset/Serializers/{Material,Mesh,Shader,Texture}Serializer.{h,cpp}`,
  `Scripting/C#/ScriptUtils.{h,cpp}`), `Renderer/RenderState.h`,
  `Renderer/Commands/CommandMemoryManager.{cpp,h}`, `Core/YAMLConverters.cpp`,
  `Asset/Serializers/AudioFileSerializer.cpp`, `Core/PerformanceProfiler.cpp`.
- **Pass 2 — UE-ported dedup (staged, ≈ 3,400 LOC):** `Containers/ContainersFwd.h`,
  the dead allocator island `Memory/ConcurrentLinearAllocator.h` +
  `Memory/TypeTraits.h` + `Task/LocalWorkQueue.h`, and `HAL/ParkingLot.cpp`'s
  duplicate `FWordMutex` (now uses `Threading/WordMutex.h`, making it live).
- **Bonus fix:** reference-counted `AssetImporter` + `PlaceholderAssetManager`
  to fix the pre-existing `CinematicAssetPlaybackTest` flakiness (see action log).
- **Pass 3 — remaining 🔵 tier, per-item review (≈ 800 LOC):** removed the unused
  `Async/QueuedThreadPool` layer (`QueuedThreadPool.{cpp,h}` + `QueuedWork.h`), the
  3 Threading mutex/lock variants, and the dead `LockFreeList.cpp` `#if 0` block.

**Deliberately kept** (verified dead, but wanted): `Containers/LinkedList.h` +
`Containers/Queue.h` (useful containers, forward-looking); `Memory/MemStackUtility.h`
(typo fixed; awaits broader `FMemStack` adoption); `Memory/PlatformMemory.h` (added a
port-platform-backends TODO); `HAL/ThreadManager.h` (backs thread-inspector issue #282);
`Algo/HeapSort|Heapify|IsHeap.h` (public API over the live `BinaryHeap.h`). Plus the
🟡 set: Quest/Inventory event payloads, `JoltMaterial`, the fmt formatters, `GamepadEvent`.

---

## Methodology

1. **Include-graph reachability.** Parsed every `#include` across
   `OloEngine/src`, `OloEngine/tests`, `OloEditor`, `OloRuntime`, `OloServer`,
   `OloEngine-ScriptCore` (vendor excluded). A header is *unreachable* — and
   therefore dead — if no `.cpp` translation unit (or the PCH) can reach it
   transitively. C++ requires inclusion to use a header's symbols, so an
   unreachable header's symbols are provably unused.
2. **Symbol-level cross-check.** For each candidate, grepped the whole repo for
   its top-level symbols (excluding the defining file) to confirm zero external
   references.
3. **Orphaned translation units.** `OloEngine/src/CMakeLists.txt` uses an
   **explicit source list** (no globbing). A `.cpp` on disk but absent from that
   list is not compiled; if its symbols were needed elsewhere the project would
   fail to *link*. Such files are provably dead.
4. **Provenance + intent.** Tagged each item Unreal-ported vs native, checked
   `docs/`, `docs/adr/`, `CLAUDE.md`, and sibling live code to judge whether an
   unused item is *redundant* (a live twin exists), *speculative* (ported, never
   used, no plan), or *scaffolding for a real feature*.
5. Commented-out-code and `#if 0` sweeps across `OloEngine/src` + `OloEditor/src`.

Caveat: the header-reachability pass treats *all* `.cpp` as roots, including
uncompiled orphans. So a header reachable only through an uncompiled orphan
`.cpp` (e.g. `CommandMemoryManager.h` via its own orphan `.cpp`) will *not*
appear in the unreachable-header list; the orphaned-TU pass (3) catches those.

---

## Verdict legend

| Verdict | Meaning |
|---|---|
| **REMOVE** | Genuine cruft: superseded, duplicate of a live twin, abandoned, or empty. Safe to delete. |
| **KEEP / FINISH-WIRE** | Unused, but a deliberate scaffold for a real, implemented subsystem. Delete would remove intended feature surface. |
| **UE-PORTED — DECISION** | Unreal-ported infra with no current use *and* a live equivalent already chosen. Not documented as a kept foundation. Removal is reasonable but is a deliberate "do we keep the ported toolkit?" call. |
| **SKIP** | Touches an actively-developed area (e.g. Audio/SoundGraph) — leave alone to avoid collisions. |

---

## 🟢 REMOVE — native cruft (superseded / duplicate / abandoned / empty)

> ✅ **All of these were removed in pass 1 / 1b (committed).** Table retained as the record.

| Item | LOC | In build? | Evidence | Verdict |
|---|---|---|---|---|
| `Asset/Serializers/MaterialSerializer.{h,cpp}` | 0 | yes (`.cpp`) | Empty (0-byte) stubs. The real serialization lives in `AssetSerializer.{h,cpp}`. | REMOVE |
| `Asset/Serializers/MeshSerializer.{h,cpp}` | 0 | yes | Empty stubs; real `MeshSerializer` class is `AssetSerializer.h:288`. | REMOVE |
| `Asset/Serializers/ShaderSerializer.{h,cpp}` | 0 | yes | Empty stubs. | REMOVE |
| `Asset/Serializers/TextureSerializer.{h,cpp}` | 0 | yes | Empty stubs; real `TextureSerializer` class is `AssetSerializer.h:122`. | REMOVE |
| `Scripting/C#/ScriptUtils.{h,cpp}` | 0 | yes | Empty (0-byte) stubs. | REMOVE ✅ |
| `Renderer/RenderState.h` | 237 | no | Superseded early render-state design (`StateType`/`RenderStateBase`/`BlendState`/…). Unreachable; `Renderer/Commands/` has its own state representation. | REMOVE |
| `Renderer/Commands/CommandMemoryManager.{cpp,h}` | 440 | no | Not compiled; `FrameResourceManager.h:21` comment: *"replaces the old FrameResourceManager + CommandMemoryManager split."* Only a comment references the name. | REMOVE |
| `Core/YAMLConverters.cpp` | 169 | no | Not compiled; an out-of-line duplicate of the **inline** converters in the live `YAMLConverters.h` (included by 5 live TUs). | REMOVE |
| `Asset/Serializers/AudioFileSerializer.cpp` | 142 | no | Not compiled; duplicate of the live `AudioFileSourceSerializer` impl in `AssetSerializer.cpp` (asset metadata, **not** SoundGraph). Registered at `AssetImporter.cpp:65`. | REMOVE |
| `Core/PerformanceProfiler.cpp` | 14 | no | Not compiled; a byte-identical duplicate of the 5-line `GetGlobalPerformanceProfiler()` accessor that lives in `Application.cpp:420`. See note below. | REMOVE ✅ |
| `Memory/LockFreeList.cpp` `#if 0` block (lines 63–72) | ~9 | n/a | Explicitly "kept for reference but disabled." **But `LockFreeList` is UE-ported** → grouped with the UE decision, not the native cruft pass. | UE-PORTED — DECISION |

**Commented-out-code / `#if 0` sweep:** otherwise **clean** — no large dead
comment blocks or macro-guarded-never-defined blocks in engine/editor `.cpp`/`.h`.

### Note — the PerformanceProfiler "duplicate"
There is **no** profiler implementation in `Application.cpp`. The
`PerformanceProfiler` *class* lives in `Core/PerformanceProfiler.h:18` and is
**owned by `Application`** (retrieved via `app->GetPerformanceProfiler()`). The
only thing in `Application.cpp` is a 5-line free helper
`GetGlobalPerformanceProfiler()` (line 420) that forwards to that
Application-owned instance — it lives there because it needs the full
`Application` definition, and it backs the `OLO_PERF_*` macros so they don't
need an `Application` handle. The orphaned `Core/PerformanceProfiler.cpp` holds
a **byte-identical, uncompiled copy of only that one function**. So the `.cpp`
is a pure dead duplicate; the class and the live accessor stay.

---

## 🟡 KEEP / FINISH-WIRE — scaffolding for real, shipped features

| Item | LOC | Why keep |
|---|---|---|
| `Gameplay/Quest/QuestEvents.h` | 61 | ✅ **WIRED (2026-06-04).** POD notification payloads (`QuestStartedEvent`, `ObjectiveProgressEvent`, …) for the **fully-implemented Quest system**. Now published via the new per-Scene `GameplayEventBus` from `QuestSystem`'s entity-aware service methods (see action log). |
| `Gameplay/Inventory/InventoryEvents.h` | 43 | ✅ **WIRED (2026-06-04).** Event payloads (`ItemAddedEvent`, `ItemEquippedEvent`, …) for the **real Inventory system**. Now published via the `GameplayEventBus` from `InventorySystem`'s service methods + the auto-pickup path. |
| `Physics3D/JoltMaterial.{cpp,h}` | 153 | `JPH::PhysicsMaterial` subclass with a friction policy — plausible per-material physics feature for the live Jolt subsystem. Currently unused/uncompiled. |
| `Core/FilesystemUtils.h` | 33 | fmt log-formatter for `std::filesystem::path`. Unused but harmless and reusable. |
| `Math/GLMFormatter.h` | 22 | fmt log-formatter for glm types. Unused but harmless and reusable. |
| `Events/GamepadEvent.h` | 120 | **Borderline.** Gamepad support exists but is *manager/polling*-based (`Core/Gamepad`, `GamepadManager`), so these `Event`-derived classes are an unused *alternative* mechanism. Event-driven input is a plausible want — decide keep vs remove. |

---

## 🔵 UE-PORTED dead toolkit — DECISION (your call)

None of these are documented as a kept "foundation" (no mention in `docs/`,
`docs/adr/`, or `CLAUDE.md`), and **every one already has a live, used
equivalent** — i.e. the engine has effectively chosen a different
implementation. They read as redundant/speculative port leftovers rather than a
toolkit being grown into. Listed for a deliberate keep-or-remove decision.

### Redundant (an exact/near-exact live twin is in active use)

| Header | LOC | Status | Live twin in use |
|---|---|---|---|
| `Memory/ConcurrentLinearAllocator.h` | 1284 | ✅ removed (pass 2) | Was a *broken* dup of `Experimental/ConcurrentLinearAllocator.h` (never compiled). |
| `Memory/TypeTraits.h` | 1364 | ✅ removed (pass 2) | Partial dup of the live `Templates/UnrealTypeTraits.h` (24 users). |
| `Threading/WordMutex.h` | 265 | ✅ now **live** (pass 2) | `HAL/ParkingLot.cpp` now uses it instead of a private copy — no longer dead. |
| `Containers/ContainersFwd.h` | 254 | ✅ removed (pass 2) | The singular `Containers/ContainerFwd.h` (included by `Array.h`). |
| `Containers/Queue.h` | 341 | 🟡 KEPT | Useful generic `TQueue`; live fast paths are `SpscQueue.h` / `MpscQueue.h` / `Deque.h`. |
| `Memory/PlatformMemory.h` | 17 | 🟡 KEPT | Kept as the per-OS seam; added a `TODO(platform-memory)` to port platform backends. |

### Speculative (ported, zero use, no live consumer, no documented plan)

| Header / TU | LOC | Status | Live alternative |
|---|---|---|---|
| `Task/LocalWorkQueue.h` | 285 | ✅ removed (pass 2) | Sole includer of the dead `Memory/ConcurrentLinearAllocator.h` (removed with the island). |
| `Async/QueuedThreadPool.{cpp,h}` (+ `QueuedWork.h`) | 620+ | ✅ removed (pass 3) | Abandoned UE thread-pool layer; engine uses `LowLevelTasks::FScheduler` directly. |
| `Threading/RecursiveWordMutex.h` | 96 | ✅ removed (pass 3) | Superseded by merged threading-unification (`FRecursiveMutex`). |
| `Threading/TransactionallySafeMutex.h` | 38 | ✅ removed (pass 3) | Inert AutoRTFM-compat aliases to `FMutex` etc. |
| `Threading/IntrusiveUniqueLock.h` | 37 | ✅ removed (pass 3) | Superseded by `UniqueLock.h` / `SharedLock.h`. |
| `Memory/LockFreeList.cpp` `#if 0` block | ~18 | ✅ removed (pass 3) | Disabled stat-tracking reference block (in-file). |
| `Containers/LinkedList.h` | 783 | 🟡 KEPT | Useful `TLinkedList`/`TDoubleLinkedList`; no clean current use-site (forward-looking). |
| `HAL/ThreadManager.h` | 363 | 🟡 KEPT | Backs thread-inspector **issue #282**. Live thread mgmt is `Tasks::FNamedThreadManager`. |
| `Algo/HeapSort.h` / `Heapify.h` / `IsHeap.h` | 245 | 🟡 KEPT | Public API over the live `Algo/BinaryHeap.h`. |
| `Memory/MemStackUtility.h` | 147 | 🟡 KEPT | Typo fixed; awaits broader `FMemStack` adoption. |

**Dead island note (✅ done):** `Task/LocalWorkQueue.h` → `Memory/ConcurrentLinearAllocator.h`
→ `Memory/TypeTraits.h` formed one self-contained dead cluster (~2,933 LOC), removed
together in pass 2 — which also eliminated the ODR hazard with the live
`Experimental/` allocator.

---

## ⛔ SKIP — active colleague areas

| Item | LOC | Why |
|---|---|---|
| `Audio/SoundGraph/Nodes/NodeTypeImpls.h` | 47 | SoundGraph under active development (several `feature/soundgraph-*` branches). |
| `Audio/AudioCallback.{h,cpp}` | 548 | `AudioCallback.h` is included by `Audio/SoundGraph/NodeProcessor.h` (active area). Header is live even though the `.cpp` is uncompiled. |

---

## Totals (for reference)

- Unreachable engine headers (reachability pass): **28 headers ≈ 6,082 LOC.**
- Dead/orphaned `.cpp` translation units (not compiled): `CommandMemoryManager.cpp`
  (314) + `QueuedThreadPool.cpp` (264) + `YAMLConverters.cpp` (169) +
  `AudioFileSerializer.cpp` (142) + `PerformanceProfiler.cpp` (14) +
  `AudioCallback.cpp` (165, SKIP) + `JoltMaterial.cpp` (10, KEEP).
- Empty stub files: **10** (8 serializer + 2 ScriptUtils).

### LOC by decision band (✅ = done)
- 🟢 Native cruft: ~1,000 LOC + 10 empty files — ✅ **removed** (pass 1/1b).
- 🔵 UE-ported redundant: ~3,525 LOC — ✅ **~3,167 removed** (allocator island +
  ContainersFwd; WordMutex now live); **remaining: `Containers/Queue.h` 341 +
  `Memory/PlatformMemory.h` 17**.
- 🔵 UE-ported speculative: ~2,600 LOC — ✅ `LocalWorkQueue.h` 285 removed;
  **remaining ~2,338**: LinkedList 783, Async/QueuedThreadPool 620, ThreadManager
  363, Algo trio 245, MemStackUtility 147, the 3 Threading mutex/lock variants 171,
  LockFreeList `#if 0` ~9.
- 🟡 Keep / finish-wire: ~432 LOC (do **not** delete).

---

## Action log

- **2026-06-03 — pass 1 (this branch):** deleted the confirmed native-cruft
  subset — the 4 empty serializer stub pairs, `Renderer/RenderState.h`,
  `Core/YAMLConverters.cpp`, `Renderer/Commands/CommandMemoryManager.{cpp,h}`,
  `Asset/Serializers/AudioFileSerializer.cpp` — and removed the 8 empty-serializer
  entries from `OloEngine/src/CMakeLists.txt`. Build + full test suite green
  (2765 passed, 0 failed).
- **2026-06-03 — pass 1b:** also deleted `Core/PerformanceProfiler.cpp` (dead
  duplicate of the 5-line accessor in `Application.cpp`) and the two empty
  `Scripting/C#/ScriptUtils.{h,cpp}` stubs (+ their 2 CMake entries).
- **2026-06-03 — pass 2 (UE-ported "bucket A" dedup, ~3,300 LOC):**
  - `Containers/ContainersFwd.h` (254) deleted — the live fwd-decl header is the
    *singular* `ContainerFwd.h` (which is deliberately OloEngine-adapted; the plural
    full UE port can't fwd-declare OloEngine's `TSet`/`TMap` aliases).
  - **Allocator island deleted** (`Memory/ConcurrentLinearAllocator.h` 1284 +
    `Memory/TypeTraits.h` 1364 + `Task/LocalWorkQueue.h` 285). *Correction to the
    original plan:* the `Memory/` allocator turned out to be **broken / never
    compiled** (its only includer was the dead `LocalWorkQueue.h`; wiring it to
    `TaskDelegate` surfaced multiple compile errors incl. a `TAllocatorTraits`
    redefinition clashing with `ContainerAllocationPolicies.h`). So the **working
    `Experimental/ConcurrentLinearAllocator.h` is kept** (used by `ParallelFor` /
    `TaskDelegate`) and the broken `Memory/` island removed. `Memory/TypeTraits.h`
    was likewise a *partial* dup of the live `Templates/UnrealTypeTraits.h`
    (24 users) — kept the canonical one.
  - **`FWordMutex` consolidated:** `HAL/ParkingLot.cpp` dropped its private
    duplicate of `FWordMutex` and now uses `Threading/WordMutex.h`'s
    `OloEngine::FWordMutex` (algorithm-identical), making `WordMutex.h` live.
- **2026-06-03 — pre-existing bug fixed (cinematic test flakiness, surfaced while
  running the suite):** `CinematicAssetPlaybackTest` (and the `#258`-class
  `AssetSceneLoadTest`) flaked because `EditorAssetManager`'s ctor/dtor
  `Init()`/`Shutdown()` the process-global `AssetImporter` serializer registry and
  `PlaceholderAssetManager` behind **boolean guards**. When a new manager is
  constructed before the old one is destroyed (`Project::SetAssetManager` swap,
  or back-to-back tests), the old manager's destructor tore those shared
  registries down under the live new manager → *"No serializer available for
  CinematicSequence"* → `GetAsset` returned null. **Fix:** reference-count both
  registries so they survive overlapping manager lifetimes (static-destruction
  safety preserved); plus `FunctionalTest::EnableAssetManager` now passes
  `Initialize(false)` (no file watcher in tests, per that method's own guidance).
  Verified: cinematic test 10/10 repeats, full suite 2766/2766.
- **2026-06-03 — pass 3 (per-item review of the remaining 🔵 tier):**
  - **Removed:** `Async/QueuedThreadPool.{cpp,h}` + `Async/QueuedWork.h` (the unused
    UE thread-pool layer — the engine uses `LowLevelTasks::FScheduler` directly; `QueuedWork.h`
    was included only by `QueuedThreadPool.h`). The 3 Threading mutex/lock variants
    (`Threading/RecursiveWordMutex.h`, `TransactionallySafeMutex.h`,
    `IntrusiveUniqueLock.h`) — superseded by the **already-merged** threading-unification
    onto `FSharedMutex`/`FMutex`/`FRecursiveMutex` (that branch is an ancestor of master,
    so no collision). The dead `#if 0` stat-tracking block in the otherwise-live
    `Memory/LockFreeList.cpp`. (`Async/Async.h` + `Future.h` **kept** — `Async.h` is used by
    `TaskSystemTest`.)
  - **Kept (deliberately, per review):**
    - `Containers/LinkedList.h` (`TLinkedList`/`TDoubleLinkedList`) + `Containers/Queue.h`
      (`TQueue`) — useful general-purpose containers. *Use-site analysis:* no clean current
      home — the only `std::list` uses are the SoundGraph LRU caches (off-limits, and
      `std::list` is already a fine fit there); no `std::forward_list`. So these are
      forward-looking, not droppable-in today.
    - `Memory/MemStackUtility.h` — fixed the stray-`n` typo. *Use-site analysis:* value is
      gated on broader `FMemStack` adoption (only `ParallelFor` uses `FMemStack` today);
      they'd pay off once scratch-stack allocation is adopted in render-command building,
      serialization scratch buffers, or per-frame format temporaries.
    - `Memory/PlatformMemory.h` — kept; added a `TODO(platform-memory)` to port the per-OS
      `FPlatformMemory` backends (large pages / accurate stats / NUMA) when needed.
    - `HAL/ThreadManager.h` (`FThreadManager`) — kept to back a future thread inspector;
      filed **issue #282**.
    - `Algo/HeapSort|Heapify|IsHeap.h` — kept as public API over the live `BinaryHeap.h`.
      (No `[[maybe_unused]]` added: uninstantiated function templates don't trigger
      unused warnings, so it would be a no-op.)
- **2026-06-04 — finish-wire (branch `feature/quest-inventory-event-wiring`):** the
  🟡 Quest/Inventory event payloads are now *wired*, not deleted. Added a per-Scene
  `GameplayEventBus` (`Gameplay/GameplayEventBus.h`) — a synchronous, type-keyed
  pub/sub, the "notification mechanism" `QuestSystem.h` previously lacked. The pure
  `QuestJournal` value type now reports its changes (including the internal objective
  → stage → auto-complete cascade) through an optional `QuestEventSink` out-param
  (no new members → `operator==`/serialization/undo untouched). Entity-aware service
  methods on `QuestSystem` / `InventorySystem` translate those into the
  entity-stamped `QuestEvents.h` / `InventoryEvents.h` payloads and publish them; the
  real per-frame paths emit too (auto-pickup → `ItemAdded`, timed-quest deadline →
  `QuestFailed`). C# + Lua scripting mutators route through the service layer (Lua
  recovers the owning entity via `Scene::GetEntityForComponent`). Covered by
  `QuestEventsEmittedTest`, `InventoryEventsEmittedTest`, and the `GameplayEventBusTest`
  unit test. **All 11 payloads now have a producer; none remain dead scaffolding.**
