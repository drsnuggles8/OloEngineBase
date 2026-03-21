#include <gtest/gtest.h>
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Effects/ActiveEffectsContainer.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/CooldownManager.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"

using namespace OloEngine;

// ============================================================================
// GameplayTag Tests
// ============================================================================

TEST(GameplayTagTest, ConstructAndQuery)
{
    GameplayTag tag("Ability.Spell.Fire.Fireball");
    EXPECT_EQ(tag.GetTagString(), "Ability.Spell.Fire.Fireball");
    EXPECT_TRUE(tag.IsValid());
    EXPECT_NE(tag.GetHash(), 0u);
}

TEST(GameplayTagTest, EmptyTag)
{
    GameplayTag tag;
    EXPECT_FALSE(tag.IsValid());
    EXPECT_EQ(tag.GetTagString(), "");
}

TEST(GameplayTagTest, ExactMatch)
{
    GameplayTag a("Ability.Spell.Fire");
    GameplayTag b("Ability.Spell.Fire");
    GameplayTag c("Ability.Spell.Ice");

    EXPECT_TRUE(a.MatchesExact(b));
    EXPECT_FALSE(a.MatchesExact(c));
}

TEST(GameplayTagTest, PartialMatch)
{
    GameplayTag child("Ability.Spell.Fire.Fireball");
    GameplayTag parent("Ability.Spell");
    GameplayTag differentParent("Ability.Melee");
    GameplayTag exact("Ability.Spell.Fire.Fireball");

    EXPECT_TRUE(child.MatchesPartial(parent));
    EXPECT_FALSE(child.MatchesPartial(differentParent));
    EXPECT_TRUE(child.MatchesPartial(exact));
}

TEST(GameplayTagTest, PartialMatchNotSubstring)
{
    // "Ability.SpellCast" should NOT match parent "Ability.Spell"
    GameplayTag child("Ability.SpellCast");
    GameplayTag parent("Ability.Spell");
    EXPECT_FALSE(child.MatchesPartial(parent));
}

TEST(GameplayTagTest, HashEquality)
{
    GameplayTag a("Status.Stun");
    GameplayTag b("Status.Stun");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.GetHash(), b.GetHash());
}

// ============================================================================
// GameplayTagContainer Tests
// ============================================================================

TEST(GameplayTagContainerTest, AddAndQuery)
{
    GameplayTagContainer container;
    container.AddTag(GameplayTag("Status.Burning"));
    container.AddTag(GameplayTag("Status.Poisoned"));

    EXPECT_TRUE(container.HasTagExact(GameplayTag("Status.Burning")));
    EXPECT_TRUE(container.HasTagExact(GameplayTag("Status.Poisoned")));
    EXPECT_FALSE(container.HasTagExact(GameplayTag("Status.Frozen")));
    EXPECT_EQ(container.Count(), 2u);
}

TEST(GameplayTagContainerTest, NoDuplicates)
{
    GameplayTagContainer container;
    container.AddTag(GameplayTag("Status.Burning"));
    container.AddTag(GameplayTag("Status.Burning"));
    EXPECT_EQ(container.Count(), 1u);
}

TEST(GameplayTagContainerTest, RemoveTag)
{
    GameplayTagContainer container;
    container.AddTag(GameplayTag("Status.Burning"));
    container.RemoveTag(GameplayTag("Status.Burning"));
    EXPECT_FALSE(container.HasTagExact(GameplayTag("Status.Burning")));
    EXPECT_TRUE(container.IsEmpty());
}

TEST(GameplayTagContainerTest, HasAll)
{
    GameplayTagContainer owner;
    owner.AddTag(GameplayTag("Class.Warrior"));
    owner.AddTag(GameplayTag("Status.Enraged"));

    GameplayTagContainer required;
    required.AddTag(GameplayTag("Class.Warrior"));
    required.AddTag(GameplayTag("Status.Enraged"));

    EXPECT_TRUE(owner.HasAll(required));

    required.AddTag(GameplayTag("Status.Invisible"));
    EXPECT_FALSE(owner.HasAll(required));
}

TEST(GameplayTagContainerTest, HasAny)
{
    GameplayTagContainer owner;
    owner.AddTag(GameplayTag("Class.Warrior"));

    GameplayTagContainer check;
    check.AddTag(GameplayTag("Class.Warrior"));
    check.AddTag(GameplayTag("Class.Mage"));

    EXPECT_TRUE(owner.HasAny(check));

    GameplayTagContainer noMatch;
    noMatch.AddTag(GameplayTag("Class.Rogue"));
    EXPECT_FALSE(owner.HasAny(noMatch));
}

TEST(GameplayTagContainerTest, PartialQuery)
{
    GameplayTagContainer container;
    container.AddTag(GameplayTag("Ability.Spell.Fire.Fireball"));
    container.AddTag(GameplayTag("Ability.Melee.Slash"));

    EXPECT_TRUE(container.HasTagPartial(GameplayTag("Ability.Spell")));
    EXPECT_TRUE(container.HasTagPartial(GameplayTag("Ability.Melee")));
    EXPECT_FALSE(container.HasTagPartial(GameplayTag("Status")));
}

// ============================================================================
// AttributeSet Tests
// ============================================================================

TEST(AttributeSetTest, DefineAndGet)
{
    AttributeSet attrs;
    attrs.DefineAttribute("Health", 100.0f);
    attrs.DefineAttribute("Mana", 50.0f);

    EXPECT_FLOAT_EQ(attrs.GetBaseValue("Health"), 100.0f);
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("Health"), 100.0f);
    EXPECT_FLOAT_EQ(attrs.GetBaseValue("Mana"), 50.0f);
    EXPECT_TRUE(attrs.HasAttribute("Health"));
    EXPECT_FALSE(attrs.HasAttribute("Stamina"));
}

TEST(AttributeSetTest, SetBaseValue)
{
    AttributeSet attrs;
    attrs.DefineAttribute("Health", 100.0f);
    attrs.SetBaseValue("Health", 80.0f);
    EXPECT_FLOAT_EQ(attrs.GetBaseValue("Health"), 80.0f);
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("Health"), 80.0f);
}

TEST(AttributeSetTest, AdditiveModifier)
{
    AttributeSet attrs;
    attrs.DefineAttribute("AttackPower", 10.0f);

    AttributeModifier mod;
    mod.Op = AttributeModifier::Operation::Add;
    mod.Magnitude = 5.0f;
    mod.Source = GameplayTag("Buff.Strength");

    attrs.AddModifier("AttackPower", mod);
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("AttackPower"), 15.0f);
    EXPECT_FLOAT_EQ(attrs.GetBaseValue("AttackPower"), 10.0f); // Base unchanged
}

TEST(AttributeSetTest, MultiplicativeModifier)
{
    AttributeSet attrs;
    attrs.DefineAttribute("Defense", 20.0f);

    AttributeModifier mod;
    mod.Op = AttributeModifier::Operation::Multiply;
    mod.Magnitude = 1.5f;
    mod.Source = GameplayTag("Buff.IronSkin");

    attrs.AddModifier("Defense", mod);
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("Defense"), 30.0f); // 20 * 1.5
}

TEST(AttributeSetTest, OverrideModifier)
{
    AttributeSet attrs;
    attrs.DefineAttribute("Speed", 5.0f);

    AttributeModifier mod;
    mod.Op = AttributeModifier::Operation::Override;
    mod.Magnitude = 0.0f;
    mod.Source = GameplayTag("Status.Stun");

    attrs.AddModifier("Speed", mod);
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("Speed"), 0.0f);
}

TEST(AttributeSetTest, StackedModifiers)
{
    AttributeSet attrs;
    attrs.DefineAttribute("AttackPower", 10.0f);

    AttributeModifier addMod;
    addMod.Op = AttributeModifier::Operation::Add;
    addMod.Magnitude = 5.0f;
    addMod.Source = GameplayTag("Buff.A");
    attrs.AddModifier("AttackPower", addMod);

    AttributeModifier mulMod;
    mulMod.Op = AttributeModifier::Operation::Multiply;
    mulMod.Magnitude = 2.0f;
    mulMod.Source = GameplayTag("Buff.B");
    attrs.AddModifier("AttackPower", mulMod);

    // (10 + 5) * 2 = 30
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("AttackPower"), 30.0f);
}

TEST(AttributeSetTest, RemoveModifiersBySource)
{
    AttributeSet attrs;
    attrs.DefineAttribute("AttackPower", 10.0f);

    AttributeModifier mod;
    mod.Op = AttributeModifier::Operation::Add;
    mod.Magnitude = 5.0f;
    mod.Source = GameplayTag("Buff.Strength");
    attrs.AddModifier("AttackPower", mod);

    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("AttackPower"), 15.0f);

    attrs.RemoveModifiersBySource("AttackPower", GameplayTag("Buff.Strength"));
    EXPECT_FLOAT_EQ(attrs.GetCurrentValue("AttackPower"), 10.0f);
}

TEST(AttributeSetTest, GetAttributeNames)
{
    AttributeSet attrs;
    attrs.DefineAttribute("Health", 100.0f);
    attrs.DefineAttribute("Mana", 50.0f);
    auto names = attrs.GetAttributeNames();
    EXPECT_EQ(names.size(), 2u);
}

// ============================================================================
// CooldownManager Tests
// ============================================================================

TEST(CooldownManagerTest, StartAndCheck)
{
    CooldownManager cdm;
    GameplayTag tag("Ability.Fireball");

    cdm.StartCooldown(tag, 5.0f);
    EXPECT_TRUE(cdm.IsOnCooldown(tag));
    EXPECT_FLOAT_EQ(cdm.GetRemainingCooldown(tag), 5.0f);
}

TEST(CooldownManagerTest, TickReducesCooldown)
{
    CooldownManager cdm;
    GameplayTag tag("Ability.Fireball");

    cdm.StartCooldown(tag, 5.0f);
    cdm.Tick(2.0f);
    EXPECT_TRUE(cdm.IsOnCooldown(tag));
    EXPECT_FLOAT_EQ(cdm.GetRemainingCooldown(tag), 3.0f);
}

TEST(CooldownManagerTest, CooldownExpires)
{
    CooldownManager cdm;
    GameplayTag tag("Ability.Fireball");

    cdm.StartCooldown(tag, 3.0f);
    cdm.Tick(4.0f);
    EXPECT_FALSE(cdm.IsOnCooldown(tag));
    EXPECT_FLOAT_EQ(cdm.GetRemainingCooldown(tag), 0.0f);
}

TEST(CooldownManagerTest, CooldownFraction)
{
    CooldownManager cdm;
    GameplayTag tag("Ability.Fireball");

    cdm.StartCooldown(tag, 10.0f);
    cdm.Tick(5.0f);
    EXPECT_FLOAT_EQ(cdm.GetCooldownFraction(tag), 0.5f);
}

TEST(CooldownManagerTest, ResetCooldown)
{
    CooldownManager cdm;
    GameplayTag tag("Ability.Fireball");

    cdm.StartCooldown(tag, 10.0f);
    cdm.ResetCooldown(tag);
    EXPECT_FALSE(cdm.IsOnCooldown(tag));
}

// ============================================================================
// GameplayEffect Tests
// ============================================================================

TEST(ActiveEffectsContainerTest, InstantEffect)
{
    ActiveEffectsContainer container;
    AttributeSet attrs;
    GameplayTagContainer tags;

    attrs.DefineAttribute("Health", 100.0f);

    GameplayEffect healEffect;
    healEffect.Name = "Heal";
    healEffect.Policy.DurationType = GameplayEffectPolicy::Duration::Instant;
    healEffect.Modifiers.push_back({ "Health", AttributeModifier::Operation::Add, 20.0f });

    container.ApplyEffect(healEffect, tags, GameplayTag("Source.Potion"));

    // Tick to process instant effects
    container.Tick(0.0f, attrs, tags);

    EXPECT_FLOAT_EQ(attrs.GetBaseValue("Health"), 120.0f);
    EXPECT_FALSE(container.HasAnyEffects()); // Instant effects are consumed
}

TEST(ActiveEffectsContainerTest, DurationEffect)
{
    ActiveEffectsContainer container;
    AttributeSet attrs;
    GameplayTagContainer tags;

    attrs.DefineAttribute("AttackPower", 10.0f);

    GameplayEffect buff;
    buff.Name = "AttackBuff";
    buff.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    buff.Policy.DurationSeconds = 5.0f;
    buff.GrantedTags.AddTag(GameplayTag("Status.Buffed"));

    container.ApplyEffect(buff, tags, GameplayTag("Source.Spell"));

    container.Tick(1.0f, attrs, tags);
    EXPECT_TRUE(container.HasAnyEffects());
    EXPECT_TRUE(tags.HasTagExact(GameplayTag("Status.Buffed")));

    // After expiration
    container.Tick(5.0f, attrs, tags);
    EXPECT_FALSE(container.HasAnyEffects());
    EXPECT_FALSE(tags.HasTagExact(GameplayTag("Status.Buffed")));
}

TEST(ActiveEffectsContainerTest, PeriodicEffect)
{
    ActiveEffectsContainer container;
    AttributeSet attrs;
    GameplayTagContainer tags;

    attrs.DefineAttribute("Health", 100.0f);

    GameplayEffect dot;
    dot.Name = "Poison";
    dot.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    dot.Policy.DurationSeconds = 5.0f;
    dot.Policy.IsPeriodic = true;
    dot.Policy.PeriodSeconds = 1.0f;
    dot.Modifiers.push_back({ "Health", AttributeModifier::Operation::Add, -5.0f });

    container.ApplyEffect(dot, tags, GameplayTag("Source.Enemy"));

    // Tick 3 seconds - should apply 3 times
    container.Tick(3.0f, attrs, tags);
    EXPECT_FLOAT_EQ(attrs.GetBaseValue("Health"), 85.0f); // 100 - (3 * 5)
}

TEST(ActiveEffectsContainerTest, StackingEffect)
{
    ActiveEffectsContainer container;
    AttributeSet attrs;
    GameplayTagContainer tags;

    attrs.DefineAttribute("Health", 100.0f);

    GameplayEffect bleed;
    bleed.Name = "Bleed";
    bleed.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    bleed.Policy.DurationSeconds = 10.0f;
    bleed.MaxStacks = 5;
    bleed.RefreshDurationOnStack = true;

    container.ApplyEffect(bleed, tags, GameplayTag("Source.Attack"));
    container.ApplyEffect(bleed, tags, GameplayTag("Source.Attack"));
    container.ApplyEffect(bleed, tags, GameplayTag("Source.Attack"));

    auto const& effects = container.GetActiveEffects();
    EXPECT_EQ(effects.size(), 1u); // Stacked into one
    EXPECT_EQ(effects[0].CurrentStacks, 3);
}

TEST(ActiveEffectsContainerTest, RequiredTagsBlocked)
{
    ActiveEffectsContainer container;
    GameplayTagContainer tags;

    GameplayEffect effect;
    effect.Name = "WarriorBuff";
    effect.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    effect.Policy.DurationSeconds = 5.0f;
    effect.RequiredTags.AddTag(GameplayTag("Class.Warrior"));

    // Should fail - target doesn't have required tag
    EXPECT_FALSE(container.ApplyEffect(effect, tags, GameplayTag("Source.Spell")));

    // Add the required tag
    tags.AddTag(GameplayTag("Class.Warrior"));
    EXPECT_TRUE(container.ApplyEffect(effect, tags, GameplayTag("Source.Spell")));
}

TEST(ActiveEffectsContainerTest, BlockedTagsPrevents)
{
    ActiveEffectsContainer container;
    GameplayTagContainer tags;
    tags.AddTag(GameplayTag("Status.Immune"));

    GameplayEffect effect;
    effect.Name = "Debuff";
    effect.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    effect.Policy.DurationSeconds = 5.0f;
    effect.BlockedTags.AddTag(GameplayTag("Status.Immune"));

    EXPECT_FALSE(container.ApplyEffect(effect, tags, GameplayTag("Source.Enemy")));
}

TEST(ActiveEffectsContainerTest, RefCountedTags)
{
    ActiveEffectsContainer container;
    AttributeSet attrs;
    GameplayTagContainer tags;

    // Two effects granting the same tag
    GameplayEffect buffA;
    buffA.Name = "BuffA";
    buffA.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    buffA.Policy.DurationSeconds = 5.0f;
    buffA.GrantedTags.AddTag(GameplayTag("Status.Empowered"));

    GameplayEffect buffB;
    buffB.Name = "BuffB";
    buffB.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
    buffB.Policy.DurationSeconds = 10.0f;
    buffB.GrantedTags.AddTag(GameplayTag("Status.Empowered"));

    container.ApplyEffect(buffA, tags, GameplayTag("Source.A"));
    container.ApplyEffect(buffB, tags, GameplayTag("Source.B"));

    container.Tick(0.01f, attrs, tags);
    EXPECT_TRUE(tags.HasTagExact(GameplayTag("Status.Empowered")));

    // Expire first effect — tag should persist (ref count > 0)
    container.Tick(5.0f, attrs, tags);
    EXPECT_TRUE(tags.HasTagExact(GameplayTag("Status.Empowered")));

    // Expire second effect — tag should be removed
    container.Tick(5.0f, attrs, tags);
    EXPECT_FALSE(tags.HasTagExact(GameplayTag("Status.Empowered")));
}

// ============================================================================
// DamageCalculation Tests
// ============================================================================

TEST(DamageCalculationTest, BasicDamage)
{
    DamageEvent event;
    event.RawDamage = 50.0f;
    event.DamageType = GameplayTag("Damage.Physical");
    event.IsCritical = false;

    AttributeSet source;
    source.DefineAttribute("AttackPower", 10.0f);

    AttributeSet target;
    target.DefineAttribute("Defense", 15.0f);

    // 50 + 10(atk) - 15(def) = 45
    f32 damage = DamageCalculation::Calculate(event, source, target);
    EXPECT_FLOAT_EQ(damage, 45.0f);
}

TEST(DamageCalculationTest, CriticalHit)
{
    DamageEvent event;
    event.RawDamage = 30.0f;
    event.DamageType = GameplayTag("Damage.Physical");
    event.IsCritical = true;
    event.CritMultiplier = 2.0f;

    AttributeSet source;
    source.DefineAttribute("AttackPower", 0.0f);

    AttributeSet target;
    target.DefineAttribute("Defense", 10.0f);

    // (30 + 0) * 2.0 - 10 = 50
    f32 damage = DamageCalculation::Calculate(event, source, target);
    EXPECT_FLOAT_EQ(damage, 50.0f);
}

TEST(DamageCalculationTest, Resistance)
{
    DamageEvent event;
    event.RawDamage = 100.0f;
    event.DamageType = GameplayTag("Fire");
    event.IsCritical = false;

    AttributeSet source;
    source.DefineAttribute("AttackPower", 0.0f);

    AttributeSet target;
    target.DefineAttribute("Defense", 0.0f);
    target.DefineAttribute("Resistance.Fire", 0.5f); // 50% fire resistance

    // 100 * (1.0 - 0.5) = 50
    f32 damage = DamageCalculation::Calculate(event, source, target);
    EXPECT_FLOAT_EQ(damage, 50.0f);
}

TEST(DamageCalculationTest, MinimumZeroDamage)
{
    DamageEvent event;
    event.RawDamage = 5.0f;
    event.DamageType = GameplayTag("Damage.Physical");
    event.IsCritical = false;

    AttributeSet source;
    source.DefineAttribute("AttackPower", 0.0f);

    AttributeSet target;
    target.DefineAttribute("Defense", 100.0f);

    f32 damage = DamageCalculation::Calculate(event, source, target);
    EXPECT_FLOAT_EQ(damage, 0.0f);
}

TEST(DamageCalculationTest, CustomFormula)
{
    DamageCalculation::SetCustomFormula([](const DamageEvent& e, const AttributeSet&, const AttributeSet&) -> f32
                                        { return e.RawDamage * 3.0f; });

    DamageEvent event;
    event.RawDamage = 10.0f;
    event.DamageType = GameplayTag("Damage.Physical");

    AttributeSet empty;

    f32 damage = DamageCalculation::Calculate(event, empty, empty);
    EXPECT_FLOAT_EQ(damage, 30.0f);

    DamageCalculation::ClearCustomFormula();
}

TEST(DamageCalculationTest, ScopedFormula)
{
    DamageEvent event;
    event.RawDamage = 10.0f;
    event.DamageType = GameplayTag("Damage.Physical");

    AttributeSet source;
    source.DefineAttribute("AttackPower", 0.0f);
    AttributeSet target;
    target.DefineAttribute("Defense", 0.0f);

    // Default formula: 10 + 0 - 0 = 10
    EXPECT_FLOAT_EQ(DamageCalculation::Calculate(event, source, target), 10.0f);

    {
        DamageCalculation::ScopedFormula guard([](const DamageEvent& e, const AttributeSet&, const AttributeSet&) -> f32
                                               { return e.RawDamage * 5.0f; });
        EXPECT_FLOAT_EQ(DamageCalculation::Calculate(event, source, target), 50.0f);
    }

    // Reverts to default after scope exit
    EXPECT_FLOAT_EQ(DamageCalculation::Calculate(event, source, target), 10.0f);
}

// ============================================================================
// AbilityComponent Tests
// ============================================================================

TEST(AbilityComponentTest, InitializeDefaultRPG)
{
    AbilityComponent ac;
    ac.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 5.0f);

    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("MaxHealth"), 100.0f);
    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("Health"), 100.0f);
    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("MaxMana"), 50.0f);
    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("Mana"), 50.0f);
    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("AttackPower"), 10.0f);
    EXPECT_FLOAT_EQ(ac.Attributes.GetBaseValue("Defense"), 5.0f);
}

TEST(AbilityComponentTest, EqualityOperator)
{
    AbilityComponent a;
    a.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 5.0f);

    AbilityComponent b;
    b.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 5.0f);

    EXPECT_EQ(a, b);

    b.Attributes.SetBaseValue("Health", 80.0f);
    EXPECT_NE(a, b);

    // Reset and test OwnedTags divergence
    b.Attributes.SetBaseValue("Health", 100.0f);
    EXPECT_EQ(a, b);
    b.OwnedTags.AddTag(GameplayTag("Status.Buffed"));
    EXPECT_NE(a, b);

    // Reset and test Abilities divergence
    b.OwnedTags.RemoveTag(GameplayTag("Status.Buffed"));
    EXPECT_EQ(a, b);
    ActiveAbility aa;
    aa.Definition.Name = "Slash";
    aa.Definition.AbilityTag = GameplayTag("Ability.Melee.Slash");
    b.Abilities.push_back(aa);
    EXPECT_NE(a, b);
}
