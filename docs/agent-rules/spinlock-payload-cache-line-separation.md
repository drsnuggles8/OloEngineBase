# Don't put a lock and its guarded payload on the same cache line

Source: Fedor Pikus, *Lock-Free Programming is Dead* (C++Now 2026), closing slide.
Written from a repo-wide audit for a C++Now 2026 follow-up branch (issue-free —
see `HANDOVER.md` history on `feature/cppnow26-audits`).

## The mechanism

A thread that merely *reads* a lock — an unlocked fast-path check, a spin-wait,
a `TryLock`-and-bail — pulls that cache line into Shared state on its core. If
the lock and the payload it guards share a line, that read also drags the
payload along, even though the reader never touched the payload itself. When
the owner then does a genuinely exclusive operation on the lock (a CAS to
acquire, a release-store), the CPU has to pay an invalidate round-trip it
would not otherwise need — the owner's line was Exclusive, a reader's
unrelated peek downgraded it to Shared, and the owner has to re-acquire
Exclusive before it can proceed. This costs real cycles even when the lock
itself is uncontended in the traditional sense (no thread ever blocks on it).

The fix is mechanical once you've found a real instance: pad the lock (or the
payload) onto its own line with `alignas(OLO_PLATFORM_CACHE_LINE_SIZE)` —
**use the project's existing constant** (`OloEngine/src/OloEngine/Memory/Platform.h`),
don't invent a second one. `/wd4324` is already suppressed project-wide
precisely because this pattern's padding warning is expected, not a defect.

## Judgement: not every atomic-bool-next-to-a-mutex qualifies

The audit that produced this note grepped `Threading/`, `Task/`, `Async/`,
`Audio/`, `Core/` for `std::atomic<bool>` / `std::atomic_flag` / lock-shaped
members and found roughly two dozen candidates. Only one was worth fixing.
The bar that separated it from the rest:

1. **Is the flag actually read by threads that don't also take the lock?**
   A flag that's always read-then-immediately-locked (double-checked-locking
   idiom) doesn't get the "spinning reader avoids the lock" benefit from
   separation — the mutex's own cache-line traffic already dominates. This is
   why `Core/Ref.cpp`'s `LiveReferencesData::isValid` was **not** padded
   despite sitting next to `FMutex mutex` and being read unlocked at the top
   of every `Ref<T>` add/release/lock: every one of those call sites goes on
   to acquire the lock in the common (non-shutdown) path anyway. See
   [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) —
   that doc's own conclusion is the more important one here: the *global
   mutex itself* (one lock serializing
   every `Ref<T>` op process-wide) is the dominant cost, matching Pikus's
   framing that "the cost of the lock doesn't matter — disruption of
   execution flow matters." Cache-line padding is a rounding error next to a
   structural single-global-lock bottleneck; fix the bigger problem first, if
   it's ever profiled as hot.
2. **Is the path actually contended?** A flag touched once at init/shutdown
   (audio suspend flags polled by a 100µs-sleep teardown loop, async-load
   "ready" flags checked once per load) isn't worth a padding byte — and
   padding indiscriminately bloats every instance of a hot struct for no
   benefit. Several `std::atomic<bool>` candidates in `Audio/SoundGraph/`
   were rejected on this basis alone.
3. **Is the adjacency provable, not guessed?** A hand-wavy "these two members
   are probably on the same line" isn't enough to justify a change — either
   the containing struct is small enough that adjacency is obvious by
   inspection (the fix below), or it needs an actual `offsetof`/`sizeof`
   check before touching it. `Task/WaitingQueue.h`'s `FWaitingQueue` was
   considered (its `m_State`/`m_StandbyState` CAS traffic is about as hot as
   this codebase gets, with `m_IsShuttingDown` declared nearby) and rejected
   specifically because the real layout — depending on `TFunction`'s
   captured-callback size — couldn't be confirmed without compiling and
   inspecting it, and a wrong guess is worse than no fix.

## The one fix that cleared the bar

`Tasks::Private::FTaskBase::TSubsequents` (`OloEngine/src/OloEngine/Task/TaskPrivate.h`):

```cpp
TArray<FTaskBase*, AllocatorType> m_Subsequents;   // payload
std::atomic<bool> m_IsClosed{ false };             // read unlocked, every AddSubsequent()
FMutex m_Mutex;                                    // guards m_Subsequents
```

`PushIfNotClosed()` — called via `FTaskBase::AddSubsequent()` for **every**
prerequisite→subsequent edge registered anywhere in the task graph — reads
`m_IsClosed` unlocked before deciding whether to take the lock at all. A busy
task graph routinely has several worker threads racing to attach themselves
as subsequents of the same shared prerequisite task concurrently, so this is
a genuinely hot, multi-reader path — unlike the `Ref.cpp` case, here the
*unlocked* read is the common case that matters (a closed subsequents list
short-circuits before ever touching the mutex). The whole struct — a small
inline-allocator `TArray` plus a `bool` plus a 1-byte `FMutex` — comfortably
fits on one 64-byte line by default, so the adjacency needed no
`offsetof` archaeology to confirm.

Fix: `alignas(OLO_PLATFORM_CACHE_LINE_SIZE)` on `m_IsClosed`, separating it
(and everything declared after it) from `m_Subsequents`'s inline storage.

## Outcome of the audit

One site fixed out of ~2 dozen candidates surveyed across `Threading/`,
`Task/`, `Async/`, `Audio/`, `Core/`. That is the expected outcome, not a
sign the audit was too shallow — see the judgement criteria above. If a
future pass through a different subsystem turns up more, the same three
questions apply.
