// LockDebug.h - same-thread re-entrancy detection for non-recursive locks
//
// Turns a silent, permanent park into an immediate assert naming the offending lock.
//
// WHY THIS EXISTS
// ---------------
// FSharedMutex does not support recursive locking, and an exclusive lock and a shared
// lock may not be held simultaneously by the same thread. Break either rule and the
// thread parks in ParkingLot::Wait forever: no assert, no log line, no CPU, no crash
// record — a wedged process that looks identical to a slow one. The editor's game
// thread was lost to exactly this twice in one day (issues #439 and #863, the same
// line reached by two different triggers), and each cost a live debugging session with
// cdb to find something the process could have said itself in a microsecond.
//
// Documenting the rule was not enough, because the rule is invisible at the call site:
// `SerializeAssetRegistry()` looks like an ordinary helper, and whether it is safe to
// call depends on a lock acquired several frames up the stack. This detector makes the
// composition itself loud. See
// docs/agent-rules/non-recursive-lock-self-locking-helper.md.
//
// HOW IT WORKS
// ------------
// A thread-local table of the locks this thread currently holds, keyed by address. The
// mutex objects themselves are NOT modified and NOT grown — sizeof(FSharedMutex) is
// four bytes in every configuration, exactly as before, so there is no layout change to
// go stale in an incremental build and nothing for a concurrent branch to rebase onto.
//
// COST
// ----
// Compiled out entirely unless OLO_LOCK_DEBUG (defaults to !NDEBUG; see below). When on,
// each acquire/release scans a small thread-local array. FSharedMutex guards
// manager-and-registry-granularity state, not per-task hot paths, so this is noise
// against a Debug build. FMutex is deliberately NOT instrumented: it is used inside the
// task scheduler's hot path and inside the locking primitives themselves, where the
// cost and the re-entrancy risk are both different questions.
//
// LIMITS (deliberate, and all fail toward silence rather than a false alarm)
// -------------------------------------------------------------------------
//  * A lock acquired on one thread and released on another is not tracked correctly.
//    That is already a bug for these types, and every in-tree acquisition goes through
//    the scoped TUniqueLock / TSharedLock guards, so acquire and release are the same
//    thread by construction.
//  * Beyond kMaxTrackedLocks simultaneously-held locks on one thread, tracking stops
//    and detection silently degrades to off for that thread rather than guessing.

#pragma once

#include "OloEngine/Core/Base.h"

// Gated on NDEBUG, deliberately NOT on OLO_DEBUG.
//
// OLO_DEBUG is a PRIVATE compile definition of the OloEngine target, so it is absent
// when this header is compiled into OloEditor or OloEngine-Tests (verified: those TUs
// receive neither OLO_DEBUG nor NDEBUG). FSharedMutex::Lock is an inline function, so
// gating its body on a macro that differs between translation units is an ODR
// violation, and the linker would pick one definition arbitrarily -- meaning the
// detector could silently vanish from exactly the build relying on it. The same
// reasoning is why ENTT_USE_ATOMIC is PUBLIC on the OloEngine target (see CLAUDE.md).
// NDEBUG is supplied per-configuration by CMake to every target uniformly, so all TUs
// in a build agree on it.
//
// For the same reason the check below must NOT expand OLO_CORE_ASSERT, whose definition
// also turns on OLO_DEBUG. It calls an out-of-line reporting function instead, so the
// inline body is textually identical everywhere.
//
// Override by defining OLO_LOCK_DEBUG yourself -- uniformly, for the whole build.
#if !defined(OLO_LOCK_DEBUG)
#if defined(NDEBUG)
#define OLO_LOCK_DEBUG 0
#else
#define OLO_LOCK_DEBUG 1
#endif
#endif

namespace OloEngine::LockDebug
{
    enum class EMode : u8
    {
        Exclusive,
        Shared
    };

    // Per-thread capacity. Deep lock nesting is itself a smell; 64 is far above
    // anything in this engine and keeps the scan trivially short.
    inline constexpr u32 kMaxTrackedLocks = 64;

    namespace Private
    {
        struct FHeldLock
        {
            const void* Mutex = nullptr;
            EMode Mode = EMode::Exclusive;
        };

        // Trivially destructible on purpose: no thread-exit teardown ordering to get
        // wrong, which matters because these live as long as any thread that locks.
        inline thread_local FHeldLock s_Held[kMaxTrackedLocks]{};
        inline thread_local u32 s_HeldCount = 0;
        inline thread_local bool s_Overflowed = false;

        [[nodiscard]] inline i32 FindHeld(const void* mutex)
        {
            for (u32 i = 0; i < s_HeldCount; ++i)
            {
                if (s_Held[i].Mutex == mutex)
                    return static_cast<i32>(i);
            }
            return -1;
        }
    } // namespace Private

    // @brief Does the calling thread already hold this lock, in either mode?
    [[nodiscard]] inline bool IsHeldByCurrentThread(const void* mutex)
    {
        return Private::FindHeld(mutex) >= 0;
    }

    // @brief The mode this thread holds `mutex` in. Only meaningful when
    //        IsHeldByCurrentThread(mutex) is true.
    [[nodiscard]] inline EMode HeldMode(const void* mutex)
    {
        const i32 index = Private::FindHeld(mutex);
        return index >= 0 ? Private::s_Held[static_cast<u32>(index)].Mode : EMode::Exclusive;
    }

    // @brief Would acquiring `mutex` in `wanted` mode park this thread against itself?
    //
    // True for EVERY combination, because these locks are non-recursive in both modes:
    //   exclusive → exclusive   the classic self-deadlock
    //   exclusive → shared      the mixed-mode case FSharedMutex's header forbids
    //   shared    → exclusive   ditto, and the shape issues #439 / #863 both hit
    //   shared    → shared      parks whenever a writer is already queued, because
    //                           FSharedMutex gives waiting writers priority over new
    //                           readers — so it deadlocks intermittently, which is worse
    //                           than deadlocking always.
    //
    // This is the predicate behind the check, exposed so it can be unit-tested without
    // actually taking a lock twice (which would hang the test rather than fail it).
    [[nodiscard]] inline bool WouldSelfDeadlock(const void* mutex, EMode wanted)
    {
        (void)wanted;
        return IsHeldByCurrentThread(mutex);
    }

    // @brief Record that the calling thread acquired `mutex`. Silently stops tracking
    //        past kMaxTrackedLocks rather than evicting an entry (which would create a
    //        false negative that looks like a pass).
    inline void OnAcquired(const void* mutex, EMode mode)
    {
        if (Private::s_HeldCount >= kMaxTrackedLocks)
        {
            Private::s_Overflowed = true;
            return;
        }
        Private::s_Held[Private::s_HeldCount++] = { mutex, mode };
    }

    // @brief Record that the calling thread released `mutex`. A release of something not
    //        tracked (overflowed, or acquired on another thread) is ignored.
    inline void OnReleased(const void* mutex, EMode mode)
    {
        (void)mode;
        const i32 index = Private::FindHeld(mutex);
        if (index < 0)
            return;
        Private::s_Held[static_cast<u32>(index)] = Private::s_Held[--Private::s_HeldCount];
    }

    // @brief Test/diagnostic helper: how many locks this thread is tracking.
    [[nodiscard]] inline u32 HeldCountForCurrentThread()
    {
        return Private::s_HeldCount;
    }

    // @brief Log the offending lock and break into the debugger.
    //
    // Deliberately OUT OF LINE (defined in LockDebug.cpp): it needs OLO_DEBUGBREAK and
    // the logger, both gated on the engine-private OLO_DEBUG, and an inline body whose
    // contents depend on a per-target macro is an ODR violation. One definition, in one
    // TU, keeps every caller's inline body identical.
    //
    // NOT marked [[noreturn]]: with no debugger attached the process continues and then
    // genuinely deadlocks on the next line, which is the pre-existing behaviour. The
    // point is that it now says so first, instead of going silent forever.
    void ReportSelfDeadlock(const void* mutex, EMode wanted);

    // @brief Test helper: drop this thread's tracking state.
    inline void ResetCurrentThread()
    {
        Private::s_HeldCount = 0;
        Private::s_Overflowed = false;
    }
} // namespace OloEngine::LockDebug

#if OLO_LOCK_DEBUG

#define OLO_LOCK_DEBUG_CHECK_ACQUIRE(mutexPtr, mode)                                     \
    do                                                                                   \
    {                                                                                    \
        if (OLO_UNLIKELY(::OloEngine::LockDebug::WouldSelfDeadlock((mutexPtr), (mode)))) \
        {                                                                                \
            ::OloEngine::LockDebug::ReportSelfDeadlock((mutexPtr), (mode));              \
        }                                                                                \
    } while (0)

#define OLO_LOCK_DEBUG_ON_ACQUIRED(mutexPtr, mode) ::OloEngine::LockDebug::OnAcquired((mutexPtr), (mode))
#define OLO_LOCK_DEBUG_ON_RELEASED(mutexPtr, mode) ::OloEngine::LockDebug::OnReleased((mutexPtr), (mode))

#else

#define OLO_LOCK_DEBUG_CHECK_ACQUIRE(mutexPtr, mode) ((void)0)
#define OLO_LOCK_DEBUG_ON_ACQUIRED(mutexPtr, mode) ((void)0)
#define OLO_LOCK_DEBUG_ON_RELEASED(mutexPtr, mode) ((void)0)

#endif
