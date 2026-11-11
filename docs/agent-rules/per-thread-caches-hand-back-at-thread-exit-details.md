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

## PR 1679: a hook that ran after the pool it needed

The first push of the three hand-backs was green on Windows and red in `ASan + LSan
(Linux/Clang)`. `LockFreeFixedSizeAllocatorThreadExit.*` passed its assertion and then
LeakSanitizer reported 65560 bytes in 3 allocations: one 64 KB bundle and two 12-byte link
caches.

The allocator's hook pushes the thread's bundles to a lock-free list, and a push takes a link
from the thread's link cache. On Linux a `thread_local` is constructed on first use. The test
thread used the allocator first and a link second, so the link cache's hook was constructed
second and destroyed first. The allocator's hook then ran with the link cache already handed
back, and its push started a second link cache that nothing handed back. That is the fault the
hand-back was written to remove, one level down. Under clang-cl the link cache's hook is a
namespace-scope `thread_local`, which the MSVC ABI constructs when the thread starts, so it is
destroyed last and the fault cannot occur. A ten-line probe printed the two orders.

The 64 KB bundle was a separate fault in the test. Its allocator was a function-local static,
destroyed at exit, and its destructor emptied the free list that held the bundle.

The fix has two parts. A thread past its link hook gets no link cache: it takes and returns
single links from the orphans under their mutex. A thread past its allocator hook hands back
whatever each call leaves in its cache. The allocator's hook also registers the link hook
before itself, which keeps the common case on the lock-free path.

Negative controls, on the Linux ASan harness:

| Reverted | Result |
|---|---|
| nothing | 7 tests pass, no leak |
| branch head before the fix | CI's report exactly: 65560 bytes in 3 allocations |
| link cache started again after its hook | 50 threads carve 2944 links (limit 640); 50 link caches leaked |
| allocator does not hand back after its hook | 120 threads carve 90 blocks (limit 15); 6 bundles leaked |
| only the ordering call | 7 tests pass, no leak: the path without a cache is enough |
| destroyed collections not forgotten | the thread remembers 100 collections instead of 1 |
| the test's allocator destroyed at exit | the first test, run alone, leaks its 64 KB bundle |

## The WSL harness

Compile, with `clang++-19 -std=gnu++23 -fsanitize=address -g -O1` and the Linux defines from
any Linux `build.ninja`, the three test files, a `main` that calls `RUN_ALL_TESTS`, and
`Memory/LockFreeList.cpp`, `Memory/UnrealMemory.cpp`, `Memory/MallocAnsi.cpp`,
`Memory/GenericPlatformMemory.cpp`, `Core/Log.cpp`, `Misc/Exec.cpp` and
`Platform/Linux/LinuxPlatformMemory.cpp`. Link `gtest`, `spdlog` and `fmt` from a Linux
`vcpkg_installed`. It builds in about five seconds. Run each suite in its own process, as ctest
does: a later test in the same process can take back what an earlier one would have leaked.

## PR 1679: the growth the hand-backs did not explain

With all three hand-backs in, the same CI shard passed once and then stopped at the ceiling
again: 6159 MB, 57.81 seconds into `WorkerRestartTest.RestartWorkersAndExternalThreads`. The
stranded caches were real, but they were not what filled 6 GB on the runner.

The test's second thread loops `Launch(...).Wait()`. `Wait()` retracts the task and runs it
on that thread. The task's entry stays in the scheduler's overflow queue, and holds the
scheduler's reference to the 256-byte task object, until a worker dequeues it and finds it
already run. Nothing in the scheduler limits how many such entries exist. While the workers
restart, the launching thread still queues (and wakes no one), and a restarted worker only
gets to discard entries when the runner gives it a core.

Measured on a Release build, on four cores shared with five busy-loop processes, which is how
the four-vCPU runner shares its cores between six ctest processes:

| | launched | most finished tasks queued at once | memory kept | peak over start | time |
|---|---|---|---|---|---|
| branch before this change | 218 million | 660,127 | +202 MB | +294 MB | 106 s |
| the same, a second run | not counted | not counted | +273 MB | +431 MB | 368 s |
| launcher holds back at 4096 outstanding, yielding | not counted | 4096 | +5 MB, then -1 MB | +5 MB | 164 s, 230 s |
| launcher holds back at 4096 outstanding, sleeping 1 ms | not counted | 4096 | +5 MB, then 0 MB | +5 MB | 45 s, 44 s |

660,127 queued tasks are 169 MB of task objects, which accounts for the growth. The runner's
contention is harsher than five busy loops, and its 6 GB is 23 million of them.

The fix is in the test, because the scheduler behaves as designed and as UE's does: the
launcher counts its tasks' releases through the task result and sleeps while more than 4096
are outstanding. Sleeping rather than yielding matters as much as the limit. A yielding
launcher stays runnable and keeps taking the core the restart needs, and the run was four
times slower.

The binary that reproduces this has to be a Release one. In Debug the launcher loop is slow
enough that the workers keep up, and the same starvation kept 79 MB.
