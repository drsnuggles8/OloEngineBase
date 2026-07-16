// OLO_TEST_LAYER: unit
// =============================================================================
// SkillTreeDatabaseTest.cpp
//
// Unit tests for the SkillTreeDatabase asset (issue #635): the Validate()
// matrix (unique non-empty node ids, no self/dangling prerequisites, acyclic
// prerequisite graph via Kahn's algorithm), the FindNode/RebuildIndex
// contract, and the SkillTreeDatabaseSerializer string round-trip — which
// must preserve every node field (both payload kinds, prerequisites and the
// EditorPosition canvas layout), force a passive payload's duration policy
// to Infinite on load, and REJECT structurally invalid trees rather than
// loading them (a cycle in shipped content would deadlock every unlock gate
// behind it).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"

#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    SkillTreeNode MakeNode(std::string id, std::vector<std::string> prereqs = {})
    {
        SkillTreeNode node;
        node.NodeID = std::move(id);
        node.Prerequisites = std::move(prereqs);
        return node;
    }
} // namespace

// ============================================================================
// Validate() matrix
// ============================================================================

TEST(SkillTreeDatabaseTest, ValidateAcceptsDagAndForest)
{
    SkillTreeDatabase tree;
    // Diamond A -> {B, C} -> D, plus an isolated second root E (a forest —
    // one asset may hold several visual trees).
    tree.m_Nodes.push_back(MakeNode("A"));
    tree.m_Nodes.push_back(MakeNode("B", { "A" }));
    tree.m_Nodes.push_back(MakeNode("C", { "A" }));
    tree.m_Nodes.push_back(MakeNode("D", { "B", "C" }));
    tree.m_Nodes.push_back(MakeNode("E"));

    std::string error;
    EXPECT_TRUE(tree.Validate(&error))
        << "a valid diamond DAG + isolated root must pass validation, got: " << error;
    EXPECT_TRUE(error.empty()) << "no error text should be written on success, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsEmptyNodeID)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode(""));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "an empty NodeID must be rejected";
    EXPECT_NE(error.find("empty NodeID"), std::string::npos)
        << "error must name the empty-id failure, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsDuplicateNodeID)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode("dup"));
    tree.m_Nodes.push_back(MakeNode("dup"));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "duplicate NodeIDs must be rejected";
    EXPECT_NE(error.find("duplicate NodeID 'dup'"), std::string::npos)
        << "error must name the duplicated id, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsSelfPrerequisite)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode("narcissus", { "narcissus" }));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "a node listing itself as prerequisite must be rejected";
    EXPECT_NE(error.find("lists itself"), std::string::npos)
        << "error must name the self-prerequisite failure, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsDanglingPrerequisite)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode("A"));
    tree.m_Nodes.push_back(MakeNode("B", { "ghost" }));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "a prerequisite referencing a missing node must be rejected";
    EXPECT_NE(error.find("unknown prerequisite 'ghost'"), std::string::npos)
        << "error must name the dangling prerequisite, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsTwoCycleNamingBothNodes)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode("A", { "B" }));
    tree.m_Nodes.push_back(MakeNode("B", { "A" }));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "a 2-cycle A<->B must be rejected";
    EXPECT_NE(error.find("prerequisite cycle"), std::string::npos)
        << "error must identify the cycle failure, got: " << error;
    EXPECT_NE(error.find("A"), std::string::npos) << "error must name cycle member A, got: " << error;
    EXPECT_NE(error.find("B"), std::string::npos) << "error must name cycle member B, got: " << error;
}

TEST(SkillTreeDatabaseTest, ValidateRejectsThreeCycleNamingCycleNodes)
{
    SkillTreeDatabase tree;
    // legit root feeding a 3-cycle: X -> Y -> Z -> X (root not in the cycle).
    tree.m_Nodes.push_back(MakeNode("root"));
    tree.m_Nodes.push_back(MakeNode("X", { "Z", "root" }));
    tree.m_Nodes.push_back(MakeNode("Y", { "X" }));
    tree.m_Nodes.push_back(MakeNode("Z", { "Y" }));

    std::string error;
    EXPECT_FALSE(tree.Validate(&error)) << "a 3-cycle X->Y->Z->X must be rejected";
    EXPECT_NE(error.find("prerequisite cycle"), std::string::npos) << "got: " << error;
    EXPECT_NE(error.find("X"), std::string::npos) << "error must name cycle member X, got: " << error;
    EXPECT_NE(error.find("Y"), std::string::npos) << "error must name cycle member Y, got: " << error;
    EXPECT_NE(error.find("Z"), std::string::npos) << "error must name cycle member Z, got: " << error;
    EXPECT_EQ(error.find("root"), std::string::npos)
        << "the Kahn residue must contain only the cycle nodes, not the drained root, got: " << error;
}

// ============================================================================
// FindNode / RebuildIndex contract
// ============================================================================

TEST(SkillTreeDatabaseTest, FindNodeRequiresRebuildIndexAfterMutation)
{
    SkillTreeDatabase tree;
    tree.m_Nodes.push_back(MakeNode("alpha"));

    // Direct m_Nodes mutation without RebuildIndex: lookup must miss (the
    // serializer rebuilds on load; hand-built trees must do it themselves).
    EXPECT_EQ(tree.FindNode("alpha"), nullptr)
        << "FindNode must not see nodes added without a RebuildIndex() call";

    tree.RebuildIndex();
    const SkillTreeNode* found = tree.FindNode("alpha");
    ASSERT_NE(found, nullptr) << "FindNode must find the node after RebuildIndex()";
    EXPECT_EQ(found->NodeID, "alpha");

    EXPECT_EQ(tree.FindNode("missing"), nullptr) << "an unknown id must return nullptr";

    tree.m_Nodes.push_back(MakeNode("beta"));
    tree.RebuildIndex();
    EXPECT_NE(tree.FindNode("beta"), nullptr) << "a second RebuildIndex must pick up new nodes";
}

// ============================================================================
// Serializer round-trip
// ============================================================================

namespace
{
    // A fully-populated three-node tree exercising every serialized field:
    // a pure-prerequisite root, an Ability payload, and a PassiveEffect
    // payload with modifiers + granted tags.
    Ref<SkillTreeDatabase> MakeRichTree()
    {
        auto tree = Ref<SkillTreeDatabase>::Create();
        tree->m_TreeID = "warrior_arms";
        tree->m_DisplayName = "Arms Mastery";

        SkillTreeNode root = MakeNode("root");
        root.DisplayName = "Foundation";
        root.Description = "Where it all begins.";
        root.LevelRequirement = 1;
        root.SkillPointCost = 1;
        root.Payload = SkillTreeNode::PayloadKind::None;
        root.EditorPosition = { 12.5f, -8.25f };
        tree->m_Nodes.push_back(std::move(root));

        SkillTreeNode ability = MakeNode("cleave", { "root" });
        ability.DisplayName = "Cleave";
        ability.Description = "Sweeping strike.";
        ability.LevelRequirement = 3;
        ability.SkillPointCost = 2;
        ability.Payload = SkillTreeNode::PayloadKind::Ability;
        ability.EditorPosition = { -40.0f, 160.0f };
        ability.GrantedAbility.Name = "Cleave";
        ability.GrantedAbility.AbilityTag = GameplayTag("Ability.Cleave");
        ability.GrantedAbility.CooldownDuration = 6.5f;
        ability.GrantedAbility.ResourceCost = 25.0f;
        ability.GrantedAbility.CostAttribute = "Stamina";
        ability.GrantedAbility.IsChanneled = true;
        ability.GrantedAbility.IsToggled = false;
        ability.GrantedAbility.ChannelDuration = 1.5f;
        ability.GrantedAbility.RequiredTags.AddTag(GameplayTag("State.Alive"));
        ability.GrantedAbility.BlockedTags.AddTag(GameplayTag("State.Stunned"));
        ability.GrantedAbility.ActivationGrantedTags.AddTag(GameplayTag("State.Cleaving"));
        {
            GameplayEffect fx;
            fx.Name = "CleaveSlow";
            fx.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
            fx.Policy.DurationSeconds = 5.0f;
            fx.Policy.IsPeriodic = true;
            fx.Policy.PeriodSeconds = 0.5f;
            fx.Modifiers.push_back({ "Mana", AttributeModifier::Operation::Multiply, 0.5f });
            fx.MaxStacks = 2;
            fx.RefreshDurationOnStack = false;
            fx.GrantedTags.AddTag(GameplayTag("Debuff.Slow"));
            ability.GrantedAbility.ActivationEffects.push_back(std::move(fx));
        }
        tree->m_Nodes.push_back(std::move(ability));

        SkillTreeNode passive = MakeNode("iron_skin", { "root" });
        passive.DisplayName = "Iron Skin";
        passive.Description = "Permanent toughness.";
        passive.LevelRequirement = 2;
        passive.SkillPointCost = 3;
        passive.Payload = SkillTreeNode::PayloadKind::PassiveEffect;
        passive.EditorPosition = { 40.0f, 160.0f };
        passive.PassiveEffect.Name = "Skill.iron_skin";
        passive.PassiveEffect.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
        passive.PassiveEffect.Modifiers.push_back({ "AttackPower", AttributeModifier::Operation::Add, 5.0f });
        passive.PassiveEffect.Modifiers.push_back({ "Defense", AttributeModifier::Operation::Add, 2.5f });
        passive.PassiveEffect.MaxStacks = 3; // clamped to 1 only at UNLOCK time, not load
        passive.PassiveEffect.GrantedTags.AddTag(GameplayTag("Buff.IronSkin"));
        tree->m_Nodes.push_back(std::move(passive));

        tree->RebuildIndex();
        return tree;
    }
} // namespace

TEST(SkillTreeDatabaseTest, SerializerRoundTripPreservesEveryNodeField)
{
    auto original = MakeRichTree();
    std::string validationError;
    ASSERT_TRUE(original->Validate(&validationError)) << validationError;

    SkillTreeDatabaseSerializer serializer;
    const std::string yaml = serializer.TestSerializeToYAML(original);
    ASSERT_FALSE(yaml.empty());

    auto reloaded = Ref<SkillTreeDatabase>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, reloaded))
        << "deserialize rejected the just-serialized valid tree. YAML:\n"
        << yaml;

    EXPECT_EQ(reloaded->m_TreeID, "warrior_arms") << "TreeID dropped in round-trip";
    EXPECT_EQ(reloaded->m_DisplayName, "Arms Mastery") << "DisplayName dropped in round-trip";
    ASSERT_EQ(reloaded->m_Nodes.size(), 3u) << "node count changed in round-trip";

    // Deserialize must have rebuilt the index — FindNode works immediately.
    const SkillTreeNode* root = reloaded->FindNode("root");
    ASSERT_NE(root, nullptr) << "the serializer must RebuildIndex() on load";
    EXPECT_EQ(root->DisplayName, "Foundation");
    EXPECT_EQ(root->Description, "Where it all begins.");
    EXPECT_EQ(root->LevelRequirement, 1);
    EXPECT_EQ(root->SkillPointCost, 1);
    EXPECT_TRUE(root->Prerequisites.empty()) << "root node gained phantom prerequisites";
    EXPECT_EQ(root->Payload, SkillTreeNode::PayloadKind::None);
    EXPECT_NEAR(root->EditorPosition.x, 12.5f, 1e-4f) << "EditorPosition.x dropped";
    EXPECT_NEAR(root->EditorPosition.y, -8.25f, 1e-4f) << "EditorPosition.y dropped";

    const SkillTreeNode* cleave = reloaded->FindNode("cleave");
    ASSERT_NE(cleave, nullptr);
    EXPECT_EQ(cleave->Payload, SkillTreeNode::PayloadKind::Ability);
    ASSERT_EQ(cleave->Prerequisites.size(), 1u);
    EXPECT_EQ(cleave->Prerequisites[0], "root");
    EXPECT_EQ(cleave->LevelRequirement, 3);
    EXPECT_EQ(cleave->SkillPointCost, 2);
    const auto& def = cleave->GrantedAbility;
    EXPECT_EQ(def.Name, "Cleave");
    EXPECT_EQ(def.AbilityTag.GetTagString(), "Ability.Cleave");
    EXPECT_NEAR(def.CooldownDuration, 6.5f, 1e-4f);
    EXPECT_NEAR(def.ResourceCost, 25.0f, 1e-4f);
    EXPECT_EQ(def.CostAttribute, "Stamina");
    EXPECT_TRUE(def.IsChanneled);
    EXPECT_FALSE(def.IsToggled);
    EXPECT_NEAR(def.ChannelDuration, 1.5f, 1e-4f);
    EXPECT_TRUE(def.RequiredTags.HasTagExact(GameplayTag("State.Alive"))) << "RequiredTags dropped";
    EXPECT_TRUE(def.BlockedTags.HasTagExact(GameplayTag("State.Stunned"))) << "BlockedTags dropped";
    EXPECT_TRUE(def.ActivationGrantedTags.HasTagExact(GameplayTag("State.Cleaving")))
        << "ActivationGrantedTags dropped";
    ASSERT_EQ(def.ActivationEffects.size(), 1u) << "ActivationEffects dropped";
    const auto& fx = def.ActivationEffects[0];
    EXPECT_EQ(fx.Name, "CleaveSlow");
    EXPECT_EQ(fx.Policy.DurationType, GameplayEffectPolicy::Duration::HasDuration);
    EXPECT_NEAR(fx.Policy.DurationSeconds, 5.0f, 1e-4f);
    EXPECT_TRUE(fx.Policy.IsPeriodic);
    EXPECT_NEAR(fx.Policy.PeriodSeconds, 0.5f, 1e-4f);
    ASSERT_EQ(fx.Modifiers.size(), 1u);
    EXPECT_EQ(fx.Modifiers[0].AttributeName, "Mana");
    EXPECT_EQ(fx.Modifiers[0].Op, AttributeModifier::Operation::Multiply);
    EXPECT_NEAR(fx.Modifiers[0].Magnitude, 0.5f, 1e-4f);
    EXPECT_EQ(fx.MaxStacks, 2);
    EXPECT_FALSE(fx.RefreshDurationOnStack);
    EXPECT_TRUE(fx.GrantedTags.HasTagExact(GameplayTag("Debuff.Slow")));

    const SkillTreeNode* passive = reloaded->FindNode("iron_skin");
    ASSERT_NE(passive, nullptr);
    EXPECT_EQ(passive->Payload, SkillTreeNode::PayloadKind::PassiveEffect);
    EXPECT_EQ(passive->PassiveEffect.Name, "Skill.iron_skin");
    EXPECT_EQ(passive->PassiveEffect.Policy.DurationType, GameplayEffectPolicy::Duration::Infinite);
    ASSERT_EQ(passive->PassiveEffect.Modifiers.size(), 2u) << "passive modifiers dropped";
    EXPECT_EQ(passive->PassiveEffect.Modifiers[0].AttributeName, "AttackPower");
    EXPECT_NEAR(passive->PassiveEffect.Modifiers[0].Magnitude, 5.0f, 1e-4f);
    EXPECT_EQ(passive->PassiveEffect.Modifiers[1].AttributeName, "Defense");
    EXPECT_NEAR(passive->PassiveEffect.Modifiers[1].Magnitude, 2.5f, 1e-4f);
    EXPECT_EQ(passive->PassiveEffect.MaxStacks, 3)
        << "MaxStacks is only forced to 1 at unlock time, never at load";
    EXPECT_TRUE(passive->PassiveEffect.GrantedTags.HasTagExact(GameplayTag("Buff.IronSkin")));
    EXPECT_NEAR(passive->EditorPosition.x, 40.0f, 1e-4f);
    EXPECT_NEAR(passive->EditorPosition.y, 160.0f, 1e-4f);
}

TEST(SkillTreeDatabaseTest, SerializerForcesPassivePolicyInfiniteOnLoad)
{
    // Author a passive whose YAML claims HasDuration — a hand-edit that would
    // otherwise let a "permanent" talent expire mid-session.
    auto tree = Ref<SkillTreeDatabase>::Create();
    SkillTreeNode node = MakeNode("timed_talent");
    node.Payload = SkillTreeNode::PayloadKind::PassiveEffect;
    node.PassiveEffect.Name = "Skill.timed_talent";
    node.PassiveEffect.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    node.PassiveEffect.Policy.DurationSeconds = 30.0f;
    tree->m_Nodes.push_back(std::move(node));
    tree->RebuildIndex();

    SkillTreeDatabaseSerializer serializer;
    const std::string yaml = serializer.TestSerializeToYAML(tree);
    ASSERT_NE(yaml.find("HasDuration"), std::string::npos)
        << "test premise broken: the emitted YAML should carry the authored HasDuration policy";

    auto reloaded = Ref<SkillTreeDatabase>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, reloaded));
    const SkillTreeNode* loaded = reloaded->FindNode("timed_talent");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->PassiveEffect.Policy.DurationType, GameplayEffectPolicy::Duration::Infinite)
        << "skill passives must be forced Infinite on load regardless of authored policy";
}

TEST(SkillTreeDatabaseTest, SerializerRejectsCycleDuplicateAndDanglingPrereq)
{
    SkillTreeDatabaseSerializer serializer;

    {
        // 2-cycle: serialize happily (write side never validates), load rejects.
        auto cyclic = Ref<SkillTreeDatabase>::Create();
        cyclic->m_Nodes.push_back(MakeNode("A", { "B" }));
        cyclic->m_Nodes.push_back(MakeNode("B", { "A" }));
        auto scratch = Ref<SkillTreeDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(cyclic), scratch))
            << "a prerequisite cycle must reject the whole asset on load";
    }
    {
        auto dup = Ref<SkillTreeDatabase>::Create();
        dup->m_Nodes.push_back(MakeNode("dup"));
        dup->m_Nodes.push_back(MakeNode("dup"));
        auto scratch = Ref<SkillTreeDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(dup), scratch))
            << "duplicate NodeIDs must reject the whole asset on load";
    }
    {
        auto dangling = Ref<SkillTreeDatabase>::Create();
        dangling->m_Nodes.push_back(MakeNode("B", { "ghost" }));
        auto scratch = Ref<SkillTreeDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(dangling), scratch))
            << "a dangling prerequisite must reject the whole asset on load";
    }
    {
        auto empty = Ref<SkillTreeDatabase>::Create();
        empty->m_Nodes.push_back(MakeNode(""));
        auto scratch = Ref<SkillTreeDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(empty), scratch))
            << "an empty NodeID must reject the whole asset on load";
    }
}

TEST(SkillTreeDatabaseTest, SerializerRejectsAbilityNodeWithoutAbilityTag)
{
    // An Ability-payload node whose GrantedAbility carries no tag could never
    // be activated (TryActivateAbility matches by tag) — reject at load.
    auto tree = Ref<SkillTreeDatabase>::Create();
    SkillTreeNode node = MakeNode("tagless");
    node.Payload = SkillTreeNode::PayloadKind::Ability;
    node.GrantedAbility.Name = "Nameless Wonder"; // deliberately no AbilityTag
    tree->m_Nodes.push_back(std::move(node));

    SkillTreeDatabaseSerializer serializer;
    auto scratch = Ref<SkillTreeDatabase>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(tree), scratch))
        << "an Ability node without an AbilityTag must reject the asset on load";
}
