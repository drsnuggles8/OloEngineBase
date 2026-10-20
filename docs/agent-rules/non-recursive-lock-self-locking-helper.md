# A self-locking helper called under its own non-recursive lock

Read before adding a lock to a manager class, and before calling any sibling method from inside a locked scope. `FSharedMutex` is not recursive; re-acquiring it on the same thread parks the thread forever with no assert, log line or crash record (issues #439, #863).

## Rules

1. **Before calling a sibling method from inside a locked scope, open it and check what it locks.** A public helper that locks defensively deadlocks when composed under the caller's lock; "it's just a helper" caused both #439 and #863. [Details](non-recursive-lock-self-locking-helper-details.md#1-the-shape)
2. **Fix the callee, not just the caller.** Moving one call site out of the lock (`aa37548c7`) left `SerializeAssetRegistry()` a public method that every future caller had to avoid calling under the lock. [Details](non-recursive-lock-self-locking-helper-details.md#2-why-the-first-fix-did-not-hold)
3. **Do not put a mutex around a member that already locks itself unless you can name the compound operation it makes atomic.** `AssetRegistry` is self-synchronised, so a manager lock around a single registry call is a re-entrancy trap with no upside; a read-modify-write (`GetMetadata` then `UpdateMetadata`) is the real job of `m_RegistryMutex`. [Details](non-recursive-lock-self-locking-helper-details.md#3-the-fix-that-holds-notice-the-outer-lock-was-redundant)
4. **Do not make a public method lock defensively without considering every caller.** Prefer a self-synchronised callee, or a locking public wrapper over an unlocked private core (`DoThingUnlocked()`). [Details](non-recursive-lock-self-locking-helper-details.md#7-checklist)
5. **Never nest `EditorAssetManager`'s mutexes.** It takes `m_AssetsMutex`, `m_RegistryMutex` and `m_DependenciesMutex` one at a time in sequential scopes, so there is no lock order to get wrong. If you add a nesting, say why in a comment and check the reverse order across the class. [Details](non-recursive-lock-self-locking-helper-details.md#4-also-remove-the-lock-order-not-just-the-recursion)
6. **Do not rely on recursion in `FSharedMutex`, even shared-in-shared.** An exclusive and a shared lock cannot be held at once by one thread, and a recursive shared lock deadlocks whenever a writer is queued (intermittent). `FSharedRecursiveMutex` exists, but needing it usually means the structure is wrong. [Details](non-recursive-lock-self-locking-helper-details.md#7-checklist)
7. **Diagnose a wedge by CPU, log and MCP, then dump every thread.** Deadlocked means process CPU flat at 0 ms on all threads, a silent log, and MCP "Timed out waiting for the editor main thread"; run `~*k` in cdb (not only `~0k`) and look for `ParkingLot::Wait` under `FSharedMutex::LockSlow`. [Details](non-recursive-lock-self-locking-helper-details.md#5-diagnosis-telling-a-wedge-from-a-slow-cook)
8. **Make a test reach the deadlocking line.** `EditorAssetManager::ReloadData` returns early unless the asset is both tracked and loaded. Follow `AssetHotReloadDoesNotDeadlockTest.cpp`: stage a `.cs` script file (CPU-only serializer, no GL context) and call `ReloadData` on a worker with a bounded wait so a regression fails that case instead of hanging the suite. [Details](non-recursive-lock-self-locking-helper-details.md#6-why-no-headless-test-caught-it)
9. **To verify live, get the asset genuinely loaded first.** Open `VirtualGeometryTest.olo`, set `olo_renderer_settings_set renderpath=deferred`, confirm a `Loaded asset:` trace, touch the file, expect `Hot-reload triggered` then `Reloaded asset`, then check `olo_virtual_geometry_stats` shows `unresolvedAssets: 0` and non-zero `submitted`. A filewatch event on an unloaded asset resolves to `Ignore`. [Details](non-recursive-lock-self-locking-helper-details.md#6-why-no-headless-test-caught-it)
10. **Do not treat the `FSharedMutex` self-deadlock detector as permission to stop thinking.** It reports before a blocking acquisition when the thread already holds the lock, but sees only acquisitions that execute. It lives in `OloEngine/src/OloEngine/Threading/LockDebug.h`, keeps `sizeof(FSharedMutex)` at four bytes via a `thread_local` table, and does not instrument `FMutex`. [Details](non-recursive-lock-self-locking-helper-details.md#6a-the-detector--you-should-not-need-this-document-to-find-it-twice)
11. **Gate inline header code on `NDEBUG`, not `OLO_DEBUG`, and do not use `OLO_CORE_ASSERT` in header/inline engine code.** `OLO_DEBUG` is PRIVATE to the `OloEngine` target, so other targets compile a different body (ODR violation) and the assert is a no-op there. [Details](non-recursive-lock-self-locking-helper-details.md#6a-the-detector--you-should-not-need-this-document-to-find-it-twice)
12. **Model "multiple readers" in tests with real threads.** `SharedMutexTest.MultipleReaders` used three shared locks on one thread and was the detector's only hit across 6412 tests. [Details](non-recursive-lock-self-locking-helper-details.md#6a-the-detector--you-should-not-need-this-document-to-find-it-twice)

## Commands and checks

```
~*k
```

cdb command that dumps every thread's stack in a hung process.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[non-recursive-lock-self-locking-helper-details.md](non-recursive-lock-self-locking-helper-details.md).
