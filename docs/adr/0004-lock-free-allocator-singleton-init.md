# Lock-free link allocator uses a never-destructed magic static, not `TLazySingleton` or UE's plain-bool init

`LockFreeLinkAllocator_TLSCache` (in
[`OloEngine/src/OloEngine/Memory/LockFreeList.cpp`](../../OloEngine/src/OloEngine/Memory/LockFreeList.cpp))
is the process-wide singleton that hands out lock-free list links. It must
satisfy two constraints that pull in different directions:

1. **It must outlive static destruction.** Other statics free links in their
   destructors during program exit, so the allocator's storage must still be
   valid after `main()` returns. Its destructor must therefore never run.
2. **Its first initialization can be concurrent.** Unlike Unreal, where the
   task graph first touches this allocator on the game thread during
   single-threaded startup, OloEngine first touches it from a **scheduler
   worker thread** — the lock-free fixed-size task allocator's free-list does a
   `Push`/`Pop` (which needs a link) while *freeing a Jolt job* on a worker (see
   the TSan stack in the decision history). So two worker threads can race to
   perform the first construction.

We initialize it with a **C++11 "magic static"** — a function-local `static`
pointer whose initializer placement-news the allocator into a never-destructed
static byte buffer:

```cpp
static LockFreeLinkAllocator_TLSCache& Get()
{
    alignas(LockFreeLinkAllocator_TLSCache) static u8 Data[sizeof(LockFreeLinkAllocator_TLSCache)];
    static LockFreeLinkAllocator_TLSCache* Instance =
        ::new (static_cast<void*>(Data)) LockFreeLinkAllocator_TLSCache();
    return *Instance;
}
```

The compiler guarantees the initializer runs **exactly once, thread-safely**
(via a guard variable), and the object is placement-new'd into storage whose
destructor is never invoked. This satisfies both constraints with no hand-rolled
synchronization.

## Considered options

- **C++11 magic static (chosen).** Race-free first-init for free; never
  destructed; the post-init hot path is a single guarded load (`movzbl guard;
  test; jne` on Linux/Clang — identical to UE's bool check; a cheap
  `_Init_thread_epoch` TLS compare on MSVC, no lock). Self-contained: correct
  regardless of *who* touches the allocator first.

- **UE 5.7's exact code — plain non-atomic `if (!bIsInitialized) { construct;
  bIsInitialized = true; }`.** Byte-identical to upstream and the leanest hot
  path, but it is **only safe because UE first-inits on the game thread before
  workers exist.** OloEngine first-touches it from a worker, so the plain form
  is a data race on `bIsInitialized` (TSan-reported: unsynchronized read on the
  fast path vs. the construction write). Restoring it would require reproducing
  UE's invariant explicitly — e.g. priming the allocator on the main thread in
  `FScheduler::StartWorkers` before any worker is created. Rejected: it buys a
  couple of MSVC instructions per call (noise next to the allocator's TLS lookup
  + lock-free CAS) at the cost of a fragile ordering invariant that silently
  re-breaks the moment any lock-free list is used from a thread before the prime.

- **Hand-rolled double-checked lock (the previous OloEngine version).** Added a
  `std::atomic_flag` spinlock around the construction, but the outer
  `if (!bIsInitialized)` fast-path check read the **non-atomic** flag *outside*
  the lock while the slow path wrote it *inside* — a textbook broken DCLP, and
  the exact race TSan flagged. Strictly worse than the magic static: more code,
  a spinlock on first-init, and a bug. Rejected.

- **`TLazySingleton` (we have a port at
  [`Misc/LazySingleton.h`](../../OloEngine/src/OloEngine/Misc/LazySingleton.h)).**
  Tempting because it is the idiomatic UE singleton — but UE deliberately does
  **not** use it here, and its own comment says why: *"a replacement for
  TLazySingleton, which will still get destructed."* Both UE's and our
  `TLazySingleton` run the instance destructor at exit (`~TLazySingleton →
  Destruct → ~T`), which violates constraint (1) — a shutdown use-after-free
  while other statics are still freeing links. Rejected for the same reason UE
  rejects it.

## Consequences

- The construction is provably race-free and the object is never destructed —
  both required properties hold without relying on call-ordering.
- We **intentionally diverge from UE 5.7's source** at this one function. The
  code comment at `Get()` records the divergence so a future contributor
  comparing against UE (as happened here) does not "simplify" it back to UE's
  plain-bool form (re-introducing the race) or swap in `TLazySingleton`
  (re-introducing the shutdown destruction hazard).
- Performance is effectively unchanged versus UE: the post-init hot path is a
  single guard check. If profiling ever shows the MSVC `_Init_thread_epoch`
  check is material on this path, the escape hatch is the "UE-literal + main
  thread prime" option above — but only with the priming invariant made
  explicit and enforced.
