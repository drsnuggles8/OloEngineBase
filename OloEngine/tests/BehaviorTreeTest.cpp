#include <gtest/gtest.h>
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/BehaviorTree/BTComposites.h"
#include "OloEngine/AI/BehaviorTree/BTDecorators.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTreeAsset.h"
#include "OloEngine/AI/AIRegistry.h"

using namespace OloEngine;

// ============================================================================
// BTBlackboard tests
// ============================================================================

class BTBlackboardTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
};

TEST_F(BTBlackboardTest, SetAndGetBool)
{
    bb.Set("flag", true);
    EXPECT_TRUE(bb.Get<bool>("flag"));
    bb.Set("flag", false);
    EXPECT_FALSE(bb.Get<bool>("flag"));
}

TEST_F(BTBlackboardTest, SetAndGetInt)
{
    bb.Set("count", i32(42));
    EXPECT_EQ(bb.Get<i32>("count"), 42);
}

TEST_F(BTBlackboardTest, SetAndGetFloat)
{
    bb.Set("speed", f32(3.14f));
    EXPECT_FLOAT_EQ(bb.Get<f32>("speed"), 3.14f);
}

TEST_F(BTBlackboardTest, SetAndGetString)
{
    bb.Set("name", std::string("Enemy"));
    EXPECT_EQ(bb.Get<std::string>("name"), "Enemy");
}

TEST_F(BTBlackboardTest, SetAndGetVec3)
{
    glm::vec3 pos(1.0f, 2.0f, 3.0f);
    bb.Set("position", pos);
    auto result = bb.Get<glm::vec3>("position");
    EXPECT_FLOAT_EQ(result.x, 1.0f);
    EXPECT_FLOAT_EQ(result.y, 2.0f);
    EXPECT_FLOAT_EQ(result.z, 3.0f);
}

TEST_F(BTBlackboardTest, HasAndRemove)
{
    EXPECT_FALSE(bb.Has("key"));
    bb.Set("key", true);
    EXPECT_TRUE(bb.Has("key"));
    bb.Remove("key");
    EXPECT_FALSE(bb.Has("key"));
}

TEST_F(BTBlackboardTest, Clear)
{
    bb.Set("a", true);
    bb.Set("b", i32(1));
    bb.Clear();
    EXPECT_FALSE(bb.Has("a"));
    EXPECT_FALSE(bb.Has("b"));
}

TEST_F(BTBlackboardTest, GetAllReturnsAllKeys)
{
    bb.Set("x", true);
    bb.Set("y", i32(5));
    auto all = bb.GetAll();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_TRUE(all.contains("x"));
    EXPECT_TRUE(all.contains("y"));
}

// ============================================================================
// Helper: mock task nodes for composite testing
// ============================================================================

class MockSuccessNode : public BTNode
{
  public:
    BTStatus Tick(f32 /*dt*/, BTBlackboard& /*bb*/, Entity /*entity*/) override
    {
        return BTStatus::Success;
    }
};

class MockFailureNode : public BTNode
{
  public:
    BTStatus Tick(f32 /*dt*/, BTBlackboard& /*bb*/, Entity /*entity*/) override
    {
        return BTStatus::Failure;
    }
};

class MockRunningNode : public BTNode
{
  public:
    BTStatus Tick(f32 /*dt*/, BTBlackboard& /*bb*/, Entity /*entity*/) override
    {
        return BTStatus::Running;
    }
};

class MockCounterNode : public BTNode
{
  public:
    int TickCount = 0;
    int TotalTickCount = 0;

    BTStatus Tick(f32 /*dt*/, BTBlackboard& /*bb*/, Entity /*entity*/) override
    {
        ++TickCount;
        ++TotalTickCount;
        return BTStatus::Success;
    }

    void Reset() override
    {
        TickCount = 0;
        BTNode::Reset();
    }
};

// ============================================================================
// BTSequence tests
// ============================================================================

class BTSequenceTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BTSequenceTest, AllSucceed_ReturnsSuccess)
{
    auto seq = Ref<BTSequence>::Create();
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(seq->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTSequenceTest, OneFailure_ReturnsFailure)
{
    auto seq = Ref<BTSequence>::Create();
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    seq->Children.push_back(Ref<MockFailureNode>::Create());
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(seq->Tick(0.016f, bb, entity), BTStatus::Failure);
}

TEST_F(BTSequenceTest, Running_PausesAndResumes)
{
    auto seq = Ref<BTSequence>::Create();
    auto counter = Ref<MockCounterNode>::Create();
    seq->Children.push_back(Ref<MockSuccessNode>::Create());
    seq->Children.push_back(Ref<MockRunningNode>::Create());
    seq->Children.push_back(counter);
    EXPECT_EQ(seq->Tick(0.016f, bb, entity), BTStatus::Running);
    EXPECT_EQ(counter->TickCount, 0);
}

TEST_F(BTSequenceTest, EmptyChildren_ReturnsSuccess)
{
    auto seq = Ref<BTSequence>::Create();
    EXPECT_EQ(seq->Tick(0.016f, bb, entity), BTStatus::Success);
}

// ============================================================================
// BTSelector tests
// ============================================================================

class BTSelectorTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BTSelectorTest, FirstSuccess_ReturnsSuccess)
{
    auto sel = Ref<BTSelector>::Create();
    sel->Children.push_back(Ref<MockFailureNode>::Create());
    sel->Children.push_back(Ref<MockSuccessNode>::Create());
    sel->Children.push_back(Ref<MockFailureNode>::Create());
    EXPECT_EQ(sel->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTSelectorTest, AllFail_ReturnsFailure)
{
    auto sel = Ref<BTSelector>::Create();
    sel->Children.push_back(Ref<MockFailureNode>::Create());
    sel->Children.push_back(Ref<MockFailureNode>::Create());
    EXPECT_EQ(sel->Tick(0.016f, bb, entity), BTStatus::Failure);
}

TEST_F(BTSelectorTest, Running_PausesAtRunningChild)
{
    auto sel = Ref<BTSelector>::Create();
    sel->Children.push_back(Ref<MockFailureNode>::Create());
    sel->Children.push_back(Ref<MockRunningNode>::Create());
    sel->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(sel->Tick(0.016f, bb, entity), BTStatus::Running);
}

TEST_F(BTSelectorTest, EmptyChildren_ReturnsFailure)
{
    auto sel = Ref<BTSelector>::Create();
    EXPECT_EQ(sel->Tick(0.016f, bb, entity), BTStatus::Failure);
}

// ============================================================================
// BTParallel tests
// ============================================================================

class BTParallelTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BTParallelTest, RequireAll_AllSucceed_ReturnsSuccess)
{
    auto par = Ref<BTParallel>::Create();
    par->SuccessPolicy = BTParallel::Policy::RequireAll;
    par->Children.push_back(Ref<MockSuccessNode>::Create());
    par->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(par->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTParallelTest, RequireOne_OneSucceeds_ReturnsSuccess)
{
    auto par = Ref<BTParallel>::Create();
    par->SuccessPolicy = BTParallel::Policy::RequireOne;
    par->FailurePolicy = BTParallel::Policy::RequireAll;
    par->Children.push_back(Ref<MockFailureNode>::Create());
    par->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(par->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTParallelTest, RequireAll_OneRunning_ReturnsRunning)
{
    auto par = Ref<BTParallel>::Create();
    par->SuccessPolicy = BTParallel::Policy::RequireAll;
    par->Children.push_back(Ref<MockSuccessNode>::Create());
    par->Children.push_back(Ref<MockRunningNode>::Create());
    EXPECT_EQ(par->Tick(0.016f, bb, entity), BTStatus::Running);
}

// ============================================================================
// BTDecorator tests
// ============================================================================

class BTDecoratorTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BTDecoratorTest, Inverter_FlipsSuccess)
{
    auto inv = Ref<BTInverter>::Create();
    inv->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(inv->Tick(0.016f, bb, entity), BTStatus::Failure);
}

TEST_F(BTDecoratorTest, Inverter_FlipsFailure)
{
    auto inv = Ref<BTInverter>::Create();
    inv->Children.push_back(Ref<MockFailureNode>::Create());
    EXPECT_EQ(inv->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTDecoratorTest, Inverter_PassesThroughRunning)
{
    auto inv = Ref<BTInverter>::Create();
    inv->Children.push_back(Ref<MockRunningNode>::Create());
    EXPECT_EQ(inv->Tick(0.016f, bb, entity), BTStatus::Running);
}

TEST_F(BTDecoratorTest, Repeater_FixedCount)
{
    auto rep = Ref<BTRepeater>::Create();
    rep->RepeatCount = 3;
    auto counter = Ref<MockCounterNode>::Create();
    rep->Children.push_back(counter);

    // First two ticks return Running (repeating)
    EXPECT_EQ(rep->Tick(0.016f, bb, entity), BTStatus::Running);
    EXPECT_EQ(counter->TotalTickCount, 1);
    EXPECT_EQ(rep->Tick(0.016f, bb, entity), BTStatus::Running);
    EXPECT_EQ(counter->TotalTickCount, 2);
    // Third tick completes
    EXPECT_EQ(rep->Tick(0.016f, bb, entity), BTStatus::Success);
    EXPECT_EQ(counter->TotalTickCount, 3);
}

TEST_F(BTDecoratorTest, ConditionalGuard_AllowsWhenMatch)
{
    bb.Set("ready", true);
    auto guard = Ref<BTConditionalGuard>::Create();
    guard->BlackboardKey = "ready";
    guard->ExpectedValue = BTBlackboard::Value(true);
    guard->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(guard->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTDecoratorTest, ConditionalGuard_BlocksWhenMismatch)
{
    bb.Set("ready", false);
    auto guard = Ref<BTConditionalGuard>::Create();
    guard->BlackboardKey = "ready";
    guard->ExpectedValue = BTBlackboard::Value(true);
    guard->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(guard->Tick(0.016f, bb, entity), BTStatus::Failure);
}

TEST_F(BTDecoratorTest, ConditionalGuard_BlocksWhenKeyMissing)
{
    auto guard = Ref<BTConditionalGuard>::Create();
    guard->BlackboardKey = "nonexistent";
    guard->ExpectedValue = BTBlackboard::Value(true);
    guard->Children.push_back(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(guard->Tick(0.016f, bb, entity), BTStatus::Failure);
}

// ============================================================================
// BTTask tests
// ============================================================================

class BTTaskTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BTTaskTest, Wait_ReturnsRunningThenSuccess)
{
    auto wait = Ref<BTWait>::Create();
    wait->Duration = 0.05f;

    // First tick starts timer, returns Running
    EXPECT_EQ(wait->Tick(0.03f, bb, entity), BTStatus::Running);
    // Second tick completes
    EXPECT_EQ(wait->Tick(0.03f, bb, entity), BTStatus::Success);
}

TEST_F(BTTaskTest, SetBlackboardValue_SetsKey)
{
    auto setter = Ref<BTSetBlackboardValue>::Create();
    setter->Key = "health";
    setter->ValueToSet = BTBlackboard::Value(i32(100));
    EXPECT_EQ(setter->Tick(0.016f, bb, entity), BTStatus::Success);
    EXPECT_EQ(bb.Get<i32>("health"), 100);
}

TEST_F(BTTaskTest, CheckBlackboardKey_SucceedsWhenMatch)
{
    bb.Set("status", std::string("idle"));
    auto checker = Ref<BTCheckBlackboardKey>::Create();
    checker->Key = "status";
    checker->ExpectedValue = BTBlackboard::Value(std::string("idle"));
    EXPECT_EQ(checker->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BTTaskTest, CheckBlackboardKey_FailsWhenMismatch)
{
    bb.Set("status", std::string("combat"));
    auto checker = Ref<BTCheckBlackboardKey>::Create();
    checker->Key = "status";
    checker->ExpectedValue = BTBlackboard::Value(std::string("idle"));
    EXPECT_EQ(checker->Tick(0.016f, bb, entity), BTStatus::Failure);
}

// ============================================================================
// BehaviorTree container tests
// ============================================================================

class BehaviorTreeContainerTest : public ::testing::Test
{
  protected:
    BTBlackboard bb;
    Entity entity;
};

TEST_F(BehaviorTreeContainerTest, TickWithRoot)
{
    auto tree = Ref<BehaviorTree>::Create();
    tree->SetRoot(Ref<MockSuccessNode>::Create());
    EXPECT_EQ(tree->Tick(0.016f, bb, entity), BTStatus::Success);
}

TEST_F(BehaviorTreeContainerTest, TickWithNoRoot_ReturnsFailure)
{
    auto tree = Ref<BehaviorTree>::Create();
    EXPECT_EQ(tree->Tick(0.016f, bb, entity), BTStatus::Failure);
}

TEST_F(BehaviorTreeContainerTest, Reset_ResetsRoot)
{
    auto tree = Ref<BehaviorTree>::Create();
    auto counter = Ref<MockCounterNode>::Create();
    tree->SetRoot(counter);
    tree->Tick(0.016f, bb, entity);
    EXPECT_EQ(counter->TickCount, 1);
    tree->Reset();
    EXPECT_EQ(counter->TickCount, 0);
}

// ============================================================================
// BehaviorTreeAsset tests
// ============================================================================

TEST(BehaviorTreeAssetTest, HasCorrectAssetType)
{
    auto asset = Ref<BehaviorTreeAsset>::Create();
    EXPECT_EQ(asset->GetAssetType(), AssetType::BehaviorTree);
    EXPECT_EQ(BehaviorTreeAsset::GetStaticType(), AssetType::BehaviorTree);
}

TEST(BehaviorTreeAssetTest, StoresNodesAndRoot)
{
    auto asset = Ref<BehaviorTreeAsset>::Create();
    BTNodeData node;
    node.ID = UUID(1);
    node.TypeName = "BTSequence";
    node.Name = "Root";
    asset->AddNode(std::move(node));
    asset->SetRootNodeID(UUID(1));
    EXPECT_EQ(asset->GetNodes().size(), 1u);
    EXPECT_EQ(asset->GetRootNodeID(), UUID(1));
}

// ============================================================================
// AIRegistry tests
// ============================================================================

TEST(AIRegistryTest, RegisterBuiltInTypes)
{
    RegisterBuiltInAITypes();

    // Verify a few known node types are registered
    auto seq = BTNodeRegistry::Create("Sequence");
    EXPECT_NE(seq, nullptr);
    auto sel = BTNodeRegistry::Create("Selector");
    EXPECT_NE(sel, nullptr);
    auto wait = BTNodeRegistry::Create("Wait");
    EXPECT_NE(wait, nullptr);
    auto inverter = BTNodeRegistry::Create("Inverter");
    EXPECT_NE(inverter, nullptr);

    // Unknown type returns nullptr
    auto unknown = BTNodeRegistry::Create("NonExistentNode");
    EXPECT_EQ(unknown, nullptr);
}
