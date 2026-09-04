# A `thread_local` with a destructor is unreadable once it has run — keep the lifetime signal in a trivially destructible variable

**The rule:** if any code can read thread-local state from a static destructor, store the
"is it still there?" signal in a **trivially destructible** `thread_local` (a raw pointer, a
`bool`, an integer). Never read a member of a `thread_local` object that has its own
destructor and hope the bits survived. A trivially destructible `thread_local` gets no
dynamic initialisation and no `dynamic atexit destructor`, so its storage is valid for the
whole thread; an object with a destructor is *gone*, and reading its members afterwards is
undefined behaviour that happens to work until a compiler changes its mind.

## What it looked like when it bit

`FSchedulerTls` kept its per-thread state in a `thread_local FTlsValuesHolder`, an RAII
object owning a heap `FTlsValues`. Shutdown paths that can run from a static destructor used
a deliberately null-tolerant accessor:

```cpp
FSchedulerTls::FTlsValues* FSchedulerTls::TryGetTlsValues() noexcept
{
    // The thread_local holder's dtor sets this to nullptr before the storage
    // formally dies, so this read is well-defined even from atexit.
    return s_TlsValuesHolder.TlsValues;   // ← reading a destroyed object's member
}
```

The header said it out loud: *"its dtor zeroes out TlsValues, but the storage bits remain
readable."* That is an assumption about codegen, not a guarantee, and the destructor having
zeroed the member is irrelevant — the object it belongs to no longer exists.

It held for years on both platforms. Then the Windows toolchain moved to clang 23.1.0 and
`__dyn_tls_dtor` began running before the atexit destructors, so:

```text
WRITE of size 8 ... FScheduler::StopWorkers        Task/Scheduler.cpp:627
                    ~FScheduler
                    `dynamic atexit destructor for 's_Singleton'`
freed by       ... ~FTlsValuesHolder
                    `dynamic atexit destructor for 's_TlsValuesHolder'`
                    __dyn_tls_dtor
```

`TryGetTlsValues()` returned a dangling non-null pointer, `StopWorkers` wrote
`tls->LocalQueue = nullptr` through it, and every test process that had started the
scheduler died on the way out: **219 ctest failures across 73 suites, all after their
assertions had already passed.**

## Why the count is a lie, and how to read one like it

Every gtest case is its own ctest entry (`gtest_discover_tests`), so a single exit-time bug
is reported once per process that reaches it. 219 failures across "physics", "vehicles",
"navigation" and "cloth" was one bug in the task scheduler. When a failure list spans
subsystems that share nothing but `main()`, suspect process teardown before suspecting the
subsystems.

The tell is in the log: the suite prints `[  PASSED  ] 32 tests.` and *then* the sanitizer
report.

## The fix

Split the lifetime signal from the object that has the lifetime:

```cpp
// Trivially destructible: no dynamic init, no atexit destructor, storage valid
// for the whole thread. Safe to read from a static destructor.
static thread_local FTlsValues* s_TlsValuesPtr;
```

Publish it in the holder's constructor, retire it **first** in the holder's destructor —
before anything that can free the object — and have every null-tolerant accessor read it
instead of the holder. Hot paths keep using the reference-returning accessor, where a null
means a real bug rather than shutdown.

## Related rules

The same shape, solved the same way, in
[lazy-static-release-ownership.md](lazy-static-release-ownership.md) and in
`Platform/OpenGL/OpenGLDebug.cpp`, whose program-label registry is a deliberately leaked
heap singleton precisely because *"a namespace-scope map/mutex here could already be
destroyed by then — use-after-destruction AV at process exit."* Leaking and
trivially-destructible signalling are two answers to one question: **what is still alive
when a destructor asks?**

## A diagnostic prerequisite, learned at the same time

None of the above was readable from CI. `SetupConfigurations.cmake` compiles Release with
`/Zi`, but the linker was never given `/DEBUG`, so no program PDB was produced and
`llvm-symbolizer` fell back to the export table — every frame resolved to whatever exported
symbol happened to be nearest, which is where the `SteamNetworkingSockets_*` and `ffxFsr2*`
frames in old ASan reports came from. They are not evidence about networking or FSR2. If a
Windows sanitizer trace names an unrelated third-party symbol, check that a PDB exists
before believing the trace.
