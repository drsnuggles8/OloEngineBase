// OLO_TEST_LAYER: unit
// =============================================================================
// ActiveEffectsCleanupTest.cpp
//
// Unit tests for the ActiveEffectsContainer cleanup overloads added for
// issue #635: RemoveEffectsBySource(tag, AttributeSet&, GameplayTagContainer&)
// and RemoveEffectByName(name, AttributeSet&, GameplayTagContainer&) must
// revert what an already-ticked effect applied — the persistent AttributeSet
// modifiers (removed by source tag) and the ref-counted granted tags —
// mirroring the HasDuration-expiry path in Tick(). This is what makes an
// Infinite skill-tree passive revocable on refund.
//
// The pre-existing two-argument versions only erase the container entry and
// deliberately leave modifiers/tags in place — pinned here as legacy
// behaviour so a future "fix" can't silently change existing callers.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Effects/ActiveEffectsContainer.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

#include <string>

using namespace OloEngine;

namespace
{
    constexpr f32 kDt = 1.0f / 60.0f;

    GameplayEffect MakeInfinitePassive(const std::string& name, const std::string& attribute,
                                       f32 magnitude, const char* grantedTag = nullptr)
    {
        GameplayEffect fx;
        fx.Name = name;
        fx.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
        fx.Modifiers.push_back({ attribute, AttributeModifier::Operation::Add, magnitude });
        if (grantedTag)
        {
            fx.GrantedTags.AddTag(GameplayTag(grantedTag));
        }
        return fx;
    }

    i32 CountModifiersFromSource(const AttributeSet& attrs, const std::string& attribute,
                                 const GameplayTag& source)
    {
        i32 count = 0;
        for (const auto& mod : attrs.GetModifiers(attribute))
        {
            if (mod.Source.MatchesExact(source))
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

class ActiveEffectsCleanupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Attributes.DefineAttribute("AttackPower", 10.0f);
    }

    ActiveEffectsContainer m_Effects;
    AttributeSet m_Attributes;
    GameplayTagContainer m_OwnerTags;
    GameplayTag m_SourceA{ "Skill.iron_skin" };
    GameplayTag m_SourceB{ "Skill.war_paint" };
};

TEST_F(ActiveEffectsCleanupTest, TickedInfiniteEffectAppliesModifiersAndGrantedTags)
{
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("IronSkin", "AttackPower", 5.0f, "Buff.IronSkin"),
                                      m_OwnerTags, m_SourceA))
        << "an unconditional infinite effect must apply";

    // Modifiers land on the first Tick, not at ApplyEffect time.
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-4f)
        << "modifiers must not land before the container ticks";

    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);

    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 15.0f, 1e-4f)
        << "10 base + 5 Add modifier must be current after one tick";
    EXPECT_EQ(CountModifiersFromSource(m_Attributes, "AttackPower", m_SourceA), 1)
        << "exactly one persistent modifier from the effect's source tag";
    EXPECT_TRUE(m_OwnerTags.HasTagExact(GameplayTag("Buff.IronSkin"))) << "granted tag missing after tick";
    ASSERT_EQ(m_Effects.GetTagGrantCounts().size(), 1u);
    EXPECT_EQ(m_Effects.GetTagGrantCounts().at(GameplayTag("Buff.IronSkin")), 1)
        << "tag grant ref-count must be 1 for a single effect";
}

TEST_F(ActiveEffectsCleanupTest, RemoveBySourceCleanupRevertsModifiersAndTags)
{
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("IronSkin", "AttackPower", 5.0f, "Buff.IronSkin"),
                                      m_OwnerTags, m_SourceA));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);
    ASSERT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 15.0f, 1e-4f);

    m_Effects.RemoveEffectsBySource(m_SourceA, m_Attributes, m_OwnerTags);

    EXPECT_FALSE(m_Effects.HasAnyEffects()) << "the effect entry must be erased";
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-4f)
        << "cleanup removal must revert the persistent modifier back to base";
    EXPECT_EQ(CountModifiersFromSource(m_Attributes, "AttackPower", m_SourceA), 0)
        << "no modifier from the removed source may remain";
    EXPECT_FALSE(m_OwnerTags.HasTagExact(GameplayTag("Buff.IronSkin")))
        << "cleanup removal must revoke the granted tag";
    EXPECT_TRUE(m_Effects.GetTagGrantCounts().empty()) << "the tag grant ref-count entry must be gone";
}

TEST_F(ActiveEffectsCleanupTest, RemoveByNameCleanupRevertsModifiersAndTags)
{
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("IronSkin", "AttackPower", 5.0f, "Buff.IronSkin"),
                                      m_OwnerTags, m_SourceA));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);
    ASSERT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 15.0f, 1e-4f);

    m_Effects.RemoveEffectByName("IronSkin", m_Attributes, m_OwnerTags);

    EXPECT_FALSE(m_Effects.HasAnyEffects()) << "the effect entry must be erased";
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-4f)
        << "name-keyed cleanup removal must revert the persistent modifier";
    EXPECT_FALSE(m_OwnerTags.HasTagExact(GameplayTag("Buff.IronSkin")))
        << "name-keyed cleanup removal must revoke the granted tag";
}

TEST_F(ActiveEffectsCleanupTest, SharedGrantedTagIsRefCountedAcrossEffects)
{
    // Two effects (different names + source tags) both grant Buff.Shared:
    // removing one must NOT strip the tag while the other still holds it.
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("EffectA", "AttackPower", 5.0f, "Buff.Shared"),
                                      m_OwnerTags, m_SourceA));
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("EffectB", "AttackPower", 3.0f, "Buff.Shared"),
                                      m_OwnerTags, m_SourceB));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);

    ASSERT_TRUE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Shared")));
    ASSERT_EQ(m_Effects.GetTagGrantCounts().at(GameplayTag("Buff.Shared")), 2)
        << "two live effects must hold a ref-count of 2 on the shared tag";
    ASSERT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 18.0f, 1e-4f)
        << "10 base + 5 (A) + 3 (B)";

    m_Effects.RemoveEffectsBySource(m_SourceA, m_Attributes, m_OwnerTags);

    EXPECT_TRUE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Shared")))
        << "the shared tag must survive while effect B still grants it";
    EXPECT_EQ(m_Effects.GetTagGrantCounts().at(GameplayTag("Buff.Shared")), 1)
        << "removing one holder must decrement the ref-count to 1, not erase it";
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 13.0f, 1e-4f)
        << "only A's +5 modifier may be reverted (10 + 3 remain)";

    m_Effects.RemoveEffectsBySource(m_SourceB, m_Attributes, m_OwnerTags);

    EXPECT_FALSE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Shared")))
        << "removing the last holder must revoke the shared tag";
    EXPECT_TRUE(m_Effects.GetTagGrantCounts().empty());
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-4f) << "back to base";
}

TEST_F(ActiveEffectsCleanupTest, LegacyTwoArgRemovalLeavesAppliedStateBehind)
{
    // PINNED LEGACY BEHAVIOUR: the two-argument-less overloads only erase the
    // container entries. Callers that need the revert must use the cleanup
    // overloads; changing the legacy semantics would alter every existing
    // call site, so this test documents (not endorses) it.
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("IronSkin", "AttackPower", 5.0f, "Buff.IronSkin"),
                                      m_OwnerTags, m_SourceA));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);
    ASSERT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 15.0f, 1e-4f);

    m_Effects.RemoveEffectsBySource(m_SourceA); // legacy 2-fewer-arg version

    EXPECT_FALSE(m_Effects.HasAnyEffects()) << "legacy removal must still erase the entry";
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 15.0f, 1e-4f)
        << "legacy removal leaves the persistent modifier in place (documented legacy)";
    EXPECT_TRUE(m_OwnerTags.HasTagExact(GameplayTag("Buff.IronSkin")))
        << "legacy removal leaves the granted tag in place (documented legacy)";

    // Same contract for the name-keyed legacy version.
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("WarPaint", "AttackPower", 3.0f), m_OwnerTags, m_SourceB));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);
    ASSERT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 18.0f, 1e-4f);

    m_Effects.RemoveEffectByName("WarPaint"); // legacy 2-fewer-arg version

    EXPECT_FALSE(m_Effects.HasAnyEffects());
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 18.0f, 1e-4f)
        << "legacy name-keyed removal leaves the persistent modifier in place (documented legacy)";
}

TEST_F(ActiveEffectsCleanupTest, RemovingNeverTickedEffectRevertsNothingAndDoesNotUnderflowTagCounts)
{
    // An effect applied but never ticked has neither modifiers nor tags to
    // undo (ModifiersApplied / TagsApplied are still false). The cleanup path
    // must not touch the AttributeSet and must not decrement a tag ref-count
    // it never incremented.
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("NeverTicked", "AttackPower", 5.0f, "Buff.Fresh"),
                                      m_OwnerTags, m_SourceA));
    ASSERT_FALSE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Fresh"))) << "tags land on tick, not apply";

    m_Effects.RemoveEffectsBySource(m_SourceA, m_Attributes, m_OwnerTags);

    EXPECT_FALSE(m_Effects.HasAnyEffects());
    EXPECT_NEAR(m_Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-4f) << "nothing to revert";
    EXPECT_TRUE(m_Effects.GetTagGrantCounts().empty()) << "no phantom ref-count entry may appear";

    // Underflow guard: A grants Buff.Shared and ticks (count 1); B grants the
    // same tag but is removed before ever ticking. B's removal must not
    // decrement A's count.
    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("EffectA", "AttackPower", 5.0f, "Buff.Shared"),
                                      m_OwnerTags, m_SourceA));
    m_Effects.Tick(kDt, m_Attributes, m_OwnerTags);
    ASSERT_EQ(m_Effects.GetTagGrantCounts().at(GameplayTag("Buff.Shared")), 1);

    ASSERT_TRUE(m_Effects.ApplyEffect(MakeInfinitePassive("EffectB", "AttackPower", 3.0f, "Buff.Shared"),
                                      m_OwnerTags, m_SourceB));
    m_Effects.RemoveEffectsBySource(m_SourceB, m_Attributes, m_OwnerTags); // never ticked

    EXPECT_TRUE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Shared")))
        << "removing the never-ticked holder must not strip A's tag";
    EXPECT_EQ(m_Effects.GetTagGrantCounts().at(GameplayTag("Buff.Shared")), 1)
        << "the ref-count must not be decremented by an effect that never granted";

    m_Effects.RemoveEffectsBySource(m_SourceA, m_Attributes, m_OwnerTags);
    EXPECT_FALSE(m_OwnerTags.HasTagExact(GameplayTag("Buff.Shared"))) << "A's removal ends the grant";
    EXPECT_TRUE(m_Effects.GetTagGrantCounts().empty()) << "no residue and no underflow";
}
