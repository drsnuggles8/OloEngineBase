// OLO_TEST_LAYER: unit
// =============================================================================
// RefTest.cpp
//
// Regression coverage for issue #596 ("Ref<T>: fix DecRef() dangling-pointer
// window / ownership semantics"). The audit found two real correctness gaps
// in OloEngine/Core/Ref.h beyond the literal dangling-m_Instance complaint the
// issue opened with:
//
//   1. RefCounted::DecRefCount() decremented the atomic refcount and callers
//      separately re-read GetRefCount() to decide whether it hit zero. That
//      is a TOCTOU race: two threads racing the last two releases of the same
//      object could both observe 0 and both call delete (a double free).
//
//   2. WeakRef<T>::Lock() checked IsLive() and then unconditionally
//      incremented the refcount in a *separate* step. A concurrent DecRef()
//      releasing the last strong reference in between could still delete the
//      object even though Lock() had just "resurrected" it -- a real,
//      reachable race (Font::Create()'s font cache does exactly this
//      check-then-lock pattern across threads, see Renderer/Font.cpp).
//
// Both are fixed by making RefCounted::DecRefCount() return the atomically
// -obtained post-decrement count, and by performing the decrement itself
// *inside* the same critical section as the "did we just hit zero" decision
// (RefUtils::Release) and WeakRef's resurrection attempt
// (RefUtils::TryLockLive) -- decrementing lock-free and only taking the lock
// once a thread already believes it hit zero is NOT sufficient (it was the
// first fix attempted here, and it reproduced a real double-free within
// seconds): a concurrent TryLockLive() can fully resurrect-and-re-release
// the object in the gap before the original decrementer ever reaches the
// lock, leaving it holding a dangling pointer. See Core/Ref.h/.cpp for the
// implementation and docs/agent-rules/ for the full write-up of the audit.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Ref.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using namespace OloEngine;

namespace
{
    class RefTestObject : public RefCounted
    {
      public:
        explicit RefTestObject(std::atomic<int>* destructCounter = nullptr) : m_DestructCounter(destructCounter) {}

        ~RefTestObject() override
        {
            if (m_DestructCounter)
            {
                m_DestructCounter->fetch_add(1, std::memory_order_relaxed);
            }
        }

        int Value = 42;

      private:
        std::atomic<int>* m_DestructCounter;
    };
} // namespace

TEST(RefTest, WeakRefIsInvalidAfterLastStrongRefDropped)
{
    WeakRef<RefTestObject> weak;
    {
        Ref<RefTestObject> strong = Ref<RefTestObject>::Create();
        weak = strong;
        EXPECT_TRUE(weak.IsValid());
    }
    EXPECT_FALSE(weak.IsValid());
}

TEST(RefTest, WeakRefLockReturnsValidRefWhileStrongRefAlive)
{
    Ref<RefTestObject> strong = Ref<RefTestObject>::Create();
    WeakRef<RefTestObject> weak = strong;

    Ref<RefTestObject> locked = weak.Lock();
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->Value, 42);
    // Locking must take its own reference: refcount should now be 2 (strong + locked).
    EXPECT_EQ(strong->GetRefCount(), 2u);
}

TEST(RefTest, WeakRefLockReturnsNullAfterDestruction)
{
    WeakRef<RefTestObject> weak;
    {
        Ref<RefTestObject> strong = Ref<RefTestObject>::Create();
        weak = strong;
    }

    Ref<RefTestObject> locked = weak.Lock();
    EXPECT_FALSE(locked);
}

// Regression test for the RefCounted::DecRefCount()/GetRefCount() TOCTOU race
// (gap #1 above). Many threads each drop their own copy of a shared Ref at
// roughly the same time across many independently-allocated objects, so the
// last-two-releases race window recurs thousands of times. Before the fix
// this reliably produced a destructor-call count > round-count (a double
// free) under ASan, or crashed outright.
TEST(RefTest, DestructorRunsExactlyOnceUnderConcurrentRelease)
{
    constexpr int kThreads = 4;
    constexpr int kRounds = 20000;

    std::atomic<int> destructCount{ 0 };

    // Pre-create every round's shared object plus one Ref copy per worker so
    // the only cross-thread work inside the join'd loop below is the racy
    // release itself.
    std::vector<std::array<Ref<RefTestObject>, kThreads>> rounds;
    rounds.reserve(kRounds);
    for (int r = 0; r < kRounds; ++r)
    {
        Ref<RefTestObject> object = Ref<RefTestObject>::Create(&destructCount);
        std::array<Ref<RefTestObject>, kThreads> copies;
        for (auto& copy : copies)
        {
            copy = object;
        }
        rounds.push_back(std::move(copies));
    }

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&rounds, t]()
            {
                for (auto& round : rounds)
                {
                    round[static_cast<sizet>(t)] = nullptr;
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(destructCount.load(), kRounds);
}

// Regression test for the WeakRef<T>::Lock() resurrection race (gap #2
// above). One thread races to drop the last strong Ref to each of many
// objects while another thread concurrently calls Lock() on the matching
// WeakRef. Before the fix, Lock() could return a Ref to an object that was
// concurrently (or already) deleted by the releasing thread -- an immediate
// use-after-free the moment `locked->Value` is read, which ASan catches
// directly; this also cross-checks the read value as a plain-Debug signal.
TEST(RefTest, WeakRefLockDuringConcurrentReleaseNeverObservesDestroyedObject)
{
    constexpr int kRounds = 20000;

    std::atomic<int> destructCount{ 0 };
    std::vector<Ref<RefTestObject>> strongRefs;
    std::vector<WeakRef<RefTestObject>> weakRefs;
    strongRefs.reserve(kRounds);
    weakRefs.reserve(kRounds);
    for (int r = 0; r < kRounds; ++r)
    {
        strongRefs.push_back(Ref<RefTestObject>::Create(&destructCount));
        weakRefs.push_back(strongRefs.back());
    }

    std::thread releaser(
        [&strongRefs]()
        {
            for (auto& ref : strongRefs)
            {
                ref = nullptr;
            }
        });

    std::thread locker(
        [&weakRefs]()
        {
            for (auto& weak : weakRefs)
            {
                if (Ref<RefTestObject> locked = weak.Lock())
                {
                    EXPECT_EQ(locked->Value, 42);
                }
            }
        });

    releaser.join();
    locker.join();

    EXPECT_EQ(destructCount.load(), kRounds);
}

// =============================================================================
// The live registry must name one object, not one address
// =============================================================================

namespace
{
    // Every instance lives in the same static storage, so a new one is always
    // allocated at the address the last one had.
    class ReusedAddressObject final : public RefCounted
    {
      public:
        static void* operator new(std::size_t size)
        {
            EXPECT_LE(size, sizeof(s_Storage));
            return s_Storage;
        }
        static void operator delete(void* /*storage*/) noexcept {}

        int Value = 0;

      private:
        alignas(std::max_align_t) static inline unsigned char s_Storage[64];
    };

    struct OtherBase
    {
        virtual ~OtherBase() = default;
        int Tag = 7;
    };

    // RefCounted is the second base, so it is not at the object's address.
    class SecondBaseObject final : public OtherBase, public RefCounted
    {
      public:
        static void* operator new(std::size_t size)
        {
            EXPECT_LE(size, sizeof(s_Storage));
            return s_Storage;
        }
        static void operator delete(void* /*storage*/) noexcept {}

      private:
        alignas(std::max_align_t) static inline unsigned char s_Storage[64];
    };
} // namespace

// A WeakRef that outlives its object must not resolve to a later object that
// was allocated at the same address. The registry used to hold addresses only,
// so Lock() returned the new object: Font::Create's cache, which keeps a
// WeakRef per path, could hand back an unrelated object as a font.
TEST(RefTest, WeakRefDoesNotResolveToALaterObjectAtTheSameAddress)
{
    WeakRef<ReusedAddressObject> weak;
    const ReusedAddressObject* firstAddress = nullptr;
    {
        Ref<ReusedAddressObject> first = Ref<ReusedAddressObject>::Create();
        first->Value = 1;
        firstAddress = first.get();
        weak = first;
        EXPECT_TRUE(weak.IsValid());
    }
    EXPECT_FALSE(weak.IsValid());

    Ref<ReusedAddressObject> second = Ref<ReusedAddressObject>::Create();
    second->Value = 2;
    ASSERT_EQ(second.get(), firstAddress) << "the test needs both objects at one address";

    EXPECT_FALSE(weak.IsValid()) << "a WeakRef to the first object reports the second as its own";
    EXPECT_FALSE(weak.Lock()) << "a WeakRef to the first object locked the second";
    EXPECT_EQ(second->GetRefCount(), 1u) << "the stale WeakRef took a reference on the second object";

    WeakRef<ReusedAddressObject> weakToSecond = second;
    EXPECT_TRUE(weakToSecond.IsValid());
    EXPECT_TRUE(weakToSecond.Lock());
}

// The registry inserted an object under its own address and erased it under its
// RefCounted base's address. For a class whose RefCounted base is not first the
// two differ, so the object was never erased and its address stayed live after
// it died (every SoundGraph did this).
TEST(RefTest, AnObjectWhoseRefCountedBaseIsNotFirstLeavesTheRegistryWhenItDies)
{
    SecondBaseObject* raw = nullptr;
    WeakRef<SecondBaseObject> weak;
    {
        Ref<SecondBaseObject> strong = Ref<SecondBaseObject>::Create();
        raw = strong.get();
        ASSERT_NE(static_cast<const void*>(raw), static_cast<const void*>(static_cast<const RefCounted*>(raw)))
            << "the test needs RefCounted away from the object's address";
        weak = strong;
        EXPECT_TRUE(weak.IsValid());
        EXPECT_TRUE(WeakRef<SecondBaseObject>(raw).IsValid());
    }
    EXPECT_FALSE(weak.IsValid());
    EXPECT_FALSE(weak.Lock());
    // The storage is static, so asking about the dead object's address is safe.
    EXPECT_FALSE(WeakRef<SecondBaseObject>(raw).IsValid()) << "a dead object's address is still registered as live";
}
