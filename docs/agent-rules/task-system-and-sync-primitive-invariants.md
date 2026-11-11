# Task system and sync primitives: the invariants a threading review found broken

Applies to: `OloEngine/src/OloEngine/Task/`, `HAL/ParkingLot.cpp`, `HAL/ManualResetEvent.cpp`,
`Platform/Linux/LinuxEvent.h`, `Containers/FAAArrayQueue.h`, `Core/Ref.h`, `Core/Log.h`, and any
code that queues work to the game thread. Each rule was a bug; the reproduction for each is in
[the details](task-system-and-sync-primitive-invariants-details.md).

## Scheduler

1. **`RestartWorkers` wakes the workers after it clears the temporary-shutdown flag.** A thread
   that launches while the flag is set queues its task and wakes no one, and the new workers
   scan the queues only once when they start.
   [Details](task-system-and-sync-primitive-invariants-details.md#a-task-launched-during-a-restart-was-never-woken-for)
2. **Ask whether the thread is a worker through `TryGetTlsValues()`.** A task drained by
   `~FScheduler` runs after the main thread's scheduler values are destroyed.
   [Details](task-system-and-sync-primitive-invariants-details.md#isworkerthread-read-destroyed-thread-state)
3. **When one pass handles pending deletes and pending inserts, take the deletes first.** A thread
   that queued both between the two steps was freed while still on the insert list.
   [Details](task-system-and-sync-primitive-invariants-details.md#the-per-thread-registry-linked-a-freed-entry)
4. **Reserve a slot in a bounded array with a compare-and-swap loop, never add-then-undo, when
   readers walk the array up to the count.**
   [Details](task-system-and-sync-primitive-invariants-details.md#a-rejected-queue-registration-was-visible-to-stealers)
5. **After releasing a concurrency slot because the queue looked empty, look at the queue again.**
   A push that found every slot taken left its task to a holder that had already looked.
   [Details](task-system-and-sync-primitive-invariants-details.md#the-concurrency-limiter-stranded-a-task)
6. **Record a pipe's previous task as a prerequisite before becoming its subsequent.** As a
   subsequent the task can be unlocked, run and closed by other threads at once.
   [Details](task-system-and-sync-primitive-invariants-details.md#a-piped-task-recorded-its-predecessor-too-late)

## The game thread's queue

7. **A task queued to the game thread holds a gate, not the bare `this` of an object that can die
   first, and `Shutdown()` on the game thread never waits for a game-thread task.** The queue
   outlives an `EditorAssetManager`; use `EnqueueOnGameThread` there.
   [Details](task-system-and-sync-primitive-invariants-details.md#queued-game-thread-tasks-outlived-the-asset-manager)
8. **A release posted with `PostToGameThread` at teardown is run by `RunPostedGameThreadWork()`.**
   `~Application` calls it once the layers are gone. Never drain ordinary tasks there: they may
   use the layers that were just destroyed.
   [Details](task-system-and-sync-primitive-invariants-details.md#a-release-posted-at-teardown-never-ran)

## Waiting

9. **A timed wait computes one deadline and passes the kernel the time that is left, on the
   monotonic clock.** On Linux a timed-out wait lasted twice its span and spun a core for half.
   [Details](task-system-and-sync-primitive-invariants-details.md#linux-timed-waits-took-twice-as-long-and-spun)
10. **Nothing that runs inside `FOversubscriptionScope`'s constructor may park through the
    ParkingLot.** A blocking wait opens that scope after its wait node is queued. The
    oversubscription-limit event therefore uses an OS lock, its callbacks must not take `FMutex`,
    and `ParkingLot::Wait` asserts when a thread parks twice.
    [Details](task-system-and-sync-primitive-invariants-details.md#a-parked-worker-parked-again)
11. **A timed ParkingLot wait that finds itself dequeued takes the waker's notify before it
    returns.** Left pending, the notify ended the thread's next wait early.
    [Details](task-system-and-sync-primitive-invariants-details.md#a-stale-notify-ended-the-next-wait)
12. **An event wait that was released returns true, and an auto-reset waiter that finds its
    trigger taken keeps waiting.**
    [Details](task-system-and-sync-primitive-invariants-details.md#a-linux-event-wait-with-no-timeout-returned-false)

## Lock-free containers and shared registries

13. **Move a lagging tail off a queue node before the head leaves it.** A hazard pointer only
    protects a node that is unreachable once retired.
    [Details](task-system-and-sync-primitive-invariants-details.md#the-overflow-queue-retired-a-node-the-tail-still-named)
14. **A `WeakRef` names an object by address and generation, and the live registry is keyed by the
    `RefCounted` address.** An address alone resolves to whatever was allocated there next.
    [Details](task-system-and-sync-primitive-invariants-details.md#a-weakref-resolved-to-a-different-object)
15. **Attach a sink to the loggers at run time with `Log::AttachSink`, never through
    `logger->sinks()`.** A logger reads its sink list without a lock.
    [Details](task-system-and-sync-primitive-invariants-details.md#the-console-panel-edited-a-sink-list-other-threads-were-reading)
16. **`LockFreeLinksExhausted` aborts.** `OLO_CORE_FATAL` only logs, and the caller went on to
    index past the last page of links.

## Proving a race

17. **Widen the window, then show the fix closes the widened window.** Put a `usleep` between the
    two steps that race, on the unfixed code, until the fault is deterministic; put the same
    sleeps in the fixed code and run the same test. A stress test that never failed before the
    fix is not evidence.
    [Details](task-system-and-sync-primitive-invariants-details.md#how-these-were-reproduced)
18. **Reproduce a starved-runner fault on a Release build with busy processes sharing the cores.**
    Debug code is slow enough to hide a producer that outruns its consumers.
    [Details](task-system-and-sync-primitive-invariants-details.md#how-these-were-reproduced)

## Open, with no caller that reaches them today

Listed in [the details](task-system-and-sync-primitive-invariants-details.md#found-and-not-changed)
so the next reader does not rediscover them: `StartWorkers` and `StopWorkers` against a
concurrent launcher, `FEvent::Wait` opening no oversubscription scope, named-thread priorities
that nothing drains, and the rest.
