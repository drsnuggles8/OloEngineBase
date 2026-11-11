# Hand a per-thread cache back to its pool when the thread exits

Applies to: `OloEngine/src/OloEngine/Memory/LockFreeFixedSizeAllocator.h`,
`Memory/LockFreeList.cpp`, `Containers/HazardPointer.h`, and any pool that gives
threads a private cache.

## Rules

1. **A pool that outlives its threads takes back each thread's cache when the thread exits.**
   UE leaves the cache with the thread because UE's threads live as long as the process. Here
   the scheduler restarts its workers (`FScheduler::RestartWorkers`), so every exited worker
   stranded what it held, and the pool replaced it with fresh memory or index space.
   [Details](per-thread-caches-hand-back-at-thread-exit-details.md#pr-1679-worker-restarts-grew-past-6-gb).
2. **Run the hand-back from a separate `thread_local` hook object, never from the cache's own
   destructor.** Keep the cache trivially destructible, so code that runs later in thread
   teardown and frees into it writes valid storage. Signal "the hook has run" in a trivially
   destructible `thread_local bool`, as
   [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md) requires.
3. **The pool still works after its hook has run, and starts no new cache then.**
   `thread_local` destructors run in reverse order of construction, so one constructed before
   the hook uses the pool after the hand-back. A cache started then has nothing left to hand it
   back. Either hand back whatever each later call leaves in the cache
   (`TLockFreeFixedSizeAllocator_TLSCache`), or serve the call from the shared orphans under
   their lock (`LockFreeLinkAllocator_TLSCache`, `FHazardPointerCollection`).
   [Details](per-thread-caches-hand-back-at-thread-exit-details.md#pr-1679-a-hook-that-ran-after-the-pool-it-needed).
4. **A hook that uses another pool registers that pool's hook first.** The allocator's hook
   pushes to a lock-free list, so it calls `FLockFreeLinkPolicy::RegisterThreadExitHook()`
   before constructing itself, and the link cache outlives it. Rule 3 keeps a missed call
   correct, only slower.
5. **Hand back to something that cannot be destroyed first.** A worker can exit during static
   destruction, because `~FScheduler` joins its workers. Return blocks to a never-destroyed
   orphanage or singleton. When the pool itself can die, as an `FHazardPointerCollection` can,
   check a refcounted lifetime token that its destructor clears.
6. **A partial bundle is not a bundle.** A global free list holds only whole bundles. Collect an
   exited thread's partial blocks under a lock until they make one, then push it.
7. **A thread forgets a pool that died before it.** A thread that records each pool it must
   hand back to drops the records of destroyed pools when it adds a new one
   (`FHazardPointerCollection`). A long-lived thread otherwise keeps one record per pool it
   ever used. It only tries each pool's lifetime lock there: the call can come from a deleter
   that a pool runs under its own lock, which an exiting thread holding the lifetime lock is
   waiting for.
8. **Test it on threads that exit.** Free on a thread, let it exit, then allocate on a fresh
   thread and assert the pool did not grow. For rule 3, declare a `thread_local` before the
   thread first uses the pool and use the pool from its destructor. The existing tests are
   `LockFreeFixedSizeAllocatorThreadExit`, `LockFreeLinkThreadExit` and `HazardPointer`.
   Reproduce a starved-CI growth with `OLO_TASK_GRAPH_NUM_WORKERS=2`, affinity `0x3` and
   `--olo-rss-report` ([testing-architecture.md](testing-architecture.md) rule 19).
9. **Run a thread-exit change under Linux ASan before pushing.** The Windows build cannot show
   a destruction-order fault: clang-cl constructs a namespace-scope `thread_local` when the
   thread starts, Linux on first use, and only Linux has LeakSanitizer. A WSL harness of the
   three test files and six engine sources builds in seconds.
   [Details](per-thread-caches-hand-back-at-thread-exit-details.md#the-wsl-harness).
10. **A test's own pool instance is never destroyed.** Destroying a
    `TLockFreeFixedSizeAllocator_TLSCache` empties its free list and leaves the listed bundles
    unreachable, which LeakSanitizer reports. Allocate the instance with `new` and keep it.
11. **A thread that launches and waits in a loop holds itself back by what the scheduler has
    released, not by what has run.** `Wait()` runs the task on the waiting thread, but the task's
    queue entry keeps the task object alive until a worker dequeues it. When workers are stopped
    or starved, the loop makes such entries faster than they are discarded. Count releases with
    the task's result, which is destroyed with the task object, and sleep while too many are
    outstanding. [Details](per-thread-caches-hand-back-at-thread-exit-details.md#pr-1679-the-growth-the-hand-backs-did-not-explain).
