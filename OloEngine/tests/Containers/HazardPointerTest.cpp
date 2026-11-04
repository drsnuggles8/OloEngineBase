// OLO_TEST_LAYER: unit
//
// FHazardPointerCollection defers deleting a retired object until no hazard
// pointer protects it.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Containers/HazardPointer.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
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
