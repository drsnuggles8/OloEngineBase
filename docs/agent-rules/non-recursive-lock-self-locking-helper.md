# A self-locking helper called under its own non-recursive lock

**The failure:** `EditorAssetManager::ReloadData` held `m_RegistryMutex` and called
`SerializeAssetRegistry()`, which took the same lock. `FSharedMutex` is not recursive, so the
calling thread parked in `ParkingLot::Wait` and never came back. The editor's game thread was
gone: no assert, no log line, no CPU, no crash record — just a window that stopped answering.

It was found and fixed once (issue **#439**, `aa37548c7`), and independently rediscovered the same
day through a completely different trigger (issue **#863**), because the first fix moved *one call
site* out of the lock and left the loaded gun on the table.

Read this before adding a lock to a manager class, and before calling any sibling method from
inside a locked scope.

---

## 1. The shape

```cpp
void Manager::DoWork()
{
    TUniqueLock<FSharedMutex> lock(m_Mutex);   // (1) acquire
    m_Thing.Update(...);
    Persist();                                 // (3) …which acquires again → park
}

bool Manager::Persist()
{
    TUniqueLock<FSharedMutex> lock(m_Mutex);   // (2) a helper that locks internally
    return m_Thing.WriteToDisk(path);
}
```

Both halves read as correct in isolation. `Persist()` locking is *defensive* — it is a public
method, other callers do not hold the lock, and locking there is the obviously-safe choice. The
bug only exists in the composition, and nothing in the type system, the compiler, the linter or
the test suite looks at the composition.

`FSharedMutex` says so in its own header — *"is not fair and does not support recursive locking"*
— and the failure is 100% deterministic once the two frames meet. This is not a race. There is
nothing timing-dependent about it. The *trigger* can be rare; the deadlock, once triggered, is
certain.

## 2. Why the first fix did not hold

`aa37548c7` fixed #439 by scoping the lock so the `Persist()` call fell outside it:

```cpp
{
    TUniqueLock<FSharedMutex> lock(m_RegistryMutex);
    m_AssetRegistry.UpdateMetadata(assetHandle, metadata);
}
SerializeAssetRegistry();   // outside — correct
```

That is the right change for that call site and it is still in the code. But it fixes a *caller*.
The dangerous property lives in the *callee*: `SerializeAssetRegistry()` was a public method that
took a non-recursive lock, so every present and future caller had to know not to be holding it —
a rule enforced by nothing but a comment. Issue #863 arrived through filewatch hot-reload during a
cold-cache scene load rather than through a lightmap re-bake, on a build made hours before
`aa37548c7` landed, and produced the identical stack at the identical line.

**Fixing the caller fixes one path. Fixing the callee fixes the archetype.**

## 3. The fix that holds: notice the outer lock was redundant

`SerializeAssetRegistry()` did exactly one thing under `m_RegistryMutex`:

```cpp
return m_AssetRegistry.Serialize(registryPath);
```

and `AssetRegistry::Serialize` **already** takes the registry's own `FSharedMutex` in shared mode
around the whole write. The outer lock protected nothing the inner lock did not, and bought
nothing but a way to deadlock. Deleting it makes the re-entrant call harmless from every call
site, forever, with no discipline to remember and no assert to fire.

This generalises. `EditorAssetManager` wraps `AssetRegistry`, and `AssetRegistry` is
**self-synchronised** — every public method locks. So a manager-level lock around a *single*
registry call is always redundant. It is not redundant around a **compound** sequence:

```cpp
TUniqueLock<FSharedMutex> lock(m_RegistryMutex);
auto metadata = m_AssetRegistry.GetMetadata(handle);   // two individually-atomic
metadata.Status = status;                              // calls that must be
m_AssetRegistry.UpdateMetadata(handle, metadata);      // atomic TOGETHER
```

That read-modify-write is the actual job of `m_RegistryMutex`, and `SetAssetStatus`, the status
updates in `SyncWithAssetThread` and `ReloadData`'s `LastWriteTime` refresh all need it.

**The rule:** when you put a mutex around a member that already locks itself, ask what compound
operation you are making atomic. If the answer is "none, it's one call", the lock is not
protection — it is a re-entrancy trap with no upside.

## 4. Also remove the lock *order*, not just the recursion

The same change dropped the one place in `EditorAssetManager` that nested two different mutexes:
`SyncWithAssetThread` held `m_AssetsMutex` exclusively across its integration loop and took
`m_RegistryMutex` inside it, once per ready asset. There was no `m_RegistryMutex → m_AssetsMutex`
path anywhere, so it was not an ABBA *yet* — it was one future edit away from being one.

Splitting it into two sequential scopes (which the raw-asset branch immediately above already
did) makes a stronger statement than "the current order is consistent":

> `EditorAssetManager` acquires `m_AssetsMutex`, `m_RegistryMutex` and `m_DependenciesMutex` one
> at a time and never nests them. There is no lock order, so there is no lock order to get wrong.

An invariant a reviewer can check by grepping for a nested `TUniqueLock` beats an ordering
convention nobody has written down. If you add a nesting, you are re-introducing the audit.

## 5. Diagnosis: telling a wedge from a slow cook

Both #439 and #863 were first mistaken for "the mesh cook is taking a while". The signature that
separates them:

| | slow | deadlocked |
|---|---|---|
| process CPU | rising | **flat at 0 ms across every thread** |
| the log | still producing lines | silent, mid-operation |
| MCP calls | answer, slowly | *"Timed out waiting for the editor main thread"* |

Zero CPU with over a hundred live threads is the tell. Once you have it, crack the live process
with the store-package `cdb` (recipe in the `windbg-cdb-location-this-box` memory note) and dump
**every** thread, not just the main one:

```
~*k
```

A stack of `NtWaitForAlertByThreadId` → `ParkingLot::Wait` → `FSharedMutex::LockSlow` →
`FSharedMutex::Lock` names the *victim*. The victim's own stack is enough only when the other
acquisition is on the same thread — which is exactly the case here, and is why `~0k` alone
happened to be sufficient twice. Do not count on that: `~*k` costs nothing and the same-thread
answer is visible in it too (the culprit frame is further down the *same* stack).

## 6. Why no headless test caught it

Both reports were live-editor findings, and #863's own framing was that the bug is *"invisible to
every headless test"*. It was — but not because the path is untestable. Nothing headless had ever
called `EditorAssetManager::ReloadData` at all, because reaching its persist tail needs an asset
that is both **tracked** (in the registry) and **loaded** (in the cache); miss either and
`ReloadData` returns early, before the deadlocking line, and a test written without noticing that
asserts nothing while looking green.

`OloEngine/tests/Functional/Asset/AssetHotReloadDoesNotDeadlockTest.cpp` is that coverage. Two
things about how it is written are worth copying:

- It stages a **`.cs` script file**, because `ScriptFileSerializer` is CPU-only (text →
  `ScriptFileAsset`, no GPU resources). The path runs with no GL context, so the test is a normal
  CI citizen rather than one more `SKIP` that only ever runs on a workstation.
- It calls `ReloadData` on a worker with a bounded wait and fails on timeout, rather than calling
  it inline. That is not a timing assertion in the sense
  [timed-wait-test-assertions.md](timed-wait-test-assertions.md) warns about: the regression is a
  **permanent** park, so any generous upper bound separates pass from fail with no sensitivity at
  either end. The worker exists only so that a regression fails *this case* instead of hanging the
  whole suite.

**To exercise the same path in a live editor** (worth doing once for any change to this seam), you
must first get the asset genuinely *loaded* — a filewatch event on a tracked-but-unloaded asset
resolves to `Ignore`, so touching a file proves nothing by itself:

1. Open `VirtualGeometryTest.olo` and set `olo_renderer_settings_set renderpath=deferred`. The
   `VirtualMeshComponent` submission loop runs only on Deferred, and it is the only thing that
   resolves those mesh-source handles — on Forward nothing loads and every event is ignored. See
   [notes-editor-and-assets.md](notes-editor-and-assets.md) §2.
2. Confirm a `Loaded asset: <path>` trace appeared for the file you are about to touch.
3. Touch it. You should see `🔄 Hot-reload triggered` followed by `Reloaded asset`, about a second
   apart, with the editor still answering MCP in single-digit milliseconds.
4. Re-check `olo_virtual_geometry_stats` afterwards: `unresolvedAssets: 0` and a non-zero
   `submitted` prove the reload republished the asset rather than dropping it. Liveness alone does
   not.

## 6a. The detector — you should not need this document to find it twice

Everything above is discipline, and discipline is what failed here: the same line was
rediscovered the same day, by a second person, through a different trigger. So the check is now in
the code. `FSharedMutex` asks, before every blocking acquisition, whether the calling thread
already holds that lock, and says so:

```
Self-deadlock on a non-recursive lock at 0x2a6d683e874: this thread already holds it (exclusive),
and is now asking for it exclusive. The acquisition below this line will park the thread FOREVER
— no further log lines, no CPU, no crash record. …
```

Measured against the #863 bug deliberately reintroduced: the regression test used to fail after
**30,023 ms** with a timeout that named nothing, and now fails in **0.3 s** with the lock, both
modes and the cause printed. That is the difference between "something is wrong somewhere" and a
diagnosis.

Four things about it worth knowing before you touch it
(`OloEngine/src/OloEngine/Threading/LockDebug.h`):

- **It costs the mutex nothing.** The held-lock table is `thread_local`, keyed by mutex address.
  `sizeof(FSharedMutex)` is still four bytes in every configuration — no layout to go stale in an
  incremental build (cf. [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md)).
- **It is gated on `NDEBUG`, not `OLO_DEBUG`, and reports out of line rather than through
  `OLO_CORE_ASSERT`.** `OLO_DEBUG` is **PRIVATE** to the `OloEngine` target, so it is absent when
  these headers are compiled into `OloEditor` or `OloEngine-Tests` — those TUs receive neither
  `OLO_DEBUG` nor `NDEBUG`. Gating an *inline* function's body on a per-target macro is an ODR
  violation whose arbitrary winner might be the copy with the detector compiled out. This is the
  same hazard `CLAUDE.md` describes for `ENTT_USE_ATOMIC`, and it is worth remembering generally:
  **`OLO_CORE_ASSERT` inside header/inline engine code is a no-op wherever that header lands
  outside the engine library.**
- **All four re-acquisition combinations are flagged**, including shared→shared: waiting writers
  get priority over new readers, so a recursive shared lock deadlocks *whenever a writer happens
  to be queued* — intermittently, which is worse to debug than always.
- **It found something the first time it ran.** `SharedMutexTest.MultipleReaders` modelled
  "multiple readers" as three shared locks on one thread. It was the only hit across 6412 tests,
  and it now uses three real threads.

`FMutex` is deliberately *not* instrumented — it sits in the task scheduler's hot path and inside
the locking primitives themselves, where both the cost and the re-entrancy question are different.

## 7. Checklist

- Calling a sibling method from inside a locked scope? Open it and check what it locks. "It's just
  a helper" is how both of these happened.
- Adding a lock around a member that locks itself? Name the compound operation it makes atomic. If
  there isn't one, don't add it.
- Making a public method lock defensively? That is a constraint on every caller forever. Prefer a
  self-synchronised callee, or split into a locking public wrapper and an unlocked private core
  (`DoThingUnlocked()`), so the internal caller has something safe to call.
- Nesting two mutexes? Say why in a comment, and check for the reverse order across the whole
  class — or restructure into sequential scopes and buy the stronger invariant instead.
- `FSharedMutex` is **not** recursive, and an exclusive lock and a shared lock may not be held
  simultaneously by the same thread. If you genuinely need recursion, `FSharedRecursiveMutex`
  exists — but wanting it is usually a sign the structure is wrong, not the mutex. §6a will now
  tell you at runtime if you get this wrong; do not treat that as permission to stop thinking
  about it, because the detector only sees the acquisitions that actually execute.

## Related

- [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) — the other
  hand-ported UE concurrency primitive whose failure mode is silent.
- [notes-core-and-threading.md](notes-core-and-threading.md) — the task system, `Ref<T>` constness,
  lock-free testing.
- [notes-editor-and-assets.md](notes-editor-and-assets.md) — filewatch import, Content Browser
  refresh, asset placeholders.
- [timed-wait-test-assertions.md](timed-wait-test-assertions.md) — when a timed assertion *is* a
  flake trap, and why a liveness bound on a permanent hang is not one.
