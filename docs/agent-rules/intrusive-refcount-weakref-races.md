# Intrusive atomic refcounts: the two races a "null it out after delete" fix misses

Short rule for anyone touching `Core/Ref.h`'s `Ref<T>`/`WeakRef<T>`/`RefCounted`,
or porting the same intrusive-refcount + side-table-liveness pattern elsewhere.
Written from the audit for issue #596 ("`Ref<T>`: fix `DecRef()` dangling-pointer
window / ownership semantics").

## What the issue asked for vs. what the audit found

The issue opened with a single, narrow complaint: `Ref<T>::DecRef()` deletes the
object but never nulls `m_Instance`, so a `Ref` can (in principle) be read after
the object it pointed to has been freed — the pattern SonarQube's use-after-free
rule flags. The obvious "fix" is one line: `m_Instance = nullptr;` after `delete`.

That line is necessary but nowhere near sufficient. Auditing every place
`DecRefCount()`/`GetRefCount()`/`IsLive()` were actually read turned up two
*independent* races the one-line fix does not touch — both reachable from real
call sites, not just hypothetical:

1. **Decrement-then-reread is a TOCTOU double-free.** The old code did
   `m_Instance->DecRefCount();` (a plain `fetch_sub`, return value discarded)
   and then, in a *separate* step, `if (m_Instance->GetRefCount() == 0)`. Two
   threads racing the last two releases of the same object can each perform
   their `fetch_sub` (fine — that part is atomic and correctly serializes) and
   then *both* read the resulting count as `0` before either has acted on it —
   both conclude they're the last owner and both call `delete`. The fix:
   `DecRefCount()` must itself return the atomically-obtained post-decrement
   value (`fetch_sub(...) - 1`), and the caller must branch on *that* return
   value, never on a second, separate load. `Templates/RefCounting.h`'s
   `TTransactionalAtomicRefCount::ImmediatelyRelease` already does this
   correctly (ported from Unreal) — it was sitting in the same codebase as a
   working reference implementation the whole time.

2. **A liveness-registry `WeakRef` can resurrect into a freed object.** This
   design (an intrusive atomic refcount on the object, plus a global
   `RefUtils` mutex-guarded `unordered_set<void*>` of "live" pointers that
   `WeakRef::IsValid()`/`Lock()` consult instead of a real control block) is
   itself the "performance and thread-safety smell" the issue's TODO called
   out — and the audit found the actual gap: `Lock()` did `IsLive()` (mutex,
   released) *then*, as a separate step, unconditionally `IncRefCount()`. If a
   concurrent `DecRef()` on the last strong ref lands in that gap — after
   `Lock()`'s liveness check passes but before its increment runs — the
   releasing thread still deletes the object (it already decided the count
   hit zero), and `Lock()` hands back a `Ref` to freed memory. This is not
   theoretical: `Renderer/Font.cpp`'s font cache does exactly this
   check-then-lock across threads by design (the double-checked-locking
   comment there says so explicitly) — this was a real, live bug in
   already-shipped concurrent code, not just a defect in the abstraction.

## The fix shape (and why it isn't a full control-block rewrite)

Both races share one root cause: the "did the count hit zero" decision (made
by the releasing thread) and the "resurrect if still live" decision (made by
`Lock()`) were computed from two different, unsynchronized data sources (the
raw atomic count vs. the registry set) at two different times. The fix makes
both decisions go through the **same** critical section on the **same**
existing registry mutex:

- `RefUtils::Release(instance)` — called by every `Ref<T>::DecRef()` /
  `SafeDecRefAndDelete()`. Takes the registry lock and performs the decrement
  **itself**, *inside* that critical section, then removes the instance from
  the set only if the decrement it just performed brought the count to zero.
- `RefUtils::TryLockLive(instance)` — called by `WeakRef::Lock()`. Takes the
  *same* lock, checks the *same* set for membership, and only if present does
  it increment the refcount — also inside the one critical section.

Because the decrement (in `Release`) and the liveness check + increment (in
`TryLockLive`) are mutually exclusive via the same mutex, whichever one runs
first for a given instance determines the outcome for the other — no window
remains. This *does* mean every `DecRef()` now takes the registry mutex, not
just the terminal one — see "the fix that looked right and wasn't" below for
why the cheaper-looking alternative is actually broken. This is symmetric
with `IncRef()`, which has always taken this same mutex unconditionally (via
`AddToLiveReferences`) on every copy, not just the first construction — so
this isn't a new class of cost, just extending an existing one from Inc to
Dec.

**Why not a real control block (split strong/weak counts, Unreal
`TSharedPtr`/`TWeakPtr`-style)?** A true lock-free `weak_ptr::lock()` needs the
refcount's *memory* to survive the object's destruction (so a concurrent CAS
loop never touches freed memory) — that requires a separate heap allocation
outliving the object, i.e. an actual rewrite of the ownership model and every
`Create()`/`Ref<T>` call site's assumptions. The mutex-coordinated fix above
closes the *reachable* correctness gap (Font.cpp's real race) without that
blast radius. If a profile ever shows the registry mutex is a hot spot, that's
the trigger to revisit the control-block design — not before.

## The fix that looked right and wasn't (a lesson about "obviously correct" lock-free code)

The first attempt at closing race #2 kept the decrement **lock-free**
(`fetch_sub`, no mutex) and only acquired the registry lock *after* observing
the count reach zero, re-checking the live count under the lock before
deleting — reasoning being that the terminal decrement is rare, so why pay
the lock on every release. This reads as correct: the "did we hit zero" check
and the "resurrect" check both end up serialized through the same lock... but
only for the instant each one is *inside* the lock. The bug is the **gap
before** the lock: once a thread's lock-free `fetch_sub` observes the count
reaching zero, arbitrary time — a full OS preemption — can pass before that
thread ever calls into the locked section. In that gap, a concurrent
`TryLockLive()` can acquire the lock, resurrect the object (count 0→1),
return it to its caller, have *that* caller release it again (count 1→0),
and delete it — a complete, independent lifecycle — before the original
thread ever gets scheduled again. When it finally does run, it dereferences
an already-freed pointer to re-check the count; the freed memory occasionally
still reads back as zero, so it proceeds to `delete` a second time. This is
not a corner case that needs a research paper to find: the very first run of
`RefTest.cpp`'s `WeakRefLockDuringConcurrentReleaseNeverObservesDestroyedObject`
crashed the test binary with an MSVC debug-heap
`_CrtIsValidHeapPointer`/`is_block_type_valid` assertion — a real double-free,
reproduced in seconds. The fix (now in the code) performs the decrement
*inside* the lock, so there is no gap for a lock-free "I think I hit zero" to
go stale in.

**The takeaway that generalizes:** in code coordinating a decision (delete?
resurrect?) via a shared lock, *every* input to that decision must be read or
written while holding the lock — including the operation (like the decrement
here) whose *result* the decision depends on. Doing the operation lock-free
"because it's usually not the interesting case" and only locking for the
validation step leaves exactly the gap this bug lived in. And critically:
this kind of bug does not reliably show up from reading the code twice,
however carefully — it showed up from *running* two threads against a stress
test. Concurrent correctness claims for code like this need to be verified by
an adversarial, threaded regression test (ideally under ASan/TSan too), not
just re-derived on paper.

## The reusable lesson

When a codebase uses **intrusive** atomic refcounting (the count lives inside
the object) plus a **side-table liveness registry** standing in for weak
pointers, always ask two questions before trusting it, not just the one the
bug report names:

1. Does every "did the count hit zero" branch decide from a single atomic
   RMW's *own return value*, or does it re-read the count in a separate step?
   The second form is a TOCTOU race, full stop, regardless of how "obviously"
   atomic the individual operations look.
2. Can a "resurrect from weak" path (`Lock()`, `lock()`, `TryGetRef()`, …)
   observe "still alive" and then act on that observation in a step that
   *isn't* mutually exclusive with the path that deletes the object? If those
   two paths don't share a synchronization point, the registry is decorative.

Neither question is answerable by reading `DecRef()` in isolation — it
requires tracing every reader of the refcount/registry, which is why this
issue was correctly scoped as "spike first, then decide the fix size."

## Guard

`OloEngine/tests/RefTest.cpp` (`unit` layer) pins both fixes:
`DestructorRunsExactlyOnceUnderConcurrentRelease` stress-releases the same
object from multiple threads across many independently-allocated rounds and
asserts the destructor runs exactly once per round (would intermittently show
`destructCount > kRounds`, or crash/ASan-abort, on the old code).
`WeakRefLockDuringConcurrentReleaseNeverObservesDestroyedObject` races a
releasing thread against a `Lock()`-ing thread across many rounds and asserts
every successful `Lock()` reads a still-valid object (would ASan-fault as a
heap-use-after-free on the old code). Run the existing suite under the
`clangcl-asan` preset when touching this file again — these races are far more
likely to reproduce under ASan than in a plain Debug build.
