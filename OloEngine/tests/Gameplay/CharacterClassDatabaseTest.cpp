// OLO_TEST_LAYER: unit
// =============================================================================
// CharacterClassDatabaseTest.cpp
//
// Unit tests for the CharacterClassDatabase asset (issue #635): the
// Validate() matrix (unique non-empty class ids, non-negative per-level
// point grants and LevelCap, non-empty attribute names), the
// FindClass/RebuildIndex contract, and the CharacterClassDatabaseSerializer
// string round-trip — attribute specs (base/growth/value-per-point floats),
// starting abilities, starting tags, asset handles as u64, per-level point
// grants and LevelCap — plus NaN/Inf sanitisation of hand-edited YAML.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"
#include "OloEngine/Memory/Platform.h" // OLO_ASAN_ENABLED

#include <string>

using namespace OloEngine;

namespace
{
    CharacterClassDefinition MakeClass(std::string id)
    {
        CharacterClassDefinition def;
        def.ClassID = std::move(id);
        return def;
    }
} // namespace

// ============================================================================
// Validate() matrix
// ============================================================================

TEST(CharacterClassDatabaseTest, ValidateAcceptsWellFormedClasses)
{
    CharacterClassDatabase db;
    auto warrior = MakeClass("warrior");
    warrior.Attributes.push_back({ "MaxHealth", 120.0f, 12.0f, 5.0f });
    db.m_Classes.push_back(std::move(warrior));
    db.m_Classes.push_back(MakeClass("mage"));

    std::string error;
    EXPECT_TRUE(db.Validate(&error)) << "two distinct well-formed classes must pass, got: " << error;
}

TEST(CharacterClassDatabaseTest, ValidateRejectsEmptyClassID)
{
    CharacterClassDatabase db;
    db.m_Classes.push_back(MakeClass(""));

    std::string error;
    EXPECT_FALSE(db.Validate(&error)) << "an empty ClassID must be rejected";
    EXPECT_NE(error.find("empty ClassID"), std::string::npos) << "got: " << error;
}

TEST(CharacterClassDatabaseTest, ValidateRejectsDuplicateClassID)
{
    CharacterClassDatabase db;
    db.m_Classes.push_back(MakeClass("warrior"));
    db.m_Classes.push_back(MakeClass("warrior"));

    std::string error;
    EXPECT_FALSE(db.Validate(&error)) << "duplicate ClassIDs must be rejected";
    EXPECT_NE(error.find("duplicate ClassID 'warrior'"), std::string::npos) << "got: " << error;
}

TEST(CharacterClassDatabaseTest, ValidateRejectsNegativePointGrants)
{
    {
        CharacterClassDatabase db;
        auto cls = MakeClass("warrior");
        cls.AttributePointsPerLevel = -1;
        db.m_Classes.push_back(std::move(cls));
        std::string error;
        EXPECT_FALSE(db.Validate(&error)) << "negative AttributePointsPerLevel must be rejected";
        EXPECT_NE(error.find("negative per-level point grants"), std::string::npos) << "got: " << error;
    }
    {
        CharacterClassDatabase db;
        auto cls = MakeClass("warrior");
        cls.SkillPointsPerLevel = -3;
        db.m_Classes.push_back(std::move(cls));
        std::string error;
        EXPECT_FALSE(db.Validate(&error)) << "negative SkillPointsPerLevel must be rejected";
    }
}

TEST(CharacterClassDatabaseTest, ValidateRejectsNegativeLevelCap)
{
    CharacterClassDatabase db;
    auto cls = MakeClass("warrior");
    cls.LevelCap = -10;
    db.m_Classes.push_back(std::move(cls));

    std::string error;
    EXPECT_FALSE(db.Validate(&error)) << "a negative LevelCap must be rejected (0 = uncapped)";
    EXPECT_NE(error.find("negative LevelCap"), std::string::npos) << "got: " << error;
}

TEST(CharacterClassDatabaseTest, ValidateRejectsEmptyAttributeName)
{
    CharacterClassDatabase db;
    auto cls = MakeClass("warrior");
    cls.Attributes.push_back({ "", 10.0f, 1.0f, 1.0f });
    db.m_Classes.push_back(std::move(cls));

    std::string error;
    EXPECT_FALSE(db.Validate(&error)) << "an attribute spec with an empty name must be rejected";
    EXPECT_NE(error.find("empty name"), std::string::npos) << "got: " << error;
}

// ============================================================================
// FindClass / RebuildIndex contract
// ============================================================================

TEST(CharacterClassDatabaseTest, FindClassRequiresRebuildIndexAfterMutation)
{
    CharacterClassDatabase db;
    db.m_Classes.push_back(MakeClass("warrior"));

    EXPECT_EQ(db.FindClass("warrior"), nullptr)
        << "FindClass must not see classes added without a RebuildIndex() call";

    db.RebuildIndex();
    const CharacterClassDefinition* found = db.FindClass("warrior");
    ASSERT_NE(found, nullptr) << "FindClass must find the class after RebuildIndex()";
    EXPECT_EQ(found->ClassID, "warrior");
    EXPECT_EQ(db.FindClass("paladin"), nullptr) << "an unknown id must return nullptr";
}

// ============================================================================
// Serializer round-trip
// ============================================================================

TEST(CharacterClassDatabaseTest, SerializerRoundTripPreservesEveryField)
{
    auto original = Ref<CharacterClassDatabase>::Create();
    {
        CharacterClassDefinition warrior = MakeClass("warrior");
        warrior.DisplayName = "Warrior";
        warrior.Description = "Front-line fighter.";
        warrior.AttributePointsPerLevel = 7;
        warrior.SkillPointsPerLevel = 2;
        warrior.LevelCap = 60;
        warrior.ExperienceCurve = AssetHandle(0xABCDEF0123456789ULL);
        warrior.SkillTrees = { AssetHandle(0x1122334455667788ULL), AssetHandle(55) };
        warrior.StartingTags = { "State.Alive", "Class.Warrior" };
        warrior.Attributes.push_back({ "MaxHealth", 120.5f, 12.25f, 5.5f });
        warrior.Attributes.push_back({ "AttackPower", 14.0f, 2.0f, 1.0f });

        GameplayAbilityDef bash;
        bash.Name = "Shield Bash";
        bash.AbilityTag = GameplayTag("Ability.ShieldBash");
        bash.CooldownDuration = 4.0f;
        bash.ResourceCost = 10.0f;
        bash.CostAttribute = "Mana";
        bash.RequiredTags.AddTag(GameplayTag("State.Alive"));
        warrior.StartingAbilities.push_back(std::move(bash));

        original->m_Classes.push_back(std::move(warrior));
        original->m_Classes.push_back(MakeClass("mage"));
        original->RebuildIndex();
    }

    CharacterClassDatabaseSerializer serializer;
    const std::string yaml = serializer.TestSerializeToYAML(original);
    ASSERT_FALSE(yaml.empty());

    auto reloaded = Ref<CharacterClassDatabase>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, reloaded))
        << "deserialize rejected the just-serialized valid database. YAML:\n"
        << yaml;

    ASSERT_EQ(reloaded->m_Classes.size(), 2u) << "class count changed in round-trip";

    // Deserialize must have rebuilt the index — FindClass works immediately.
    const CharacterClassDefinition* warrior = reloaded->FindClass("warrior");
    ASSERT_NE(warrior, nullptr) << "the serializer must RebuildIndex() on load";
    EXPECT_EQ(warrior->DisplayName, "Warrior");
    EXPECT_EQ(warrior->Description, "Front-line fighter.");
    EXPECT_EQ(warrior->AttributePointsPerLevel, 7) << "per-level attribute points dropped";
    EXPECT_EQ(warrior->SkillPointsPerLevel, 2) << "per-level skill points dropped";
    EXPECT_EQ(warrior->LevelCap, 60) << "LevelCap dropped";
    EXPECT_EQ(static_cast<u64>(warrior->ExperienceCurve), 0xABCDEF0123456789ULL)
        << "ExperienceCurve handle must round-trip as u64";
    ASSERT_EQ(warrior->SkillTrees.size(), 2u) << "SkillTrees handle list dropped";
    EXPECT_EQ(static_cast<u64>(warrior->SkillTrees[0]), 0x1122334455667788ULL);
    EXPECT_EQ(static_cast<u64>(warrior->SkillTrees[1]), 55u);
    ASSERT_EQ(warrior->StartingTags.size(), 2u) << "StartingTags dropped";
    EXPECT_EQ(warrior->StartingTags[0], "State.Alive");
    EXPECT_EQ(warrior->StartingTags[1], "Class.Warrior");

    ASSERT_EQ(warrior->Attributes.size(), 2u) << "attribute spec count changed";
    EXPECT_EQ(warrior->Attributes[0].Attribute, "MaxHealth");
    EXPECT_NEAR(warrior->Attributes[0].BaseValue, 120.5f, 1e-4f) << "BaseValue dropped";
    EXPECT_NEAR(warrior->Attributes[0].GrowthPerLevel, 12.25f, 1e-4f) << "GrowthPerLevel dropped";
    EXPECT_NEAR(warrior->Attributes[0].ValuePerPoint, 5.5f, 1e-4f) << "ValuePerPoint dropped";
    EXPECT_EQ(warrior->Attributes[1].Attribute, "AttackPower");

    ASSERT_EQ(warrior->StartingAbilities.size(), 1u) << "StartingAbilities dropped";
    const auto& bash = warrior->StartingAbilities[0];
    EXPECT_EQ(bash.Name, "Shield Bash");
    EXPECT_EQ(bash.AbilityTag.GetTagString(), "Ability.ShieldBash");
    EXPECT_NEAR(bash.CooldownDuration, 4.0f, 1e-4f);
    EXPECT_NEAR(bash.ResourceCost, 10.0f, 1e-4f);
    EXPECT_EQ(bash.CostAttribute, "Mana");
    EXPECT_TRUE(bash.RequiredTags.HasTagExact(GameplayTag("State.Alive")));

    EXPECT_NE(reloaded->FindClass("mage"), nullptr) << "second class dropped in round-trip";
}

TEST(CharacterClassDatabaseTest, SerializerSanitizesNaNInfAttributeFloats)
{
    // Hand-edited YAML: .nan / .inf parse cleanly, and a negative
    // ValuePerPoint would invert attribute-point spending.
    const std::string yaml =
        "CharacterClasses:\n"
        "  Classes:\n"
        "    - ClassID: test\n"
        "      Attributes:\n"
        "        - Attribute: MaxHealth\n"
        "          BaseValue: .nan\n"
        "          GrowthPerLevel: .inf\n"
        "          ValuePerPoint: -3\n";

    CharacterClassDatabaseSerializer serializer;
    auto db = Ref<CharacterClassDatabase>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, db))
        << "NaN/Inf-injected but structurally valid YAML must load with fallbacks, not reject";

    const CharacterClassDefinition* cls = db->FindClass("test");
    ASSERT_NE(cls, nullptr);
    ASSERT_EQ(cls->Attributes.size(), 1u);
    EXPECT_FLOAT_EQ(cls->Attributes[0].BaseValue, 0.0f) << "NaN BaseValue must fall back to 0";
    EXPECT_FLOAT_EQ(cls->Attributes[0].GrowthPerLevel, 0.0f) << ".inf GrowthPerLevel must fall back to 0";
    EXPECT_FLOAT_EQ(cls->Attributes[0].ValuePerPoint, 0.0f)
        << "negative ValuePerPoint must clamp to 0 (non-spendable), never stay negative";
}

TEST(CharacterClassDatabaseTest, SerializerSkipsAttributeSpecWithEmptyName)
{
    // The load side skips (rather than rejects) a nameless spec so one bad
    // row can't take down the whole database; Validate would reject it if it
    // were committed.
    const std::string yaml =
        "CharacterClasses:\n"
        "  Classes:\n"
        "    - ClassID: test\n"
        "      Attributes:\n"
        "        - Attribute: \"\"\n"
        "          BaseValue: 10\n"
        "        - Attribute: MaxHealth\n"
        "          BaseValue: 100\n";

    CharacterClassDatabaseSerializer serializer;
    auto db = Ref<CharacterClassDatabase>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, db));
    const CharacterClassDefinition* cls = db->FindClass("test");
    ASSERT_NE(cls, nullptr);
    ASSERT_EQ(cls->Attributes.size(), 1u) << "the nameless spec must be skipped, the valid one kept";
    EXPECT_EQ(cls->Attributes[0].Attribute, "MaxHealth");
}

TEST(CharacterClassDatabaseTest, SerializerRejectsEmptyAndDuplicateClassIDs)
{
    CharacterClassDatabaseSerializer serializer;

    {
        auto empty = Ref<CharacterClassDatabase>::Create();
        empty->m_Classes.push_back(MakeClass(""));
        auto scratch = Ref<CharacterClassDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(empty), scratch))
            << "an empty ClassID must reject the whole asset on load";
    }
    {
        auto dup = Ref<CharacterClassDatabase>::Create();
        dup->m_Classes.push_back(MakeClass("warrior"));
        dup->m_Classes.push_back(MakeClass("warrior"));
        auto scratch = Ref<CharacterClassDatabase>::Create();
        EXPECT_FALSE(serializer.TestDeserializeFromYAML(serializer.TestSerializeToYAML(dup), scratch))
            << "duplicate ClassIDs must reject the whole asset on load";
    }
}

TEST(CharacterClassDatabaseTest, SerializerRejectsMalformedYAML)
{
    CharacterClassDatabaseSerializer serializer;

    // The syntax-error branch is skipped under Windows ASan — clang-cl +
    // /fsanitize=address crashes inside the C++ exception-dispatch machinery
    // when yaml-cpp throws through certain instrumented frame shapes. Full
    // evidence trail in the identical guard in
    // ExperienceCurveTest::SerializerRejectsMalformedYAML; Windows-ASan
    // coverage of the yaml-cpp throw/catch plumbing continues via
    // EngineSubsystemSmoke.ProjectLoadMalformedYAMLFailsCleanly.
#if !(OLO_ASAN_ENABLED && defined(_WIN32))
    auto scratch = Ref<CharacterClassDatabase>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("key: [unclosed", scratch))
        << "a YAML syntax error must be rejected";
#endif

    auto scratch2 = Ref<CharacterClassDatabase>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("SomethingElse: 1\n", scratch2))
        << "well-formed YAML without the 'CharacterClasses' root key must be rejected";
}
