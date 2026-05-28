#include "OloEnginePCH.h"

// =============================================================================
// ToggledAbilityReactivateCancelsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility (already-active path) ×
//   GameplayAbilityDef.IsToggled × CancelAbility × OwnedTags. A toggled
//   ability (`IsToggled=true`) flips between active and inactive on
//   each activation call. The implementation lives in two branches of
//   TryActivateAbility:
//     - If already active and toggled → CancelAbility, return true.
//     - If already active and NOT toggled → return false (no-op).
//   A regression in the IsToggled branch makes toggle-keys behave like
//   one-shot abilities (auras you can't turn off).
//
// Scenario: ability `Aura` with `IsToggled=true` grants `State.AuraActive`
// on activation. First activation: active + tag granted. Second
// activation: NOT a fresh start — must cancel (active=false + tag gone).
// Third activation (after cancel): active again (toggle back on).
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

class ToggledAbilityReactivateCancelsTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.Aura" };
    GameplayTag m_AuraTag{ "State.AuraActive" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Caster");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();

        GameplayAbilityDef def;
        def.Name = "Aura";
        def.AbilityTag = m_AbilityTag;
        def.IsToggled = true;
        def.IsChanneled = false;
        def.CooldownDuration = 0.0f;
        def.ResourceCost = 0.0f;
        def.ActivationGrantedTags.AddTag(m_AuraTag);

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(ToggledAbilityReactivateCancelsTest, SuccessiveActivationsToggleActiveStateAndGrantedTag)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    const auto& ability = ac.Abilities.front();

    ASSERT_FALSE(ability.IsActive);
    ASSERT_FALSE(ac.OwnedTags.HasTagExact(m_AuraTag));

    // First activation: toggle ON.
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));
    EXPECT_TRUE(ability.IsActive);
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_AuraTag))
        << "first toggled activation didn't grant the activation tag — "
           "ActivationGrantedTags loop didn't fire on toggle-on.";

    // Second activation: must CANCEL (return true, since the toggle attempt
    // was valid even though it deactivated). Active flips to false and the
    // granted tag is gone.
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag))
        << "second activation on a toggled+active ability returned false — "
           "the early-out is treating the second call as a duplicate request "
           "instead of a cancel.";
    EXPECT_FALSE(ability.IsActive)
        << "second activation kept IsActive=true — the toggled branch in "
           "TryActivateAbility didn't route to CancelAbility, or CancelAbility "
           "is a no-op.";
    EXPECT_FALSE(ac.OwnedTags.HasTagExact(m_AuraTag))
        << "tag wasn't removed when toggle cancelled — CancelAbility's "
           "RemoveTag loop is missing or operates on a different container.";

    // Third activation: toggle back ON.
    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));
    EXPECT_TRUE(ability.IsActive);
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_AuraTag));
}
