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
