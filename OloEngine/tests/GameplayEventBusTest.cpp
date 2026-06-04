#include "OloEnginePCH.h"

// =============================================================================
// GameplayEventBusTest — unit test for the GameplayEventBus dispatch mechanism.
//
// The bus is a synchronous, type-keyed pub/sub used by the quest/inventory
// systems to publish their notification payloads. These tests pin its core
// contract independently of any Scene: per-type routing, multiple handlers in
// subscription order, no-op when nothing subscribes, and Clear().
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Gameplay/GameplayEventBus.h"

#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    struct AlphaEvent
    {
        int Value = 0;
    };
    struct BetaEvent
    {
        std::string Name;
    };
} // namespace

TEST(GameplayEventBusTest, PublishReachesSubscriberWithPayload)
{
    GameplayEventBus bus;
    std::vector<int> seen;
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent& e)
                              { seen.push_back(e.Value); });

    bus.Publish(AlphaEvent{ 42 });
    bus.Publish(AlphaEvent{ 7 });

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], 42);
    EXPECT_EQ(seen[1], 7);
}

TEST(GameplayEventBusTest, EventsAreRoutedByType)
{
    GameplayEventBus bus;
    int alphaHits = 0;
    int betaHits = 0;
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent&)
                              { ++alphaHits; });
    bus.Subscribe<BetaEvent>([&](const BetaEvent&)
                             { ++betaHits; });

    bus.Publish(AlphaEvent{ 1 });
    EXPECT_EQ(alphaHits, 1);
    EXPECT_EQ(betaHits, 0) << "an AlphaEvent must not reach a BetaEvent handler.";

    bus.Publish(BetaEvent{ "x" });
    EXPECT_EQ(alphaHits, 1);
    EXPECT_EQ(betaHits, 1);
}

TEST(GameplayEventBusTest, MultipleHandlersFireInSubscriptionOrder)
{
    GameplayEventBus bus;
    std::vector<int> order;
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent&)
                              { order.push_back(1); });
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent&)
                              { order.push_back(2); });
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent&)
                              { order.push_back(3); });

    bus.Publish(AlphaEvent{ 0 });

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
    EXPECT_EQ(bus.HandlerCount<AlphaEvent>(), 3u);
    EXPECT_EQ(bus.HandlerCount<BetaEvent>(), 0u);
}

TEST(GameplayEventBusTest, PublishWithNoSubscribersIsNoOp)
{
    GameplayEventBus bus;
    EXPECT_NO_THROW(bus.Publish(AlphaEvent{ 99 }));
    EXPECT_EQ(bus.HandlerCount<AlphaEvent>(), 0u);
}

TEST(GameplayEventBusTest, ClearDropsAllSubscriptions)
{
    GameplayEventBus bus;
    int hits = 0;
    bus.Subscribe<AlphaEvent>([&](const AlphaEvent&)
                              { ++hits; });
    bus.Publish(AlphaEvent{ 0 });
    ASSERT_EQ(hits, 1);

    bus.Clear();
    bus.Publish(AlphaEvent{ 0 });
    EXPECT_EQ(hits, 1) << "handlers should not fire after Clear().";
    EXPECT_EQ(bus.HandlerCount<AlphaEvent>(), 0u);
}
