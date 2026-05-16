#include "OloEnginePCH.h"

// =============================================================================
// AbilityBlockedByOwnerTagTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility × GameplayAbilityDef.BlockedTags
//   × AbilityComponent.OwnedTags × Scene tick (the BlockedTags state can
//   be mutated by other systems mid-frame; ticking ensures we read
//   stable state). The blocked-tags check stops a caster from using
//   an ability while they hold any of the named tags (Stunned, Silenced,
//   etc.). If TryActivateAbility doesn't gate on this, every game with
//   crowd-control effects breaks.
//
// Scenario: entity owns a Fireball ability whose BlockedTags = {Silenced}.
//   - Activation while NOT Silenced → succeeds, cooldown starts.
//   - Add Silenced tag, wait for cooldown to clear, attempt again → refused.
//   - Remove Silenced, attempt → succeeds again.
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

class AbilityBlockedByOwnerTagTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.Fireball" };
    GameplayTag m_SilencedTag{ "State.Silenced" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Caster");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();

        GameplayAbilityDef def;
        def.Name = "Fireball";
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = 0.2f;
        def.ResourceCost = 0.0f;
        def.BlockedTags.AddTag(m_SilencedTag);

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(AbilityBlockedByOwnerTagTest, OwnerHoldingBlockedTagPreventsActivation)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    ASSERT_FALSE(ac.OwnedTags.HasTagExact(m_SilencedTag));

    // Step 1: unsilenced caster activates fine.
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "fresh caster couldn't activate the ability — preconditions "
           "(cost, required-tags) are mis-set, not a runtime regression.";
    EXPECT_TRUE(ac.Cooldowns.IsOnCooldown(m_AbilityTag));

    // Step 2: clear cooldown by ticking past it, then silence the caster.
    TickFor(0.3f);
    EXPECT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag))
        << "cooldown didn't expire after 0.3s of ticks — orthogonal to "
           "the blocked-tag check but worth flagging.";

    ac.OwnedTags.AddTag(m_SilencedTag);
    EXPECT_FALSE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "Silenced caster successfully activated a Fireball whose "
           "BlockedTags=[State.Silenced] — TryActivateAbility skipped the "
           "BlockedTags check, or HasAny is matching the wrong way.";
    EXPECT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag))
        << "blocked activation still started a cooldown — the early-return "
           "branch fires AFTER cost/cooldown bookkeeping, which is wrong.";

    // Step 3: un-silence and try again — should succeed.
    ac.OwnedTags.RemoveTag(m_SilencedTag);
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "removing the blocked tag didn't re-enable the ability — "
           "OwnedTags.RemoveTag is failing or HasTagExact has stale state.";
}
