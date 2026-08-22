#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// =============================================================================
// LockDebugTest — the same-thread re-entrancy detector behind FSharedMutex.
//
// What this guards: the detector that turns "the thread parks forever with no
// assert, no log line and no CPU" into an immediate assert naming the lock. That
// failure cost a live cdb session twice in one day (issues #439 and #863), and a
// detector that quietly stops detecting would restore the silence without any
// visible symptom — nothing else in the suite would notice.
//
// The tests drive LockDebug's bookkeeping DIRECTLY rather than through a real
// FSharedMutex, for one deliberate reason: exercising the real thing means
// actually taking a lock twice, which in a Debug build trips the assert (and in
// a Release build genuinely hangs). Asserting on the predicate keeps this a
// normal, fast, non-death test that runs in every configuration.
//
// Each case resets the thread's state first — the table is thread_local and
// gtest runs cases on one thread, so state leaks between cases otherwise.
// =============================================================================

#include "OloEngine/Threading/LockDebug.h"
#include "OloEngine/Threading/SharedMutex.h"
#include "OloEngine/Threading/SharedLock.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <thread>

using namespace OloEngine;
using LockDebug::EMode;

namespace
{
    class LockDebugTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            LockDebug::ResetCurrentThread();
        }
        void TearDown() override
        {
            LockDebug::ResetCurrentThread();
        }
    };
} // namespace

TEST_F(LockDebugTest, AnUnheldLockIsNotReportedAsHeld)
{
    int fakeMutex = 0;
    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&fakeMutex));
    EXPECT_FALSE(LockDebug::WouldSelfDeadlock(&fakeMutex, EMode::Exclusive));
    EXPECT_FALSE(LockDebug::WouldSelfDeadlock(&fakeMutex, EMode::Shared));
}

TEST_F(LockDebugTest, EveryReAcquisitionCombinationIsReportedAsASelfDeadlock)
{
    // All four are bugs: FSharedMutex is non-recursive in both modes, and its own
    // header forbids one thread holding an exclusive and a shared lock at once.
    // shared -> shared is included because waiting writers get priority over new
    // readers, so it parks intermittently — the worst of the four to debug.
    struct Combination
    {
        EMode Held;
        EMode Wanted;
        const char* What;
    };
    constexpr Combination combinations[] = {
        { EMode::Exclusive, EMode::Exclusive, "exclusive then exclusive" },
        { EMode::Exclusive, EMode::Shared, "exclusive then shared" },
        { EMode::Shared, EMode::Exclusive, "shared then exclusive (the #439 / #863 shape)" },
        { EMode::Shared, EMode::Shared, "shared then shared" },
    };

    for (const auto& combination : combinations)
    {
        LockDebug::ResetCurrentThread();
        int fakeMutex = 0;

        LockDebug::OnAcquired(&fakeMutex, combination.Held);
        EXPECT_TRUE(LockDebug::WouldSelfDeadlock(&fakeMutex, combination.Wanted))
            << "re-acquiring the same non-recursive lock (" << combination.What
            << ") was not flagged — the detector would let this park the thread silently, "
               "which is the entire failure it exists to prevent.";
    }
}

TEST_F(LockDebugTest, ReleasingClearsTheHoldSoNormalSequentialLockingIsNotFlagged)
{
    int fakeMutex = 0;

    LockDebug::OnAcquired(&fakeMutex, EMode::Exclusive);
    LockDebug::OnReleased(&fakeMutex, EMode::Exclusive);

    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&fakeMutex));
    EXPECT_FALSE(LockDebug::WouldSelfDeadlock(&fakeMutex, EMode::Exclusive))
        << "a lock/unlock/lock sequence was flagged as a self-deadlock — this would fire "
           "on ordinary correct code and the detector would have to be turned off.";
    EXPECT_EQ(LockDebug::HeldCountForCurrentThread(), 0u);
}

TEST_F(LockDebugTest, DistinctLocksDoNotAliasEachOther)
{
    int first = 0;
    int second = 0;

    LockDebug::OnAcquired(&first, EMode::Exclusive);

    EXPECT_TRUE(LockDebug::WouldSelfDeadlock(&first, EMode::Exclusive));
    EXPECT_FALSE(LockDebug::WouldSelfDeadlock(&second, EMode::Exclusive))
        << "holding one lock flagged an unrelated one — the table is keyed by address, "
           "so this would make every nested lock of two different mutexes assert.";
}

TEST_F(LockDebugTest, ReleasingOutOfOrderKeepsTheRemainingHoldsIntact)
{
    // The table swaps the last entry into the freed slot, so a release from the
    // middle must not lose or duplicate anything.
    int a = 0, b = 0, c = 0;
    LockDebug::OnAcquired(&a, EMode::Exclusive);
    LockDebug::OnAcquired(&b, EMode::Shared);
    LockDebug::OnAcquired(&c, EMode::Exclusive);

    LockDebug::OnReleased(&b, EMode::Shared);

    EXPECT_EQ(LockDebug::HeldCountForCurrentThread(), 2u);
    EXPECT_TRUE(LockDebug::IsHeldByCurrentThread(&a));
    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&b));
    EXPECT_TRUE(LockDebug::IsHeldByCurrentThread(&c))
        << "the swap-with-last removal dropped the last entry — deep nesting would stop "
           "being tracked, silently.";
}

TEST_F(LockDebugTest, TrackingIsPerThreadSoTwoThreadsHoldingDifferentLocksDoNotSeeEachOther)
{
    // The whole point of shared/exclusive locking is that other threads hold them
    // concurrently. If the table were global, every real concurrent acquisition
    // would assert.
    int mine = 0;
    LockDebug::OnAcquired(&mine, EMode::Exclusive);

    bool otherThreadSawMyHold = true;
    std::thread other([&]
                      { otherThreadSawMyHold = LockDebug::IsHeldByCurrentThread(&mine); });
    other.join();

    EXPECT_FALSE(otherThreadSawMyHold)
        << "one thread's holds are visible to another — the detector would fire on "
           "ordinary concurrent access, which is what these locks are FOR.";
}

TEST_F(LockDebugTest, OverflowStopsTrackingRatherThanEvictingAnEntry)
{
    // Degrading to "not detected" is the safe direction. Evicting an entry to make
    // room would make a genuinely-held lock report as free — a false negative that
    // looks exactly like a pass.
    std::vector<int> mutexes(LockDebug::kMaxTrackedLocks + 8, 0);
    for (auto& m : mutexes)
        LockDebug::OnAcquired(&m, EMode::Exclusive);

    EXPECT_EQ(LockDebug::HeldCountForCurrentThread(), LockDebug::kMaxTrackedLocks);
    EXPECT_TRUE(LockDebug::IsHeldByCurrentThread(&mutexes[0]))
        << "the first lock was evicted to make room — holds must never be forgotten "
           "while still held.";
}

TEST_F(LockDebugTest, RealFSharedMutexAcquisitionsAreTrackedThroughTheScopedGuards)
{
    // The bookkeeping above is only useful if the mutex actually calls it. This is
    // the wiring check — and it is skipped when the detector is compiled out,
    // rather than asserting something that is deliberately absent.
#if OLO_LOCK_DEBUG
    FSharedMutex mutex;

    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&mutex));
    {
        TUniqueLock<FSharedMutex> lock(mutex);
        EXPECT_TRUE(LockDebug::IsHeldByCurrentThread(&mutex))
            << "FSharedMutex::Lock did not register the hold — the detector is wired up "
               "wrong and would never fire on a real self-deadlock.";
        EXPECT_EQ(LockDebug::HeldMode(&mutex), EMode::Exclusive);
    }
    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&mutex))
        << "FSharedMutex::Unlock did not clear the hold — the next acquisition of this "
           "mutex would assert spuriously.";

    {
        TSharedLock<FSharedMutex> lock(mutex);
        EXPECT_TRUE(LockDebug::IsHeldByCurrentThread(&mutex));
        EXPECT_EQ(LockDebug::HeldMode(&mutex), EMode::Shared);
    }
    EXPECT_FALSE(LockDebug::IsHeldByCurrentThread(&mutex));
#else
    GTEST_SKIP() << "OLO_LOCK_DEBUG is off in this configuration; the mutex does not "
                    "call into LockDebug, so there is no wiring to verify.";
#endif
}
