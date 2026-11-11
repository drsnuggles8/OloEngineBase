// OLO_TEST_LAYER: unit
//
// FHazardPointerCollection defers deleting a retired object until no hazard
// pointer protects it. Each thread keeps its retired objects in its own list,
// collected at most once a second; FAAArrayQueue retires its 8 KB nodes this
// way, and the scheduler's worker threads are the ones that retire them.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Containers/HazardPointer.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <utility>

namespace
{
    using namespace OloEngine;

    struct FTracked
    {
        std::atomic<i32>* Destroyed;

        ~FTracked()
        {
            Destroyed->fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct FRetiresTwoWhenDestroyed
    {
        FHazardPointerCollection* Collection = nullptr;
        std::atomic<i32>* Destroyed = nullptr;

        ~FRetiresTwoWhenDestroyed()
        {
            Collection->Delete(new FTracked{ Destroyed });
            Collection->Delete(new FTracked{ Destroyed });
        }
    };
} // namespace

// A thread's first retirement is collected at once; the next one waits for a
// collection up to a second later, which a thread that exits never runs. The
// scheduler restarts its workers, so every exited worker would keep its last
// retired queue nodes for the life of the process.
TEST(HazardPointer, AnExitedThreadsRetiredObjectsAreReclaimed)
{
    std::atomic<i32> destroyed{ 0 };
    FHazardPointerCollection collection;

    std::thread retiring(
        [&collection, &destroyed]
        {
            collection.Delete(new FTracked{ &destroyed });
            collection.Delete(new FTracked{ &destroyed });
        });
    retiring.join();

    EXPECT_EQ(destroyed.load(), 2) << "the exited thread's unprotected retired object is still waiting for a collection";
}

// A moved-to hazard pointer keeps the slot it took over. Released on the move,
// the slot goes to the next hazard pointer acquired before the moved-to one's
// first Get; the two then share it, the later Get overwrites the protection,
// and the moved-to pointer's object is deleted under it.
TEST(HazardPointer, AMovedHazardPointerStillProtectsItsObject)
{
    std::atomic<i32> destroyed{ 0 };
    std::atomic<i32> otherDestroyed{ 0 };
    FHazardPointerCollection collection;

    FTracked* const guarded = new FTracked{ &destroyed };
    std::atomic<FTracked*> guardedSlot{ guarded };
    FTracked* const other = new FTracked{ &otherDestroyed };
    std::atomic<FTracked*> otherSlot{ other };
    {
        THazardPointer<FTracked> original(guardedSlot, collection);
        THazardPointer<FTracked> moved(std::move(original));
        THazardPointer<FTracked> second(otherSlot, collection);

        ASSERT_EQ(moved.Get(), guarded);
        ASSERT_EQ(second.Get(), other);

        guardedSlot.store(nullptr);
        collection.Delete(guarded, 1); // collect now
        EXPECT_EQ(destroyed.load(), 0) << "an object a moved hazard pointer protects was deleted";
    }
    delete other;
}

// A thread remembers each collection it retired through, to hand its list back
// when it exits. A collection destroyed first has nothing to take back, so the
// thread forgets it the next time it meets a new collection; a long-lived
// thread otherwise keeps a record of every collection it ever used.
TEST(HazardPointer, AThreadForgetsTheCollectionsDestroyedBeforeIt)
{
    constexpr i32 kCollections = 100;
    std::atomic<i32> destroyed{ 0 };
    i32 remembered = -1;

    std::thread retiring(
        [&destroyed, &remembered]
        {
            for (i32 i = 0; i < kCollections; ++i)
            {
                FHazardPointerCollection collection;
                collection.Delete(new FTracked{ &destroyed });
            }
            remembered = FHazardPointerCollection::NumCollectionsThisThreadHandsBackTo();
        });
    retiring.join();

    EXPECT_EQ(destroyed.load(), kCollections);
    // The last one is forgotten when the thread meets the next collection.
    EXPECT_EQ(remembered, 1) << "the thread still remembers collections that no longer exist";
}

// An object a hazard pointer still protects when its retiring thread exits
// cannot be deleted at the exit, and has no thread left to delete it later. It
// stays with the collection, and the next collection on any thread deletes it
// once the protection has ended.
TEST(HazardPointer, AnObjectProtectedWhenItsRetiringThreadExitsIsDeletedAfterTheProtectionEnds)
{
    std::atomic<i32> destroyed{ 0 };
    std::atomic<i32> otherDestroyed{ 0 };
    FHazardPointerCollection collection;

    FTracked* const guarded = new FTracked{ &destroyed };
    std::atomic<FTracked*> slot{ guarded };
    {
        THazardPointer<FTracked> hazard(slot, collection);
        ASSERT_EQ(hazard.Get(), guarded);

        std::thread retiring(
            [&collection, &slot, guarded]
            {
                slot.store(nullptr);
                collection.Delete(guarded);
            });
        retiring.join();

        EXPECT_EQ(destroyed.load(), 0) << "an object a hazard pointer protects was deleted when its retiring thread exited";
    }

    collection.Delete(new FTracked{ &otherDestroyed }, 1); // collect now
    EXPECT_EQ(destroyed.load(), 1) << "the exited thread's object was not deleted after its protection ended";
}

// thread_local destructors run in reverse order of construction, so one that
// was constructed before the thread first retired anything runs after the
// thread's list was handed back. What it retires then must not start a new
// list: nothing is left to hand that one back, and only its first object would
// be collected before the collection itself is destroyed.
TEST(HazardPointer, ObjectsRetiredAfterTheThreadsListWasHandedBackAreReclaimed)
{
    std::atomic<i32> destroyed{ 0 };
    FHazardPointerCollection collection;

    std::thread retiring(
        [&collection, &destroyed]
        {
            thread_local FRetiresTwoWhenDestroyed retiresTwoWhenDestroyed;
            retiresTwoWhenDestroyed.Collection = &collection;
            retiresTwoWhenDestroyed.Destroyed = &destroyed;
            collection.Delete(new FTracked{ &destroyed });
        });
    retiring.join();

    EXPECT_EQ(destroyed.load(), 3) << "an object retired during thread teardown is still waiting for a collection";
}
