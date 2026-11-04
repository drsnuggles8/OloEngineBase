# Per-thread caches at thread exit: incidents and measurements

Companion to [per-thread-caches-hand-back-at-thread-exit.md](per-thread-caches-hand-back-at-thread-exit.md),
which holds the rules.

## PR 1679: worker restarts grew past 6 GB

PR #1679's Windows tests 2/3 shard stopped at the memory ceiling in
`WorkerRestartTest.RestartWorkersAndExternalThreads`. The resident set was 138 MB when the test
started and 6165 MB 57 seconds later. The test restarts the scheduler's workers 1000 times while
another thread launches and waits on tasks. On an unloaded 16-core box it takes 2 seconds and
its memory stays flat. CI runs it beside five other ctest processes on four vCPUs, where it took
57 and 71 seconds in two runs.

Pinned to two cores with `OLO_TASK_GRAPH_NUM_WORKERS=2`, it kept 140 to 196 MB per repetition
and returned none of it. Pinned to one core, it kept 2.3 GB in one repetition. Thread and handle
counts stayed flat while private bytes climbed, so the threads were exiting and memory they had
touched was not coming back. Restarts alone and launches alone each stayed flat after warm-up.
Only the two together grew.

Three pools each gave a thread a private cache and let the thread exit with it:

| Pool | Per-thread cache | What an exited thread stranded |
|---|---|---|
| `TLockFreeFixedSizeAllocator_TLSCache` (task memory) | up to two 64 KB bundles | the bundles, replaced by fresh `Malloc` |
| `FHazardPointerCollection` (FAAArrayQueue nodes) | retired objects, collected at most once a second | the 8 KB nodes retired since the last collection, plus the list itself |
| `LockFreeLinkAllocator_TLSCache` (lock-free list links) | up to two 64-link bundles | links from a fixed index space; `LockFreeLinksExhausted` stops the process when it runs out |

With the allocator fixed, the two-core repro still kept about 78 MB per repetition. With the
hazard pointers fixed as well, it kept 59 MB on the first repetition (warm-up), then 0, 4, 0
and 0 MB.

The negative controls behave as predicted. Without the allocator hand-back, a fresh thread
carves exactly 2 fresh bundles where the exited threads had left 2 bundles' worth. Without the
link hand-back, 50 exiting threads carve 6528 links, two bundles each. Without the hazard
hand-back, the exited thread's second retired object is never destroyed.

## The cache's own destructor is the wrong place

The first allocator fix gave the `thread_local` cache a destructor that handed its bundles
back. That makes the cache an object with a lifetime. A block freed later in the same thread's
teardown, by another `thread_local` destructor, would then be written into a destroyed object.
That is the hazard [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md)
describes. The cache stayed trivially destructible, and a separate hook object, registered once
per thread, does the hand-back.

## A move constructor that released the slot it took over

`THazardPointer`'s move constructor took over the other pointer's hazard slot and then released
it. A hazard pointer acquired before the moved-to one's first `Get` received the same slot. The
later `Get` then overwrote the protection, and the moved-to pointer's object could be deleted
while in use. `HazardPointer.AMovedHazardPointerStillProtectsItsObject` acquires a second
pointer in that window. It fails on the old constructor and passes on the fixed one.
