# Task system and sync primitives: what the review found and how each was reproduced

Companion to [task-system-and-sync-primitive-invariants.md](task-system-and-sync-primitive-invariants.md),
which holds the rules. A read-only review of the threading code in November 2026 reported these;
each was then confirmed against the code and reproduced before it was changed. "Linux" below means
a standalone ASan or TSan harness under WSL, described at the end.

## A task launched during a restart was never woken for

`RestartWorkers` sets `m_TemporaryShutdown`, stops and starts the workers, and clears the flag.
`LaunchInternal` on a thread with no local queue queues the task and returns before waking a worker
while the flag is set. A task queued after the new workers' first scan waited for an unrelated
launch. `WorkerRestartTest.ATaskLaunchedDuringARestartRunsWithoutAnotherLaunch` failed on the first
or second restart in five of five runs; with the two `Notify()` calls it passes. The older
`RestartWorkersAndExternalThreads` could not see it, because `Wait()` retracts the task and runs it
inline. `RestartWorkers` has no engine caller.

## IsWorkerThread read destroyed thread state

`IsWorkerThread()` went through `GetTlsValuesRef()`, which dereferences the thread's holder.
`ExecuteTask` calls it for a background task that `~FScheduler` drains after the main thread's
holder is gone. On Linux, a `thread_local` constructed before the scheduler's values asks from its
destructor: SEGV at address 0x20 before, `false` after
(`SchedulerThreadExitTest.IsWorkerThreadAnswersAfterTheThreadsSchedulerValuesAreGone`). Under the
MSVC ABI the holder is constructed at thread start, so the test is the plain case there.

## The per-thread registry linked a freed entry

`ProcessPendingTlsValuesNoLock` consumed the pending inserts, then the pending deletes. A thread
whose constructor and destructor both lost the `TryLock` between the two calls was unlinked (a
no-op), freed, and left on the insert list. A plain stress of 36,000 short-lived threads did not hit
it. With 300 microseconds between the two calls it is a heap-use-after-free in `LinkHead`; with the
deletes taken first and the same sleeps, clean.

## A rejected queue registration was visible to stealers

`AddLocalQueue` did `fetch_add`, compared, and `fetch_sub` on overflow. A stealing thread that read
the count in between walked one slot past `m_LocalQueues` and used the next member as a queue.
`LocalQueueTest.ARejectedRegistrationIsNeverVisibleToAStealingThread` segfaulted after 37
rejected registrations. It needs a full registry (1024 queues), which no normal configuration has.

## The concurrency limiter stranded a task

`ProcessQueue` popped null, then released its slot. A `Push` in between queued its task, found the
slot taken and returned; nothing launched the task, and an infinite `Wait()` spun. With 200
microseconds between the pop and the release, a task was still queued two seconds later in the
first round; with the second look at the queue and the same sleeps, 300 rounds pass. Only tests
use the limiter. No test is committed for it: without the widening it does not fail.

## A piped task recorded its predecessor too late

`TryUnlock` made the task a subsequent of the pipe's previous task and only then pushed that task
into its prerequisites. The previous task closing in between unlocked, ran and closed the new one,
and the late push was never released. With 300 microseconds in the gap, 289 of 300 previous tasks
leaked under LeakSanitizer; with the prerequisite recorded first, none. Only tests use `FPipe`.

## Queued game-thread tasks outlived the asset manager

The file watcher and `ReloadDataAsync` queued `[this]` lambdas, and `Shutdown()` left them queued.
Open Project replaces the manager, so the next frame ran a task on a destroyed one; the Open
Project dialog blocks the game thread while the watcher keeps queueing. `Shutdown()` also spun
until `m_ActiveReloadTasks` reached zero, which only the game thread could bring about, so one
queued reload hung it. The gate turns a shut-down manager's tasks into no-ops, under a mutex that
also waits out a task running on another thread. `AssetManagerShutdown` has three tests; with the
gate left open, the shut-down manager is still asked to reload.

## A release posted at teardown never ran

A cancelled asset-pack build posts its temporary asset manager to the game thread while
`~EditorLayer` waits for it. `~Application` never drains the queue again, so the manager, whose
assets own GPU objects, was released at static destruction, after the renderer.
`RunPostedGameThreadWork()` runs posted work and nothing else
(`GameThreadCallTest.PostedWorkLeftQueuedRunsOnceWithoutDrainingTheQueue`).

## Linux timed waits took twice as long and spun

`WaitForSlow` waited its span, then called `WaitUntilSlow(Now() + span)` whenever the state was
unchanged, which is also what a timeout looks like. `WaitUntilSlow` passed
`FPlatformTime::Seconds()`, which counts from the first call, as a wall-clock deadline, so the
futex returned at once and the loop spun until the deadline. A 500 ms timed-out wait took 1000 ms
with 500 ms of thread CPU (`ManualResetEventTest.ATimedOutWaitSleepsInsteadOfSpinning`). Reached by
the network thread's idle tick, the audio loop and every 1 ms task-wait poll, on Linux only.

## A parked worker parked again

`ParkingLot::Wait` queues the thread's one wait node, then calls `Event.Wait()`, whose slow path
constructs `FOversubscriptionScope`. At the oversubscription limit that broadcasts the limit event
under a lock, which was an `FMutex`; contended, it parked through the ParkingLot on the node that
was still queued. `WorkerRestartTest.ReachingTheOversubscriptionLimitWhileParkedDoesNotParkAgain`
holds the lock in a callback while a second worker reaches the limit. With `FMutex` put back, the
new assertion fires; with the OS lock it passes. The first version of that test blocked its
workers on `FManualResetEvent`, which waits on its own futex and queues no node, and passed against
the old code.

## A stale notify ended the next wait

A waker clears `WaitAddress`, then notifies. A timeout between the two took the "dequeued" shortcut
and returned with the notify pending; it arrived during the thread's next wait, after that wait's
`Reset()`, and an untimed `Wait` returned with its node still queued. With 300 microseconds
between the two steps and the wake timing swept across the timeout, the next enqueue was a
heap-use-after-free; with the shortcut taking the notify, clean.

## A Linux event wait with no timeout returned false

`FEventLinux::Wait` woke on `m_Triggered` or a generation change but returned `m_Triggered`. An
auto-reset waiter whose trigger a second thread took returned false from a wait with no timeout
(`Event.AnAutoResetWaitWithoutATimeoutNeverFails`, round 0). Engine users are all manual-reset.

## The overflow queue retired a node the tail still named

`DequeueInternal` moved the head off a drained node and retired it while `m_Tail` could still point
at it. An enqueuer validating its hazard against `m_Tail` then protected a node whose reclamation
scan had already passed that enqueuer's slot. Inherited from the published algorithm and from UE.
Not reproduced: it needs three threads at a node boundary. The fix is the tail advance that a
Michael-Scott queue does at the same place.

## A WeakRef resolved to a different object

The live registry was a set of addresses. A `WeakRef` that outlived its object was valid again as
soon as another object was allocated at that address, and `Lock()` returned it: `Font::Create`'s
cache keeps a `WeakRef` per path and never prunes it. Separately, `Ref` inserted the `T*` address
and erased the `RefCounted*` address, which differ when `RefCounted` is not the first base, so
every `SoundGraph` stayed registered after it died. Both `RefTest` cases use a class whose
`operator new` returns static storage, so the address reuse is certain; both fail before and pass
after. A `WeakRef` built from a raw pointer names whatever is live there at that moment, so the
registries that keep raw pointers still depend on unregistering in their destructors.

## The console panel edited a sink list other threads were reading

`ConsolePanel` pushed its sink onto, and erased it from, each logger's `sinks()` vector, which
spdlog reads without a lock on every log call. `Log::AttachSink` and `DetachSink` go through one
`dist_sink_mt` shared by the three loggers. `LogAttachedSink` attaches and detaches 200 times
while four threads log, clean under ThreadSanitizer.

## How these were reproduced

The Linux harness compiles one test file and about thirty engine sources (`Task/`, `HAL/`,
`Threading/`, `Memory/`, a few of `Core/`) with `clang++-19 -fsanitize=address` or `thread`, and
builds in under a minute; `OloEngine/tests/Tasks/TaskSystemTest.cpp` builds in it unchanged. A
control copies the tree, edits one file and rebuilds. Run each test in its own process: two tests
sharing one reused a stack slot and produced a ThreadSanitizer report that belonged to neither.

The starved-runner measurements used a Release test binary on four cores shared with five
busy-loop processes; see
[per-thread-caches-hand-back-at-thread-exit-details.md](per-thread-caches-hand-back-at-thread-exit-details.md#pr-1679-the-growth-the-hand-backs-did-not-explain).

## Found and not changed

None has an engine caller that reaches it today.

- **`StartWorkers` / `StopWorkers` against a thread that is launching.** Only `RestartWorkers` runs
  the pending-wake-up handshake. A launch during the first `StartWorkers` indexes an empty waiting
  queue, and one during `StopWorkers` strands its task. `Application` starts before other threads
  exist and stops after the subsystems.
- **`FEvent::Wait` opens no oversubscription scope** (`WindowsEvent.h`), unlike UE. `ParallelFor`'s
  master only waits for batches a running thread already holds, so nothing deadlocks; a blocked
  worker is simply not replaced.
- **An `FThread` released on its own thread** waits for itself. Both ways in are asserted.
- **`Tasks::Launch` with a render-thread priority** queues to a thread nothing drains.
- **`ProcessTasks` never opens the re-entrancy scope** its comment describes, and
  `ProcessUntilRequestReturn` clears a return request made just before it. No callers.
- **A `TPromise` destroyed unset** leaves its waiters blocked; UE asserts.
- **The limiter's finite-timeout `Wait`** can return true with work pending once its event has
  fired; documented at the call.
- **`TLinearAllocatorBase`'s thread-local header** still points at its block after the cleanup
  object has handed the block back.
- **`FRunnableThread::SetThreadAffinity` from the creating thread** only caches a processor-group
  mask, so workers in a non-zero group never get it.
