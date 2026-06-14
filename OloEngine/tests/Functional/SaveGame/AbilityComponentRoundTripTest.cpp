#include "OloEnginePCH.h"

// =============================================================================
// AbilityComponentRoundTripTest — Functional Test.
//
// Cross-subsystem seam under test:
//   AbilityComponent (Attributes + Abilities + OwnedTags) ×
//   SceneSerializer write × SceneSerializer read. The component carries
//   hand-rolled YAML serialization (no defaulted operator==, manual
//   field-by-field write/read) so adding or removing a field is high-risk:
//   a forgotten branch silently drops the value on reload. This test pins:
//     - DefineAttribute base values round-trip exactly,
//     - GameplayAbilityDef.Name, AbilityTag, CooldownDuration, IsChanneled
//       all survive,
//     - OwnedTags survive (we add a fixed marker so the read path is
//       observable).
//
// NOTE: This uses Scene's YAML serializer — the format for authored .olo
// scene files, where AbilityComponent MUST round-trip exactly or whole
// scenes lose data. The SaveGame binary path also carries AbilityComponent
// (since issue #325 wired it into the SAVE_COMPONENT/TRY_LOAD_COMPONENT
// lists); that seam is covered by RegisteredComponentsSurviveSaveLoadTest.
//
// Scenario: build a player entity, give it attributes (Health=73.5,
// Mana=12), one ability with a custom cooldown, and one owned tag.
// Serialize to YAML → deserialize into a fresh scene. Inspect the
// rehydrated values against the originals.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AbilityComponentRoundTripTest : public FunctionalTest
{
  protected:
    static constexpr f32 kCustomHealth = 73.5f;
    static constexpr f32 kCustomMana = 12.0f;
    static constexpr f32 kCooldown = 2.5f;
    static constexpr const char* kAbilityName = "Heal";
    GameplayTag m_AbilityTag{ "Ability.Heal" };
    GameplayTag m_OwnedTag{ "State.Alive" };

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Hero");
        auto& ac = m_Player.AddComponent<AbilityComponent>();
        ac.Attributes.DefineAttribute("Health", kCustomHealth);
        ac.Attributes.DefineAttribute("Mana", kCustomMana);
        ac.OwnedTags.AddTag(m_OwnedTag);

        GameplayAbilityDef def;
        def.Name = kAbilityName;
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = kCooldown;
        def.IsChanneled = false;
        def.IsToggled = false;
        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Player;
};

TEST_F(AbilityComponentRoundTripTest, AttributesAbilitiesAndTagsSurviveSceneYAMLRoundTrip)
{
    // SceneSerializer::SerializeToYAML emits the entire scene as a YAML string.
    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer restoreSerializer(restored);
    ASSERT_TRUE(restoreSerializer.DeserializeFromYAML(yaml))
        << "DeserializeFromYAML failed — Scene YAML round-trip is fundamentally broken.";

    Entity restoredPlayer = restored->FindEntityByName("Hero");
    ASSERT_TRUE(restoredPlayer);
    ASSERT_TRUE(restoredPlayer.HasComponent<AbilityComponent>())
        << "AbilityComponent dropped on restore — Scene YAML deserializer doesn't claim "
           "the AbilityComponent node, or the writer didn't emit it.";

    const auto& ac = restoredPlayer.GetComponent<AbilityComponent>();

    // Attributes: HasAttribute and GetBaseValue must match.
    ASSERT_TRUE(ac.Attributes.HasAttribute("Health"));
    ASSERT_TRUE(ac.Attributes.HasAttribute("Mana"));
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Health"), kCustomHealth, 1e-4f)
        << "Health attribute didn't survive round-trip.";
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Mana"), kCustomMana, 1e-4f)
        << "Mana attribute didn't survive round-trip.";

    // OwnedTags: the marker tag must still be present.
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_OwnedTag))
        << "OwnedTags lost the State.Alive marker; "
           "GameplayTagContainer serializer dropped the tag list.";

    // Abilities: at least one ability with matching name + cooldown.
    ASSERT_EQ(ac.Abilities.size(), 1u)
        << "Ability count mismatch after restore (got " << ac.Abilities.size() << ", expected 1).";
    const auto& restoredDef = ac.Abilities.front().Definition;
    EXPECT_EQ(restoredDef.Name, std::string(kAbilityName));
    EXPECT_TRUE(restoredDef.AbilityTag.MatchesExact(m_AbilityTag))
        << "AbilityTag didn't survive round-trip — GameplayTag string round-trip is broken.";
    EXPECT_NEAR(restoredDef.CooldownDuration, kCooldown, 1e-4f)
        << "CooldownDuration didn't survive — a numeric write was forgotten in the writer.";
    EXPECT_EQ(restoredDef.IsChanneled, false)
        << "IsChanneled flag didn't survive round-trip — bool was dropped in writer or reader.";
    EXPECT_EQ(restoredDef.IsToggled, false)
        << "IsToggled flag didn't survive round-trip — bool was dropped in writer or reader.";
}
