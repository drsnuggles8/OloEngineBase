#include "OloEnginePCH.h"

// =============================================================================
// AbilityCooldownTicksDownViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × GameplayAbilitySystem::OnUpdate × CooldownManager::Tick.
//   TryActivateAbility starts a cooldown on the AbilityComponent; the
//   per-tick OnUpdate decrements every cooldown by `dt`. The seam is
//   "ability activation marks the cooldown" → "the scene's tick drives
//   that cooldown to zero". Both halves must agree on the same dt or the
//   ability is either unusably long or never usable.
//
// Scenario: one Player entity with an AbilityComponent that has a single
// non-channeled ability with CooldownDuration=0.5s. After
// TryActivateAbility succeeds, the cooldown should be ~0.5s, drop to
// ~0.25s after 0.25s of ticks, and reach 0 (off-cooldown) after 0.6s.
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

class AbilityCooldownTicksDownViaSceneTickTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.Fireball" };

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        auto& ac = m_Player.AddComponent<AbilityComponent>();

        GameplayAbilityDef def;
        def.Name = "Fireball";
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = 0.5f;
        def.ResourceCost = 0.0f; // free, isolate the cooldown signal
        def.IsChanneled = false;
        def.IsToggled = false;

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Player;
};

TEST_F(AbilityCooldownTicksDownViaSceneTickTest, CooldownStartsOnActivationAndDecrementsViaSceneTick)
{
    auto& ac = m_Player.GetComponent<AbilityComponent>();

    // Initially off-cooldown.
    ASSERT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag));

    const bool activated = GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Player, m_AbilityTag);
    ASSERT_TRUE(activated)
        << "TryActivateAbility returned false on a fresh activation — preconditions "
           "(cooldown, required/blocked tags, resource cost) are mis-set in the test "
           "fixture, not a runtime regression.";
    EXPECT_TRUE(ac.Cooldowns.IsOnCooldown(m_AbilityTag));
    EXPECT_NEAR(ac.Cooldowns.GetRemainingCooldown(m_AbilityTag), 0.5f, 1e-3f);

    // After ~0.25s of ticks, cooldown should be ~0.25s remaining.
    TickFor(0.25f);
    EXPECT_NEAR(ac.Cooldowns.GetRemainingCooldown(m_AbilityTag), 0.25f, 0.05f)
        << "cooldown didn't decrement at the rate of simulated time — "
           "Scene::OnUpdateRuntime isn't driving GameplayAbilitySystem::OnUpdate "
           "with the per-tick dt.";

    // After enough time to clear the cooldown, IsOnCooldown is false again.
    TickFor(0.4f);
    EXPECT_FALSE(ac.Cooldowns.IsOnCooldown(m_AbilityTag))
        << "cooldown didn't expire after the full duration of ticks elapsed — "
           "CooldownManager::Tick is failing to retire entries OR the seam "
           "between Scene tick and the system is broken.";

    // After cooldown clears, a second activation must succeed.
    const bool activatedAgain = GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Player, m_AbilityTag);
    EXPECT_TRUE(activatedAgain)
        << "second activation failed despite the cooldown having expired — "
           "Cooldowns map retains a stale entry past zero, or IsOnCooldown is "
           "reporting the wrong polarity.";
}
