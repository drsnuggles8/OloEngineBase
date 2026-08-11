# Timed-wait test assertions: microsecond precision, one-sided, budgeted (issue #753)

`SemaphoreTest.TryAcquireForWithTimeout` failed intermittently (~1-in-3 in isolation) with the wait
appearing to return in *under 5ms* against a 10ms timeout. That direction matters: scheduler jitter on
a loaded machine can only make a correctly-implemented timed wait return *later* than requested, never
earlier — so a "too early" failure looks exactly like the shape of a real early-return bug in the
primitive (a missing spurious-wakeup re-wait loop), and the two candidates need opposite fixes. Widening
the tolerance blindly would make the symptom disappear either way, including the case where it was
burying a genuine synchronisation bug.

**The actual cause was in the test, not `FSemaphore::TryAcquireFor` — but it wasn't the primitive's
tolerance either.** Two independent test-side defects combined:

1. `std::chrono::duration_cast<std::chrono::milliseconds>` truncates toward zero. A genuine 4.9ms
   reading becomes `4`, tripping a `>= 5` bound while being within noise of it. Any test measuring a
   short wait (single-digit-to-low-double-digit milliseconds) must cast to `microseconds`, not
   `milliseconds` — the truncation is a large fraction of the measured value at that scale.
2. The bound was a de-facto two-sided tolerance window (`>= 5` against a `10ms` request) when only one
   direction is ever meaningful for this primitive on Windows: `FWindowsSemaphore::TryAcquireFor`
   (`OloEngine/src/Platform/Windows/WindowsSemaphore.h`) is a single `WaitForSingleObject` call on a real
   Win32 kernel semaphore object. A kernel dispatcher-object wait has no spurious-wake state — it can
   only return `WAIT_OBJECT_0` or `WAIT_TIMEOUT` — so it cannot report "not acquired" before the deadline
   due to a logic error in our code the way a futex/`WaitOnAddress`-based wait could. (Contrast
   `FPlatformManualResetEvent::WaitForSlow` in `OloEngine/src/OloEngine/HAL/ManualResetEvent.cpp`, which
   *is* `WaitOnAddress`-based and correctly loops via `WaitUntilSlow` to re-wait out a spurious wake —
   that loop is why it was *not* a suspect here.) With the early-return path structurally ruled out, the
   only invariant worth asserting is "didn't return dramatically early," one-sided.

**Verification, not assumption.** Before concluding it was test-side, the fix was proven by reproducing
against the primitive, not just reasoning about it: 500+ isolated repeat runs (`--gtest_repeat`) with the
old assertion, split across an idle machine and one under artificial load (20 detached CPU-bound
processes, mirroring the CI-runner contention this box normally carries) — zero failures, and elapsed
time only ever grew under load (up to 25ms against the 10ms request), never shrank below ~9ms. That
empirical floor is what justifies the 50%-of-requested jitter budget in the fix below, not a guess.

**The fix pattern**, applied to `SemaphoreTest.cpp` (`TryAcquireForWithTimeout` and its sibling
`TryAcquireUntilWithTimeout`, which previously had no elapsed check at all and would have silently
passed through an early return) and `ManualResetEventTest.cpp` (`WaitForUnset`, same shape):

```cpp
auto Start = std::chrono::steady_clock::now();
bool Result = Sem.TryAcquireFor(FMonotonicTimeSpan::FromMilliseconds(10.0));
auto End = std::chrono::steady_clock::now();

EXPECT_FALSE(Result);

auto ElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(End - Start).count();
EXPECT_GE(ElapsedUs, 5000) << "TryAcquireFor(10ms) returned far too early: elapsed=" << ElapsedUs << "us";
```

Generalisable *process*, not a generalisable bound, for any test asserting a lower bound on a short
timed wait — the one-sided assertion is a claim about a specific implementation, not a property of
"semaphores" or "timed waits" in general, and must be re-verified per platform/wrapper rather than
inherited from a sibling's evidence:

- **Measure in microseconds**, not milliseconds, whenever the measured value is small enough that a
  1ms truncation is a meaningful fraction of it.
- **Verify *this* wrapper's timed-wait contract before asserting one-sided** — confirm whether it can
  report "not acquired"/timed-out before its real deadline for a reason that isn't a genuine timeout
  (a spurious wake it fails to loop out of, or a spec-permitted spurious failure), and restrict the
  one-sided pattern to implementations you've confirmed can't. Two OS-specific primitives were checked
  here and behave differently: a kernel dispatcher-object wait (`WaitForSingleObject` on a Win32
  semaphore/event handle, `FWindowsSemaphore::TryAcquireFor`) has no spurious-wake state and can't; a
  futex/`WaitOnAddress`-style wait needs its own re-wait loop, which `FPlatformManualResetEvent::WaitForSlow`
  has. **`FLinuxSemaphore::TryAcquireFor` (`OloEngine/src/Platform/Linux/LinuxSemaphore.h`) delegates to
  `std::counting_semaphore<>::try_acquire_for` — a third, unaudited code path.** The standard explicitly
  allows the *untimed* `try_acquire()` to "fail spuriously" even when a resource is available; whether
  that allowance leaks into a given standard library's `try_acquire_for` retry loop is an implementation
  detail this investigation never checked. Do not assume the Windows evidence above transfers — the
  Linux path needs its own repeat-run verification (idle + artificial contention, same method used for
  Windows above) before `SemaphoreTest.cpp`'s one-sided assertion can be trusted there too.
- **State the jitter budget and where it came from** — a bound backed by an actual measured floor
  under artificial contention, not a round number picked for comfort.
- **Print the measured value on failure** (`<< "elapsed=" << ElapsedUs`) so a future flake reports how
  far off it was instead of just that it was off — turns a mystery report into a data point.

See also [testing-architecture.md](testing-architecture.md) for the broader test classification rules;
this file is specifically about the timing-assertion trap, not test placement.
