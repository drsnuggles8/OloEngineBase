#include "OloEnginePCH.h"

// =============================================================================
// ChanneledAbilityAutoDeactivatesAtEndOfDurationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility × Scene tick ×
//   GameplayAbilitySystem::OnUpdate (channel countdown branch) ×
//   AbilityComponent.OwnedTags (activation-granted tags). Channeled
//   abilities (`IsChanneled=true`, `ChannelDuration > 0`) stay active
//   until either CancelAbility is called or ChannelRemaining hits zero.
//   On expiry the OnUpdate path must:
//     - flip ActiveAbility.IsActive = false
//     - remove every tag listed in Definition.ActivationGrantedTags
//   A regression in that block traps the caster in the channeling state
//   forever (and the granted tag, e.g. State.Channeling, never clears).
//
// Scenario: an ability with ChannelDuration=0.4s grants State.Channeling
// on activation. After activation:
//   - IsActive=true, OwnedTags has State.Channeling.
// After ticking 0.5s:
//   - IsActive=false, OwnedTags no longer has State.Channeling.
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

class ChanneledAbilityAutoDeactivatesAtEndOfDurationTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.ChannelBeam" };
    GameplayTag m_ChannelingTag{ "State.Channeling" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Caster");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();

        GameplayAbilityDef def;
        def.Name = "ChannelBeam";
        def.AbilityTag = m_AbilityTag;
        def.IsChanneled = true;
        def.ChannelDuration = 0.4f;
        def.CooldownDuration = 0.0f;
        def.ResourceCost = 0.0f;
        def.ActivationGrantedTags.AddTag(m_ChannelingTag);

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(ChanneledAbilityAutoDeactivatesAtEndOfDurationTest, ChannelRemainingHitsZeroAndGrantedTagIsRemoved)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    ASSERT_FALSE(ac.OwnedTags.HasTagExact(m_ChannelingTag));

    ASSERT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));

    // Right after activation: active, channel timer set, tag granted.
    ASSERT_FALSE(ac.Abilities.empty());
    auto& ability = ac.Abilities.front();
    EXPECT_TRUE(ability.IsActive)
        << "channeled ability didn't enter the active state immediately on activation.";
    EXPECT_NEAR(ability.ChannelRemaining, 0.4f, 1e-3f);
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_ChannelingTag))
        << "ActivationGrantedTags didn't appear on the owner — TryActivateAbility's "
           "tag-grant loop didn't run.";

    // Mid-channel: still active, tag still on.
    TickFor(0.2f);
    EXPECT_TRUE(ability.IsActive);
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_ChannelingTag));
    EXPECT_LT(ability.ChannelRemaining, 0.4f)
        << "ChannelRemaining didn't decrement during ticks — OnUpdate isn't "
           "running the channel countdown branch.";

    // Past the duration: auto-cancel must have fired.
    TickFor(0.3f);
    EXPECT_FALSE(ability.IsActive)
        << "ChannelRemaining went past zero but the ability is still active — "
           "the `if (ChannelRemaining <= 0) IsActive = false` branch was skipped.";
    EXPECT_FALSE(ac.OwnedTags.HasTagExact(m_ChannelingTag))
        << "channel ended but State.Channeling is still on the owner — "
           "the granted-tags removal loop in the channel-end block regressed.";
}
