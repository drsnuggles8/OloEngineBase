#include "OloEnginePCH.h"

// =============================================================================
// AbilityRequiredTagGatesActivationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility × GameplayAbilityDef.RequiredTags
//   × AbilityComponent.OwnedTags. RequiredTags is the inverse of
//   BlockedTags: every named tag in RequiredTags must be present on the
//   owner for activation to succeed. The AbilityBlockedByOwnerTagTest
//   already pins the "blocked when X is present" path; this one pins
//   "rejected when X is absent". A regression that swaps the polarity
//   of the check turns the gate inside-out — abilities suddenly
//   activate without their prerequisites, or never activate at all.
//
// Scenario: ability `BashWithWeapon` requires `State.WieldingWeapon`.
//   - Without the tag → activation rejected.
//   - With the tag → activation succeeds.
//   - Removing the tag (mid-cooldown) → next attempt rejected again.
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

class AbilityRequiredTagGatesActivationTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.BashWithWeapon" };
    GameplayTag m_WieldingTag{ "State.WieldingWeapon" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Caster");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();

        GameplayAbilityDef def;
        def.Name = "BashWithWeapon";
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = 0.2f;
        def.ResourceCost = 0.0f;
        def.RequiredTags.AddTag(m_WieldingTag);

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(AbilityRequiredTagGatesActivationTest, RequiredTagAbsenceBlocksActivationPresenceAllowsIt)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    ASSERT_FALSE(ac.OwnedTags.HasTagExact(m_WieldingTag));

    // Without the required tag: activation refused.
    EXPECT_FALSE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "ability with RequiredTags=[State.WieldingWeapon] activated even "
           "though the owner doesn't hold the tag — TryActivateAbility's "
           "HasAll(RequiredTags) check is inverted or unconditionally true.";
    EXPECT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag))
        << "refused activation still started a cooldown — the early-return "
           "branch is firing AFTER bookkeeping rather than before.";

    // Grant the tag and try again — should succeed and start cooldown.
    ac.OwnedTags.AddTag(m_WieldingTag);
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "added the required tag but activation still refused — the "
           "RequiredTags check rejected an owner that legitimately holds "
           "all required tags.";
    EXPECT_TRUE(ac.Cooldowns.IsOnCooldown(m_AbilityTag));

    // Clear cooldown. Strip the tag. Try once more — refused.
    TickFor(0.3f);
    EXPECT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag));
    ac.OwnedTags.RemoveTag(m_WieldingTag);
    EXPECT_FALSE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "after removing the required tag, ability still activated — "
           "OwnedTags.RemoveTag is a no-op, OR HasTagExact has cached state.";
}
