# Subsystem notes — core types, C++ traps, task system, threading

Accumulated gotchas from `Core/`, `Templates/`, `Task/`, `Memory/` and the untrusted-input
hardening work. Reference notes, not failure postmortems — see [README.md](README.md).

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. `.as<f32>(default)` returns NaN — the default does not protect you

yaml-cpp's `convert<float>::decode` explicitly recognises the YAML specials `.nan`, `.inf`,
`-.inf` (and case variants) and **returns `true`** with `rhs` set to `quiet_NaN()` / `±infinity()`.
So `node.as<f32>(default)` hands back the **non-finite value**; the default only applies when
decode *fails*, e.g. on `"abc"`. `std::stof("nan")` / `std::stof("inf")` likewise return non-finite
without throwing.

Every float read from a content YAML/JSON file must be `std::isfinite`-guarded **after** the
`.as<f32>()` / `std::stof()` call. This is `cpp-coding-quality.md` §2b; the Quest/Item/Dialogue
content DBs were hardened with a per-file `SanitizeFinite` helper (non-finite → fallback +
`OLO_CORE_WARN`).

> **To write a test that actually exercises such a guard, put `.nan` / `.inf` in the YAML.** A
> string like `"notanumber"` merely hits the default path and proves nothing.

## 2. Hardening untrusted float boundaries

- **Network wire reads.** `FArchive`'s `operator<<(f32&)` reads raw bytes with no validation.
  Sanitize on the **load path only** (`if (ar.IsLoading())`) via `SanitizeWireFloat` /
  `SanitizeWireVec3` (`std::isfinite(v) ? v : fallback`). Fallbacks: translation/rotation → 0,
  scale → 1, mass → 1 and forced `>= 0`. Reordering reads inside a `Serialize` is safe as long as
  the `ar <<` **call order** is unchanged — that order *is* the wire byte layout.
  `EntitySnapshot::CaptureDelta` hard-codes `kTransformBytes = 9*sizeof(f32)`, so keep the float
  count stable.
- **The crown-jewel UB:** `SnapshotInterpolator` cast a `NaN` renderTick to `u32`. A NaN slips past
  `renderTick < 0.0f` because **every** NaN comparison is false. Fix at **both** the setter
  (`SetRenderDelay` rejects non-finite/negative, warns, keeps the prior value) **and** the cast
  site.
- **Env-var config.** A non-finite ratio becomes `+inf`; `inf >= 1.0f` is true, so infinity reached
  `ceil(workers*ratio)` → `static_cast<i32>` UB → a bad `new[]`. Factor the parse into a **pure
  function returning `std::optional<f32>`** so a unit test can hit it directly with
  `"inf"`/`"nan"`/`"0.5"`/`"64"`; the caller warns on `nullopt`. This optional-returning pure
  function is the go-to shape for making global-reading config parse testable.
- **Do not use `std::atof`.** SonarCloud `cpp:S989` flags it CRITICAL (UB on overflow, no error
  reporting) and it fails the "D Reliability on New Code" gate — this bit PR #400. Use
  `std::strtof(env, &endPtr)`, reject when `endPtr == env`, and keep the `!isfinite || out-of-range`
  guard (overflow returns `±HUGE_VALF`). `strtof` is not flagged and — unlike floating-point
  `std::from_chars` — is available on the Linux/Clang (libc++) CI.

## 3. `Ref<T>` is const-propagating

Unlike `std::shared_ptr`, a `const Ref<T>&` yields a `const T*` through `operator->`. Calling a
non-const method on the pointee fails with MSVC **C2662** ("cannot convert 'this' pointer …
Conversion loses qualifiers").

If a function only reads the asset, `const Ref<T>&` is correct and self-documenting. If it must
call **any** non-const method, take `Ref<T>` **by value** (a cheap refcount bump) or by non-const
reference.

## 4. `ComputeShader` is not a `Shader`

They are **unrelated siblings** — both derive directly from `RendererResource`, with no base/derived
relationship. A `Ref<ComputeShader>` therefore cannot be delivered through any `Ref<Shader>` API;
the converting constructor's `std::is_convertible_v<T2*, T*>` static_assert fails to compile.

This is a recurring footgun: `GPUResourceQueue`'s `CreateShaderCommand` shipped a stubbed compute
branch whose intended one-liner *could never have compiled*. The fix was a dedicated
`CreateComputeShaderCommand` with its own `std::function<void(Ref<ComputeShader>)>` callback,
mirroring the one-command-per-resource-type pattern.

Async compute loading now exists end to end: `ComputeShader::CreateFromFileAsync(path, onReady)`
reads and `#include`-preprocesses the `.comp` on a worker (CPU/disk only, both steps thread-safe),
then enqueues the command whose `Execute()` builds the program on the main thread. **The 15+
existing compute systems still load synchronously at init and hard-bail on a null shader**, so
they are *not* safe to convert without restructuring their init into a deferred-ready state — async
is opt-in for new callers.

## 5. A fire-and-forget `FTask` must never `delete this` in its own body

`~FTask()` asserts `IsCompleted()`, but the scheduler sets `CompletedFlag` only *after* the runnable
returns: `ExecuteTask()` moves the runnable to a stack local, calls it, sets the flag, then the
local destructs. So a deleter **captured in the runnable** fires post-completion — that is the
documented `TDeleter` contract.

`Async.h`'s `AsyncTask(...)` and `Async(TaskGraph/ThreadPool, …)` originally ended the runnable with
`delete Task;` — i.e. while still `Running` (ETaskState 6). That tripped the assert and handed the
scheduler a freed task, **crashing `FScheduler::StopWorkers()`** the moment any such task actually
ran. It was latent because `AsyncTask` had no callers. Log signature:
`Assertion Failed: Task must be completed before destruction. State: 6`.

**Root cause was a porting mistake, not an upstream UE bug.** UE's `AsyncTask` is a `TGraphTask`,
and its `Async(ThreadPool)` is an `IQueuedWork` whose `DoThreadedWork()` legitimately ends with
`delete this` (no completion assert). The port reimplemented both on raw `FTask`, which forbids it.

The fix matches UE literally: own the heap task in a small struct and clean up via
`LowLevelTasks::TDeleter` captured **by value** in the runnable, with no body-side delete.
`TDeleter` is move-only and nulls itself on move, so it fires exactly once.

> A non-`Application` test binary using the scheduler must start it the `FunctionalTest` way:
> `LowLevelTasks::InitGameThreadId()` + `FNamedThreadManager::Get().AttachToThread(GameThread)` +
> `StartWorkers()`, with no matching `StopWorkers`.

## 6. Scheduler shutdown vs. standby workers is the recurring race class

`Task/` is a port of UE5's `LowLevelTasks` (Vyukov EventCount + standby-worker oversubscription).
The bug class is **races between shutdown and the standby-worker lifecycle**
(`FWaitingQueue::StartShutdown` / `ConditionalStandby` / `CommitStandby` / `TryStartNewThread`).

Issue #359's root cause: `StartShutdown` drained the standby stack by walking `Node->Next` **in
place** while triggering each node's event, *without* changing `m_StandbyState`. A woken
`ConditionalStandby` worker re-pushed itself onto a head it still owned → `Node->Next == self`
self-cycle → the drain spun in `SetEvent` forever. Fix: drain via an atomic
`m_StandbyState.exchange(StackMask)` and read `Next` **before** `Trigger()`.

Two invariants to preserve when touching this subsystem:

1. Any waker of a node-stack parker must **atomically change the shared head** (pop/exchange), never
   signal in place.
2. Always capture `Node->Next` **before** signalling a node — it may be reused instantly.

Stress-verify with `scripts/repro-flaky-test.ps1` under `OLO_TASK_GRAPH_NUM_WORKERS=4`, 2-core
affinity and a high `--gtest_repeat`.

### Debugging signature for a scheduler hang

- **The hang is often not in the test body.** The log can show `[ RUN ]` *and* `[ OK ]`, then
  nothing — each `--gtest_repeat` iteration calls `SetUpTestSuite`→`StartWorkers` and
  `TearDownTestSuite`→`StopWorkers`, so the hang is in teardown. Tell: the dump has **no task-worker
  threads left** (only main, the Windows threadpool and Tracy) and main spins in `SetEvent`.
- **Release stacks are useless** — ICF folding misreports symbols as `pugi::*` /
  `SteamNetworkingSockets_*`. Rebuild Debug and re-run the repro.
- **Inspect state, not just stacks.** In cdb on the full dump, `~0s; .frame N; dv /t` gave the
  drain's `LocalState`/`Node`, then `?? this->m_StandbyState._Storage._Value` and `dq <node> L2`
  revealed the self-cycle. That memory inspection is what pinpointed it; stacks alone would not
  have.

## 7. The lock-free allocator: two testing traps

`TLockFreeAllocOnceIndexedAllocator` (`Memory/LockFreeList.h`) **never frees its pages** — alloc-once
by design, matching UE5.8.

- **LSan trap.** A *local* instance in a test leaks every page on destruction, and the
  `asan-lsan-linux` job runs `detect_leaks=1`. Production is clean only because LSan reports
  *unreachable* allocations: the global `FLockFreeLinkPolicy::s_LinkAllocator`'s pages stay pointed
  to by its static `m_Pages[]` at exit. **Drive the singleton, not a fresh local instance** — it is
  also the faithful production path, and each gtest case runs in its own process so consuming
  indices doesn't pollute other tests.
- **The obvious TSan test is broken.** Do not build a producer/consumer test where one thread
  `Alloc`s and another reads the item's contents. The acquire consume load pairs with the
  page-publish CAS, so it orders only what happened *before* publish — the link's contents are
  written *after*. A relaxed index handoff makes the construct-vs-read its own data race (TSan
  flags the test, not the allocator); a release/acquire handoff fixes that but *masks* the load
  being tested. **The only cleanly reproducible race is concurrent `Alloc` first-touching the same
  page** — many threads racing, one wins the publish CAS, losers consume and construct. That is the
  actual reported race, and it is TSan-clean with the fix and reddens if weakened.

## 8. `OLO_CORE_VERIFY`, not `OLO_CORE_ASSERT`, for a startup invariant

`OLO_CORE_ASSERT` is gated on `OLO_ENABLE_ASSERTS`, defined **only in Debug** — in Release/Dist it
compiles to `((void)(condition))` and the check is gone. `OLO_CORE_VERIFY` is gated on
`OLO_ENABLE_VERIFY`, which is **always** defined.

So a programmer-error invariant that must surface in non-Debug builds uses `VERIFY`.

> **Testing implication:** a failure debug-breaks the Debug test binary, so don't drive the reject
> path through the asserting function. Expose a **pure validator** (e.g.
> `McpServer::IsValidToolName`), unit-test that for rejection, and feed the asserting wrapper only
> valid inputs.

## 9. The primitive typedefs are at global scope, not in `namespace OloEngine`

`u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 sizet uptr iptr` are declared in `Core/Base.h` **after** the
`namespace OloEngine { … }` block closes. `OloEngine::u8` is a compile error (MSVC C2039 plus a
cascade of template-arg errors).

Inside a `using namespace OloEngine;` block the unqualified name still resolves via ordinary global
lookup, which hides the issue — but in a **file-scope function signature** written before any
`using`, you must use the plain global name. Real engine types (`FMemoryWriter`, components,
`ComponentReplicator`) *are* in the namespace and do need the qualifier.

## 10. An unqualified name in a template needs its own include

`Algo/BinaryHeap.h` called unqualified `Swap(A, B)` in template functions but never included
`Templates/UnrealTemplate.h`, where `OloEngine::Swap` is declared. It compiled everywhere purely
because something else always happened to include that header first — a textbook
non-self-contained-header violation.

It surfaced only when a **new** test file pulled in a chain reaching `Algo::Sort` with `T=void*` — a
type with no associated namespace, so ADL couldn't find `Swap` either. The error pointed at
`BinaryHeap.h(198)` deep inside an unrelated instantiation chain and looked nothing like a missing
include.

**If a new or reordered TU hits a `C3861`-style error deep in a template chain that doesn't relate
to your change, suspect a missing include for a name used unqualified inside a template** — not a
bug in your new code.

## 11. Codegen-mirror tests need a holder struct, not a reference parameter

A test mirroring OloHeaderTool's generated serializer code (the
`NestedStructSerializerCodegenTest.cpp` / `ContainerSerializerCodegenTest.cpp` pattern) must access
the field as `holder.Field` — a class-member-access expression — never as a bare reference
parameter named after the field.

`decltype(expr)` on an unparenthesized id-expression naming a **reference** yields the reference
type itself, and `(const T&)::value_type` does not compile. On a member-access expression it yields
the **member's own declared type**, ignoring the object's reference/const qualification — which is
exactly what the generator relies on (`decltype(comp.m_Field)::value_type`, where `comp` is often
`const T&`). Skipping the holder indirection produces cascading C2651/C2039/C3312 errors that look
like unrelated syntax problems deeper in the function.

## 12. Two "unused" things in the UE-ported headers are kept on purpose

- **`FRefCountedObject`** (`Templates/RefCounting.h`) is provably unused but retained: the file is a
  faithful port of UE5.7's header, and `Task/TaskConcurrencyLimiter.h` carries a comment anchoring
  on it being "the non-atomic legacy type" to justify using `FThreadSafeRefCountedObject`. Deleting
  it would falsify that documented parity model and dangle the cross-reference. It carries a
  "KEPT FOR UE5.7 PARITY" note so it isn't re-litigated.
- **`ENamedThread::RenderThread`** is reserved but unattached — nothing drains its queue.
  `EnqueueRenderThreadTask` used to silently drop work; it is now a hard **compile error** (a
  dependent `static_assert`, never fires since there are zero call sites) while the enum and queue
  scaffold stay intact for future multithreaded rendering.

**Before deleting an "unused" type in these or sibling UE-ported headers, grep for cross-file
comments referencing it and check the file-header "Ported from UE5.x" note.** If parity intent
exists, leave it and downgrade to the safe fix.

## 13. The gameplay RNG is deliberately not cryptographic

`RandomUtils::GetGlobalRandom` (PCG32, `Core/FastRandom.h`) is a high-quality **statistical** PRNG.
For determinism, replay and rollback you *want* the seed known and shareable — every lockstep peer
must run the identical stream, so secrecy is impossible and undesired. Hardening the seed buys
nothing anyway: an observer of a handful of PCG32 outputs can reconstruct the internal state
regardless.

When roll secrecy genuinely matters (competitive multiplayer with a trustable economy) the fix is
**architectural, not cryptographic**: an authoritative server owns the RNG and ships only results,
plus server-issued item IDs, transactional trades, and idempotency/sequence numbers so a replayed
packet can't apply twice. That is the opposite trust model from lockstep.

**Don't "upgrade" the gameplay RNG to a CSPRNG or obscure the seed** — it breaks determinism without
adding real security.

## 14. Environment variables: one reader, and don't add a modal escape hatch

`Core/Environment.h` is the **only** place the engine calls `getenv`. Use
`Env::Get` / `IsTruthy` / `IsExactly` / `GetInt`; do not add a private wrapper.

This was consolidated from ~60 scattered call sites, eight of which were private
near-identical `IsTruthyEnvironmentVariable` copies. They had already drifted:
some sites tested `strcmp(v, "1") == 0`, others `*v != '0'`, others
`value[0] != 'f'`. So two variables documented the same way behaved differently,
and `FOO=true` silently meant *off* at half of them. Pick the accessor that
matches your intent:

| you want | use | note |
|---|---|---|
| an on/off toggle | `IsTruthy` | set, non-empty, not starting `0`/`f`/`F` |
| "only this exact value acts" | `IsExactly` | a typo must NOT read as on |
| a number | `GetInt` | `from_chars`; rejects garbage instead of `atoi`'s silent 0 |
| a path or free string | `Get` | empty is reported as unset |

Two rules that come with it:

- **Read once, at startup, into your own state.** `getenv` hands back a pointer
  into shared static storage, and `McpDispatchTest` sets and restores variables
  around a case. Reading once during init keeps you clear of that, and it is why
  the cpp:S990 suppression in `Environment.cpp` is honest rather than
  hand-waved.
- **`Env::Get` returns `std::optional<std::string>`, which fmt cannot format.**
  Log `*value`, not `value` — passing the optional is a wall of
  `type_is_unformattable_for` template spew that names the *formatter*, not your
  call site. Cost one build cycle during the consolidation.

### Debug levers go in the registry, not in a fresh `Env::IsTruthy`

`Core/DebugLevers.h` + `DebugLevers.inl` hold the ~21 switches you flip for one
run of an already-built binary — transient poisoning, bindless routing, the
threading and terrain-LOD bisection switches, the task-graph tuning knobs.
**Add a lever to `DebugLevers.inl` and nowhere else.** The accessors, the
setters, the environment seeding and the enumeration are all generated from
that one table, so there is no second list to update.

The environment is still the *input* — that part was never wrong. What was
missing is everything around it:

- **No list.** Twenty-one independent `Env::IsTruthy(...)` reads across twelve
  TUs. "Which levers exist?" had no answer but a grep, and "which are on in
  this session?" had none at all — an editor launched hours ago with
  `OLO_RG_POISON_TRANSIENTS` set looked identical to a clean one.
- **Not addressable from code.** A test or tool that wanted one on had to write
  the environment. That is the `_putenv_s` the test harness carried.

So now: `Levers::LogActive()` prints the non-default levers at startup (silent
when everything is default), and `olo_debug_levers` answers the same question
against a running editor. `DebugLeversTest` fails if a new `Env::` read of an
`OLO_*` variable appears anywhere in `OloEngine/src` — which is exactly how the
previous 21 accumulated, each one individually reasonable.

Two things to get right when adding one:

- **Pick the right shape.** `TOGGLE` is lenient (`Env::IsTruthy`). `EXACT` only
  accepts `"1"`, for a lever where a typo silently disabling the fast path
  would read as a mysterious performance cliff rather than a visible failure.
  `TRISTATE` exists because two task-graph knobs must distinguish "force off"
  from "leave the hardware-derived default alone" — flattening that into a
  toggle would change behaviour. `INT`/`NUMBER` return `optional` so
  set-to-zero stays distinguishable from unset, which is what `std::atoi`
  silently destroyed at the old call sites. `TEXT` has no setter, because every
  text lever is a path consumed once at init.
- **Seeding is lazy and once.** A setter marks the lever overridden *before*
  seeding, so a set that happens before any read survives the seed the first
  read triggers. Get that order backwards and the environment quietly wins.
  A subsystem that cached the value at init still won't see a later change —
  check the consumer.

This is deliberately **not** a console-variable system: no name-based lookup, no
editor console, no persistence, and no runtime change without a restart. It is
the registry such a system would need underneath it — that layer is issue #821,
which would also retire `FTaskPriorityCVar` in `Task.h`, a UE port whose own
constructor says the engine has no console-variable system yet and which has
zero call sites.

### An environment variable is the wrong mechanism for a knob you own

Reach for `Env::` when the value comes from **outside the process** — the OS
(`HOME`, `APPDATA`, `COMPUTERNAME`, `XDG_*`), or a launcher configuring a child
it started (`driver.ps1` handing the editor its per-worktree MCP port). Also for
a **one-run debug lever on an already-built binary**: `OLO_RG_POISON_TRANSIENTS`,
`OLO_RHI_BINDLESS`, `OLO_NO_THREADING`. Those are all genuinely environmental.

For a knob the process owns, a **command-line flag** is strictly better, and the
test suite is the worked example. It had twelve variables read from 26 sites;
they are now `--olo-*` flags in `OloEngine/tests/TestOptions.{h,cpp}`, parsed in
`main` before `InitGoogleTest`. Three things went wrong that flags make
impossible:

* **Invisibility.** A flag is in the command line CI already prints. A variable
  set three YAML levels up is not, so "why did this run rebase the goldens?"
  was archaeology.
* **Readers disagreeing about their own values.** The AMD conformance workflow
  carried a comment explaining it had to emit `'0'` and not `'false'`, because
  one of the three `OLOENGINE_GOLDEN_REBASE` readers tested only the first
  character — so `false` *enabled* a rebase there. Present-or-absent has no
  such class of bug.
* **Leaking into children.** Environment is inherited. `McpHeadlessAttachTest`
  spawns an editor and was handing it the whole set by accident.

A fourth, subtler one: an env-seeded `static const bool` **latches**. Five
renderer TUs each had their own copy of the `OLO_RENDERGRAPH_DIAGNOSTICS` gate,
so whichever asked first froze the answer process-wide — which is why the test
`main` had to *write* the environment to reach it. One accessor
(`Renderer/RenderGraphDiagnostics.h`, with a `Set…` alongside the `Is…`) removed
both the duplication and the only place the engine mutated its own environment.
If you find yourself writing `_putenv_s`/`setenv` to reach your own code, the
value wants a setter, not a variable.

Unknown `--olo-*` flags are deliberately **fatal**: a silently-ignored
`--olo-golden-rebse` would reproduce exactly the invisibility being replaced.
`--olo-help` lists them.

### Do not add a fourth way to skip a modal

`Core/Interactivity.h` answers "is anyone at the keyboard?" once, for the
process. Before it existed the engine had grown three separate escape hatches,
each added *after* someone lost time to a hang:

* `OLO_EDITOR_AUTOSAVE_RECOVERY` — the auto-save recovery modal (#316 Part 5)
* `OLO_EDITOR_UNSAVED_PROMPT` — the unsaved-changes modal
* the assert dialog — `OLO_CORE_ASSERT` called `MessageBoxA` unconditionally (#714)

That is one problem wearing three hats: **a blocking modal in a process nobody
is watching is a hang, not a prompt** — and it does not look like a failure. The
process sits in `NtUserWaitMessage` burning ~0% CPU, which reads as "slow" for
as long as you are willing to believe it. The tell is flat CPU time against
climbing wall-clock; confirm with `cdb -p <pid> -c "~0 kn 24; qd"` and look for
`USER32!MessageBoxA`.

So: **any new modal asks `IsNonInteractive()` first**, and the automated path
takes the *least destructive* answer — "cancel", "keep what is on disk" — never
the convenient one. A host that knows it is automated calls
`SetNonInteractive(true)` at startup (the test binary already does).

## 15. A "benign" data race you documented is still a failed TSan job

Issue #704's `GPUHashMap` shipped its first round with this in the class comment:

> concurrent writes to the SAME key are last-writer-wins with no value-tearing
> guarantee (the value store is not atomic — the reference has the same contract).

That is a data race described in prose. The keys were claimed with a CAS through
`std::atomic_ref`, but the *value* store next to it was a plain write, so two
threads inserting one key wrote the same bytes unsynchronized. Writing the
consequence down does not make it defined: C++ has no "benign race", and the
gating `tsan-linux` job failed on exactly the two tests that exercised it — while
the full 6100-test suite passed clean on Windows, where **no TSan exists**
(neither MSVC nor clang-cl ships it).

Two things generalise:

**Narrow the contract instead of documenting the race.** The fix was not to make
the value atomic — `V` is caller-supplied and `ObjectAllocation` is 16 bytes, so
`atomic_ref<V>` would take a lock and tax the single-writer path every engine
consumer actually uses. It was to state what is genuinely supported and true:
concurrent `Insert`/`Erase`/`Find` of **distinct** keys is lock-free-safe
(including keys colliding on one slot), and same-key mutation is unsupported.
Ask what concurrency the consumers need before paying for concurrency nobody
asked for.

**Then make the tests contend the thing you actually claim.** The old test had 16
threads hammering one key — which only exercised the race. The replacement gives
each thread its own keys, all chosen (by brute force over the real hash) to land
in the **same start slot**: maximal contention on the claim CAS, zero shared
value writes. That tests the guarantee the class now makes, and it is a stronger
test than the one it replaced, not a weaker one. The general trap: when a
sanitizer flags a test, check whether the test is asserting a property the code
should not have been offering — deleting or muting it is the wrong repair, and so
is loosening the code to match a bad test.

Corollary for planning: any change adding atomics or a multithreaded test should
assume a CI round is spent on TSan alone, because it cannot be reproduced on this
platform. Get the report from `gh api repos/<o>/<r>/actions/jobs/<id>/logs` and
grep `WARNING: ThreadSanitizer` — the `SUMMARY:` line names both conflicting
accesses by file:line, which is usually enough to fix without reproducing.
