#include "OloEnginePCH.h"

// =============================================================================
// AbilityResourceCostDeductedFromManaTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility (resource-cost gate +
//   commit) × AbilityComponent.Attributes (CostAttribute base value).
//   TryActivateAbility has TWO touches on the cost attribute:
//     1. Pre-check: `GetCurrentValue(CostAttribute) < ResourceCost` →
//        refuse activation.
//     2. Commit: `SetBaseValue(CostAttribute, current - ResourceCost)`.
//   A regression where the pre-check uses the wrong attribute or polarity
//   either lets free casts through OR makes all abilities permanently
//   ungated; a regression in the commit either double-deducts or never
//   deducts. Both halves are exercised here.
//
// Scenario: caster with Mana=30, MaxMana=50. Ability costs 20 Mana.
//   - First activation succeeds, Mana drops to 10.
//   - Second activation refused (10 < 20).
//   - Restore Mana to 25, third activation succeeds (cooldown is 0 so
//     immediate retry works), Mana drops to 5.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AbilityResourceCostDeductedFromManaTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.Frostbolt" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Caster");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();
        ac.Attributes.DefineAttribute("MaxMana", 50.0f);
        ac.Attributes.DefineAttribute("Mana", 30.0f);

        GameplayAbilityDef def;
        def.Name = "Frostbolt";
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = 0.0f;
        def.ResourceCost = 20.0f;
        def.CostAttribute = "Mana";

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(AbilityResourceCostDeductedFromManaTest, ActivationDeductsCostAndRefusesWhenInsufficient)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    ASSERT_NEAR(ac.Attributes.GetBaseValue("Mana"), 30.0f, 1e-3f);

    // First activation: 30 Mana >= 20 cost → succeeds, deducts 20.
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Mana"), 10.0f, 1e-3f)
        << "ResourceCost commit didn't deduct Mana — SetBaseValue branch missing or "
           "writes to the wrong attribute (the CostAttribute string mismatch).";

    // Second activation: 10 Mana < 20 cost → refused, Mana unchanged.
    EXPECT_FALSE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "ability activated with insufficient Mana — the `current < ResourceCost` "
           "check is inverted, dropped, or compares against the wrong attribute.";
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Mana"), 10.0f, 1e-3f)
        << "refused activation still deducted Mana — the cost-commit path runs "
           "before the cost-precheck, instead of after.";

    // Restore Mana, try again — third activation succeeds and deducts.
    ac.Attributes.SetBaseValue("Mana", 25.0f);
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Mana"), 5.0f, 1e-3f);
}
