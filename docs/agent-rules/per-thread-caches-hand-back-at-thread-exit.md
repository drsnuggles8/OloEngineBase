# Hand a per-thread cache back to its pool when the thread exits

Applies to: `OloEngine/src/OloEngine/Memory/LockFreeFixedSizeAllocator.h`,
`Memory/LockFreeList.cpp`, `Containers/HazardPointer.h`, and any pool that gives
threads a private cache.

## Rules

1. **A pool that outlives its threads takes back each thread's cache when the thread exits.**
   UE leaves the cache with the thread because UE's threads live as long as the process. Here
   the scheduler restarts its workers (`FScheduler::RestartWorkers`), so every exited worker
   stranded what it held, and the pool replaced it with fresh memory or index space.
   [Details](per-thread-caches-hand-back-at-thread-exit-details.mdpr-1679-worker-restarts-grew-past-6-gb).
2. **Run the hand-back from a separate `thread_local` hook object, never from the cache's own
   destructor.** Keep the cache trivially destructible, so code that runs later in thread
   teardown and frees into it writes valid storage. Its blocks are lost then, but nothing is
   corrupted. Signal "the hook has run" in a trivially destructible `thread_local bool`, as
   [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md) requires.
3. **Hand back to something that cannot be destroyed first.** A worker can exit during static
   destruction, because `~FScheduler` joins its workers. Return blocks to a never-destroyed
   orphanage or singleton. When the pool itself can die, as an `FHazardPointerCollection` can,
   check a refcounted lifetime token that its destructor clears.
4. **A partial bundle is not a bundle.** A global free list holds only whole bundles. Collect an
   exited thread's partial blocks under a lock until they make one, then push it.
5. **Test it on threads that exit.** Free on a thread, let it exit, then allocate on a fresh
   thread and assert the pool did not grow. The existing tests are
   `LockFreeFixedSizeAllocatorThreadExit`, `LockFreeLinkThreadExit` and
   `HazardPointer.AnExitedThreadsRetiredObjectsAreReclaimed`. Reproduce a starved-CI growth
   with `OLO_TASK_GRAPH_NUM_WORKERS=2`, affinity `0x3` and `--olo-rss-report`
   ([testing-architecture.md](testing-architecture.md) rule 19).
