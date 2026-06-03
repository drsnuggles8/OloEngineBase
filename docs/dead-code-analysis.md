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
| `Gameplay/Quest/QuestEvents.h` | 61 | POD notification payloads (`QuestStartedEvent`, `ObjectiveProgressEvent`, …) for the **fully-implemented Quest system** (`QuestSystem`, `QuestJournal`, `QuestDatabase`, `QuestComponents`, `QuestDefinition`). `QuestSystem.h` has no notification mechanism yet → **finish the wiring**, don't delete. |
| `Gameplay/Inventory/InventoryEvents.h` | 43 | Event payloads (`ItemAddedEvent`, `ItemEquippedEvent`, …) for the **real Inventory system** (`InventorySystem`, `Inventory`, `InventoryComponents`). |
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

| Header | LOC | Live twin in use |
|---|---|---|
| `Memory/ConcurrentLinearAllocator.h` | 1284 | Duplicate of `Experimental/ConcurrentLinearAllocator.h` (used by `Task/ParallelFor.h`, `TaskDelegate.h`). Same namespace + symbols → latent ODR hazard. |
| `Memory/TypeTraits.h` | 1364 | Same provenance as the live `Templates/UnrealTypeTraits.h` (included by ~40 files + the type-trait tests). |
| `Threading/WordMutex.h` | 265 | `FWordMutex` re-implemented privately in `HAL/ParkingLot.cpp`; the engine-wide lock is `Threading/SharedMutex.h` (`FSharedMutex`). |
| `Containers/Queue.h` | 341 | `Containers/SpscQueue.h` / `MpscQueue.h` / `Deque.h`. |
| `Containers/ContainersFwd.h` | 254 | The singular `Containers/ContainerFwd.h` (included by `Array.h`). |
| `Memory/PlatformMemory.h` | 17 | `FPlatformMemory` already aliased in `GenericPlatformMemory.h` (self-described redundant). |

### Speculative (ported, zero use, no live consumer, no documented plan)

| Header / TU | LOC | Live alternative |
|---|---|---|
| `Threading/RecursiveWordMutex.h` | 96 | `Threading/RecursiveMutex.h` (its own comment says "Prefer FRecursiveMutex"). |
| `Threading/TransactionallySafeMutex.h` | 38 | Just aliases `FMutex`/`FRecursiveMutex`/`FSharedMutex`. |
| `Threading/IntrusiveUniqueLock.h` | 37 | `UniqueLock.h` / `SharedLock.h`. |
| `Containers/LinkedList.h` | 783 | `Containers/IntrusiveLinkedList.h` (used by `Task/Scheduler.h`). |
| `Algo/HeapSort.h` / `Heapify.h` / `IsHeap.h` | 245 | `Algo/Sort.h`→`IntroSort.h`→`BinaryHeap.h`. **Keep `BinaryHeap.h` — it is live.** |
| `HAL/ThreadManager.h` | 363 | `Tasks::FNamedThreadManager` (`Task/NamedThreads.h`). |
| `Task/LocalWorkQueue.h` | 285 | (none) — sole includer of the dead `Memory/ConcurrentLinearAllocator.h`. |
| `Memory/MemStackUtility.h` | 147 | `Memory/MemStack.h` (`FMemStack`/`FMemMark`, used by `ParallelFor.h`). |
| `Async/QueuedThreadPool.{cpp,h}` | 620 | Abandoned UE-port thread pool; `AsyncPool()` never called. `Async/Async.h` is pulled into one test only. |
| `Memory/LockFreeList.cpp` `#if 0` block | ~9 | Disabled reference code in an otherwise-live file (in-file edit). |

**Dead island note:** `Task/LocalWorkQueue.h` → `Memory/ConcurrentLinearAllocator.h`
→ `Memory/TypeTraits.h` form one self-contained dead cluster (~2,933 LOC).
Removing all three together is clean and also kills the ODR hazard with the live
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

### Rough removable LOC by decision band
- 🟢 Native cruft (this effort): ~1,011 LOC + 10 empty files.
- 🔵 UE-ported redundant: ~3,525 LOC.
- 🔵 UE-ported speculative: ~2,600 LOC.
- 🟡 Keep / finish-wire: ~432 LOC (do not delete).

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
- **Pending decision:** the entire 🔵 UE-ported tier, and whether to
  *finish-wire* the 🟡 Quest/Inventory event payloads vs leave as-is.
