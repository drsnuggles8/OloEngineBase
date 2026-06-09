// =============================================================================
// LocalizationTest -- contracts of the localization foundation.
//
// Covers StringTable YAML loading, TextFormatter substitution + plural-form
// resolution, and LocalizationManager locale switching / event firing /
// missing-key fallback.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/UTF8.h"
#include "OloEngine/Localization/LocaleDefinition.h"
#include "OloEngine/Localization/LocalizationCsv.h"
#include "OloEngine/Localization/LocalizationEvents.h"
#include "OloEngine/Localization/LocalizationLint.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/LocalizationSystem.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Localization/StringTable.h"
#include "OloEngine/Localization/TextFormatter.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace OloEngine;

namespace
{
    constexpr const char* kEnglishYAML = R"(
locale: en
name: English
direction: ltr
strings:
  ui.main_menu.play: "Play"
  ui.main_menu.settings: "Settings"
  combat.damage_dealt: "You dealt {damage} damage to {target}."
  combat.kills: "You have slain {count} {count:enemy|enemies}."
)";

    constexpr const char* kGermanYAML = R"(
locale: de
name: Deutsch
direction: ltr
strings:
  ui.main_menu.play: "Spielen"
  combat.kills: "Du hast {count} {count:Feind|Feinde} besiegt."
)";

    constexpr const char* kJapaneseYAML = R"(
locale: ja
name: Japanese
direction: ltr
plural_rule: other-only
strings:
  combat.kills: "{count}人の敵を倒した。"
)";

    class LocalizationFixture : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            LocalizationManager::ResetForTesting();
        }

        void TearDown() override
        {
            LocalizationManager::ResetForTesting();
        }
    };
} // namespace

// -----------------------------------------------------------------------------
// StringTable
// -----------------------------------------------------------------------------

TEST(StringTable, LoadFromYAMLPopulatesKeysAndMetadata)
{
    StringTable table;
    ASSERT_TRUE(table.LoadFromYAMLString(kEnglishYAML));
    EXPECT_EQ(table.GetLocaleInfo().Code, "en");
    EXPECT_EQ(table.GetLocaleInfo().Name, "English");
    EXPECT_EQ(table.GetLocaleInfo().Direction, TextDirection::LTR);
    EXPECT_EQ(table.Size(), 4u);
    EXPECT_TRUE(table.Has("ui.main_menu.play"));
    EXPECT_EQ(table.Get("ui.main_menu.play"), "Play");
    EXPECT_FALSE(table.Has("missing.key"));
    EXPECT_TRUE(table.Get("missing.key").empty());
}

TEST(StringTable, MalformedYAMLLeavesPriorContentsUntouched)
{
    StringTable table;
    ASSERT_TRUE(table.LoadFromYAMLString(kEnglishYAML));
    const auto sizeBefore = table.Size();

    EXPECT_FALSE(table.LoadFromYAMLString("locale: en\nstrings: 'not a map'"));
    EXPECT_EQ(table.Size(), sizeBefore);
    EXPECT_EQ(table.Get("ui.main_menu.play"), "Play");
}

TEST(StringTable, OtherOnlyPluralRuleParses)
{
    StringTable table;
    ASSERT_TRUE(table.LoadFromYAMLString(kJapaneseYAML));
    EXPECT_EQ(table.GetLocaleInfo().Plural, PluralRule::OtherOnly);
}

// -----------------------------------------------------------------------------
// TextFormatter
// -----------------------------------------------------------------------------

TEST(TextFormatter, NamedParameterSubstitution)
{
    const std::string s = TextFormatter::Format(
        "You dealt {damage} damage to {target}.",
        { { "damage", "42" }, { "target", "Wolf" } });
    EXPECT_EQ(s, "You dealt 42 damage to Wolf.");
}

TEST(TextFormatter, MissingParameterLeavesTokenLiteral)
{
    const std::string s = TextFormatter::Format("Hello {who}!", {});
    EXPECT_EQ(s, "Hello {who}!");
}

TEST(TextFormatter, DoubleBracesEmitLiteralBraces)
{
    const std::string s = TextFormatter::Format("{{not a token}} but {real}", { { "real", "yes" } });
    EXPECT_EQ(s, "{not a token} but yes");
}

TEST(TextFormatter, PluralOneOtherSelectsByCount)
{
    const std::string pattern = "You have slain {count} {count:enemy|enemies}.";
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 0, {}), "You have slain 0 enemies.");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}), "You have slain 1 enemy.");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 2, {}), "You have slain 2 enemies.");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", -1, {}), "You have slain -1 enemy.");
}

TEST(TextFormatter, PluralFrenchLikeCollapsesZeroAndOne)
{
    const std::string pattern = "{count:singular|plural}";
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 0, {}, PluralRule::FrenchLike), "singular");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}, PluralRule::FrenchLike), "singular");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 2, {}, PluralRule::FrenchLike), "plural");
}

TEST(TextFormatter, PluralPolishLikePicksThreeForms)
{
    // Polish: one (1), few (2..4 mod 100 not in 12..14), many (everything else).
    const std::string pattern = "{count:1|2-4|5+}";
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}, PluralRule::PolishLike), "1");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 2, {}, PluralRule::PolishLike), "2-4");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 3, {}, PluralRule::PolishLike), "2-4");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 4, {}, PluralRule::PolishLike), "2-4");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 5, {}, PluralRule::PolishLike), "5+");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 12, {}, PluralRule::PolishLike), "5+"); // 12 is teen
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 22, {}, PluralRule::PolishLike), "2-4");
}

TEST(TextFormatter, PluralRussianLikePicksFourForms)
{
    // Russian: one (mod10==1 && mod100!=11), few (mod10 2..4 && mod100 ∉ 12..14),
    // many (mod10 ∈ {0, 5..9} || mod100 ∈ 11..14), other = leftover.
    const std::string pattern = "{count:one|few|many|other}";
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}, PluralRule::RussianLike), "one");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 21, {}, PluralRule::RussianLike), "one");  // mod10==1
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 11, {}, PluralRule::RussianLike), "many"); // mod100==11 is teen
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 2, {}, PluralRule::RussianLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 3, {}, PluralRule::RussianLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 4, {}, PluralRule::RussianLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 22, {}, PluralRule::RussianLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 5, {}, PluralRule::RussianLike), "many");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 0, {}, PluralRule::RussianLike), "many");
}

TEST(TextFormatter, PluralArabicLikePicksSixForms)
{
    const std::string pattern = "{count:zero|one|two|few|many|other}";
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 0, {}, PluralRule::ArabicLike), "zero");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}, PluralRule::ArabicLike), "one");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 2, {}, PluralRule::ArabicLike), "two");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 3, {}, PluralRule::ArabicLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 10, {}, PluralRule::ArabicLike), "few");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 11, {}, PluralRule::ArabicLike), "many");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 99, {}, PluralRule::ArabicLike), "many");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 100, {}, PluralRule::ArabicLike), "other");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 102, {}, PluralRule::ArabicLike), "other");
}

TEST(TextFormatter, PluralOtherOnlyAlwaysPicksFirstForm)
{
    const std::string pattern = "{count} {count:item|items}";
    // OtherOnly maps every count to index 0 — caller is expected to author
    // only one form, but if they provide multiples we just pick the first.
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 1, {}, PluralRule::OtherOnly), "1 item");
    EXPECT_EQ(TextFormatter::FormatPlural(pattern, "count", 5, {}, PluralRule::OtherOnly), "5 item");
}

TEST(TextFormatter, NonIntegerCountFallsBackToTokenLiteral)
{
    // params["count"] is not an int — token is left visible so the bug is loud.
    const std::string s = TextFormatter::Format("{count:a|b}", { { "count", "abc" } });
    EXPECT_EQ(s, "{count:a|b}");
}

TEST(TextFormatter, UnclosedBraceLeavesRemainderLiteral)
{
    const std::string s = TextFormatter::Format("Hello {unclosed", { { "unclosed", "should-not-substitute" } });
    EXPECT_EQ(s, "Hello {unclosed");
}

// -----------------------------------------------------------------------------
// LocalizationManager
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, GetReturnsFallbackBeforeAnyLocaleIsLoaded)
{
    EXPECT_EQ(LocalizationManager::Get("anything"), "???");
}

TEST_F(LocalizationFixture, LoadLocaleAndLookupKey)
{
    auto tempPath = std::filesystem::temp_directory_path() / "olo_en_test.ololocale";
    {
        std::ofstream out(tempPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(tempPath));
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "en");
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play"), "Play");
    EXPECT_EQ(LocalizationManager::Get("missing.key"), "???");
    EXPECT_TRUE(LocalizationManager::HasKey("ui.main_menu.play"));
    EXPECT_FALSE(LocalizationManager::HasKey("missing.key"));

    std::filesystem::remove(tempPath);
}

TEST_F(LocalizationFixture, FormatUsesActiveLocaleTemplate)
{
    // Direct in-memory injection — avoids touching the filesystem for this case.
    StringTable enTable;
    ASSERT_TRUE(enTable.LoadFromYAMLString(kEnglishYAML));
    auto tempPath = std::filesystem::temp_directory_path() / "olo_en_format.ololocale";
    {
        std::ofstream out(tempPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(tempPath));

    EXPECT_EQ(
        LocalizationManager::Format("combat.damage_dealt", { { "damage", "42" }, { "target", "Wolf" } }),
        "You dealt 42 damage to Wolf.");

    EXPECT_EQ(LocalizationManager::FormatPlural("combat.kills", "count", 1), "You have slain 1 enemy.");
    EXPECT_EQ(LocalizationManager::FormatPlural("combat.kills", "count", 5), "You have slain 5 enemies.");

    std::filesystem::remove(tempPath);
}

TEST_F(LocalizationFixture, SetCurrentLocaleSwitchesActiveTableAndFiresEvent)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_switch.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_switch.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }

    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    std::atomic<int> eventCount{ 0 };
    std::string capturedPrev;
    std::string capturedNew;
    const u64 subId = LocalizationManager::Subscribe(
        [&](const LocaleChangedEvent& evt)
        {
            eventCount.fetch_add(1);
            capturedPrev = evt.GetPreviousLocale();
            capturedNew = evt.GetNewLocale();
        });

    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "de");
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play"), "Spielen");
    EXPECT_EQ(eventCount.load(), 1);
    EXPECT_EQ(capturedPrev, "en");
    EXPECT_EQ(capturedNew, "de");

    // Plural-form lookup uses German "one/other" semantics (same as English).
    EXPECT_EQ(LocalizationManager::FormatPlural("combat.kills", "count", 1), "Du hast 1 Feind besiegt.");
    EXPECT_EQ(LocalizationManager::FormatPlural("combat.kills", "count", 3), "Du hast 3 Feinde besiegt.");

    // Re-setting the same locale should be a no-op (no event).
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));
    EXPECT_EQ(eventCount.load(), 1);

    LocalizationManager::Unsubscribe(subId);

    // After unsubscribe, switching should not invoke our listener again.
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("en"));
    EXPECT_EQ(eventCount.load(), 1);

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, SetCurrentLocaleFailsForUnknownCode)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_unknown.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    EXPECT_FALSE(LocalizationManager::SetCurrentLocale("zz"));
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "en");
    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, InitializeScansDirectory)
{
    auto dir = std::filesystem::temp_directory_path() / "olo_loc_dir";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "en.ololocale");
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dir / "de.ololocale");
        out << kGermanYAML;
    }
    {
        // A non-.ololocale file should be ignored.
        std::ofstream out(dir / "readme.txt");
        out << "not a locale file";
    }

    LocalizationManager::Initialize(dir);
    const auto locales = LocalizationManager::GetAvailableLocales();
    ASSERT_EQ(locales.size(), 2u);
    EXPECT_EQ(locales[0].Code, "de");
    EXPECT_EQ(locales[1].Code, "en");

    // Active locale defaults to alphabetically first.
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "de");

    std::filesystem::remove_all(dir);
}

TEST_F(LocalizationFixture, ReloadCurrentLocaleRereadsFile)
{
    auto path = std::filesystem::temp_directory_path() / "olo_en_reload.ololocale";
    {
        std::ofstream out(path);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(path));
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play"), "Play");

    // Rewrite the file with a tweaked translation.
    {
        std::ofstream out(path);
        out << R"(
locale: en
name: English
strings:
  ui.main_menu.play: "PLAY!"
)";
    }
    ASSERT_TRUE(LocalizationManager::ReloadCurrentLocale());
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play"), "PLAY!");

    std::filesystem::remove(path);
}

TEST_F(LocalizationFixture, MissingKeyFallbackIsConfigurable)
{
    auto path = std::filesystem::temp_directory_path() / "olo_en_fallback.ololocale";
    {
        std::ofstream out(path);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(path));

    LocalizationManager::SetMissingKeyFallback("[missing]");
    EXPECT_EQ(LocalizationManager::Get("does.not.exist"), "[missing]");
    EXPECT_EQ(LocalizationManager::Format("does.not.exist", {}), "[missing]");

    std::filesystem::remove(path);
}

TEST_F(LocalizationFixture, ConcurrentGetIsSafeAcrossLocaleSwitch)
{
    // Smoke-test concurrent reads racing against a writer. Validates that
    // returning std::string-by-value rather than const-ref from Get() is
    // sound — refs would dangle the moment the writer swaps tables.
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_race.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_race.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    std::atomic<bool> stop{ false };
    std::atomic<u64> readCount{ 0 };
    constexpr int kReaderCount = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);
    for (int i = 0; i < kReaderCount; ++i)
    {
        readers.emplace_back(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    const std::string s = LocalizationManager::Get("ui.main_menu.play");
                    // Both translations are valid; "???" only if a window
                    // between switches left no active locale, which shouldn't
                    // happen with our lock discipline.
                    EXPECT_TRUE(s == "Play" || s == "Spielen") << "got: " << s;
                    readCount.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    for (int i = 0; i < 50; ++i)
    {
        LocalizationManager::SetCurrentLocale((i % 2 == 0) ? "en" : "de");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers)
        t.join();

    EXPECT_GT(readCount.load(), 0u);

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

// -----------------------------------------------------------------------------
// Per-locale accessors (Get/HasKey/GetAllKeys with explicit localeCode)
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, PerLocaleAccessorsBypassActiveLocale)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_perloc.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_perloc.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("en"));

    // German lookup works while English is active.
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "de"), "Spielen");
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "en"), "Play");
    EXPECT_TRUE(LocalizationManager::HasKey("ui.main_menu.play", "de"));
    EXPECT_TRUE(LocalizationManager::HasKey("ui.main_menu.settings", "en"));
    EXPECT_FALSE(LocalizationManager::HasKey("ui.main_menu.settings", "de")); // not in German YAML

    // Unknown locale → empty key list + fallback.
    EXPECT_TRUE(LocalizationManager::GetAllKeys("zz").empty());
    EXPECT_EQ(LocalizationManager::Get("anything", "zz"), "???");

    // GetAllKeys per-locale matches what's actually in each YAML.
    const auto enKeys = LocalizationManager::GetAllKeys("en");
    EXPECT_GT(enKeys.size(), LocalizationManager::GetAllKeys("de").size())
        << "English sample has more keys than German sample (missing translations).";

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

// -----------------------------------------------------------------------------
// Generation counter
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, GenerationIncrementsOnLocaleLoad)
{
    const u64 before = LocalizationManager::GetGeneration();

    auto path = std::filesystem::temp_directory_path() / "olo_en_gen.ololocale";
    {
        std::ofstream out(path);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(path));
    EXPECT_GT(LocalizationManager::GetGeneration(), before);

    std::filesystem::remove(path);
}

TEST_F(LocalizationFixture, GenerationIncrementsOnLocaleSwitch)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_gen_switch.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_gen_switch.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    const u64 before = LocalizationManager::GetGeneration();
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));
    EXPECT_GT(LocalizationManager::GetGeneration(), before);

    // Re-setting the same locale shouldn't bump (no-op early-out).
    const u64 stable = LocalizationManager::GetGeneration();
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));
    EXPECT_EQ(LocalizationManager::GetGeneration(), stable);

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, GenerationIncrementsOnReloadCurrentLocale)
{
    auto path = std::filesystem::temp_directory_path() / "olo_en_gen_reload.ololocale";
    {
        std::ofstream out(path);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(path));

    const u64 before = LocalizationManager::GetGeneration();
    ASSERT_TRUE(LocalizationManager::ReloadCurrentLocale());
    EXPECT_GT(LocalizationManager::GetGeneration(), before);

    std::filesystem::remove(path);
}

// -----------------------------------------------------------------------------
// LocalizationSystem
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, SystemUpdatesTextComponentOnLocaleSwitch)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_sys.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_sys.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("en"));

    auto scene = Scene::Create();
    Entity e = scene->CreateEntity("MainMenuButton");
    e.AddComponent<TextComponent>();
    e.AddComponent<LocalizedTextComponent>("ui.main_menu.play");

    // First call refreshes because m_LocalizationGeneration == 0 < current.
    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 1u);
    EXPECT_EQ(e.GetComponent<TextComponent>().TextString, "Play");

    // Second call short-circuits (no generation bump).
    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 0u);

    // Switching locale bumps generation; next system call refreshes.
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));
    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 1u);
    EXPECT_EQ(e.GetComponent<TextComponent>().TextString, "Spielen");

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, SystemSkipsEntitiesWithEmptyKey)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_sys_empty.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    auto scene = Scene::Create();
    Entity e = scene->CreateEntity("Untagged");
    auto& tc = e.AddComponent<TextComponent>();
    tc.TextString = "preset";
    e.AddComponent<LocalizedTextComponent>(); // empty key

    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 0u);
    // Pre-existing text should be untouched.
    EXPECT_EQ(e.GetComponent<TextComponent>().TextString, "preset");

    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, SystemRefreshesNewlyAddedComponent)
{
    // OnComponentAdded<LocalizedTextComponent> resets the scene's cached
    // generation, so adding a component AFTER the system has already
    // synced should still pick up the active locale on the next call.
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_sys_add.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    auto scene = Scene::Create();
    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 0u); // empty scene, but sync occurs

    Entity e = scene->CreateEntity("LateAdd");
    e.AddComponent<TextComponent>();
    e.AddComponent<LocalizedTextComponent>("ui.main_menu.settings");

    EXPECT_EQ(LocalizationSystem::UpdateLocalizedText(*scene), 1u);
    EXPECT_EQ(e.GetComponent<TextComponent>().TextString, "Settings");

    std::filesystem::remove(enPath);
}

// -----------------------------------------------------------------------------
// CSV round-trip — translator-facing export/import path.
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, CsvRoundTripPreservesEveryKey)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_csv.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_csv.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    auto csvPath = std::filesystem::temp_directory_path() / "olo_loc_roundtrip.csv";
    ASSERT_TRUE(LocalizationCsv::ExportToCsv(csvPath));

    // Mutate the active table so we can prove import overwrote it.
    ASSERT_TRUE(LocalizationManager::SetKey("en", "ui.main_menu.play", "<mutated>"));
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "en"), "<mutated>");

    auto result = LocalizationCsv::ImportFromCsv(csvPath);
    EXPECT_TRUE(result.Warnings.empty()) << "unexpected warnings during round-trip import";
    EXPECT_GE(result.RowsImported, 1u);
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "en"), "Play");
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "de"), "Spielen");

    std::filesystem::remove(csvPath);
    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, CsvQuotingHandlesCommasAndQuotesAndNewlines)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_quoting.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    // Inject tricky values that exercise every quoting rule.
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.commas", "a, b, c"));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.quotes", "he said \"hi\""));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.newlines", "line1\nline2"));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.crlf", "line1\r\nline2"));

    auto csvPath = std::filesystem::temp_directory_path() / "olo_loc_quoting.csv";
    ASSERT_TRUE(LocalizationCsv::ExportToCsv(csvPath));

    // Mutate, then re-import — the special-character cells should survive verbatim.
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.commas", "<scrub>"));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.quotes", "<scrub>"));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.newlines", "<scrub>"));
    ASSERT_TRUE(LocalizationManager::SetKey("en", "test.crlf", "<scrub>"));
    auto result = LocalizationCsv::ImportFromCsv(csvPath);
    EXPECT_TRUE(result.Warnings.empty());

    EXPECT_EQ(LocalizationManager::Get("test.commas", "en"), "a, b, c");
    EXPECT_EQ(LocalizationManager::Get("test.quotes", "en"), "he said \"hi\"");
    EXPECT_EQ(LocalizationManager::Get("test.newlines", "en"), "line1\nline2");
    // The export normalises CRLF inside cells via the codec's literal byte
    // path; the importer hands us back exactly what was written. Both halves
    // of the round-trip preserve the embedded bytes.
    EXPECT_EQ(LocalizationManager::Get("test.crlf", "en"), "line1\r\nline2");

    std::filesystem::remove(csvPath);
    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, CsvImportWarnsOnUnknownLocaleColumn)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_unk.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    // Hand-craft a CSV whose `xx` column is never going to be loaded.
    auto csvPath = std::filesystem::temp_directory_path() / "olo_loc_unknown_locale.csv";
    {
        std::ofstream out(csvPath, std::ios::binary);
        out << "key,en,xx\r\n";
        out << "ui.main_menu.play,Play,FOREIGN\r\n";
    }

    auto result = LocalizationCsv::ImportFromCsv(csvPath);
    EXPECT_FALSE(result.Warnings.empty()) << "missing warning for unknown 'xx' locale";
    EXPECT_EQ(result.LocalesUpdated, 1u); // only 'en' was actually written
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "en"), "Play");

    std::filesystem::remove(csvPath);
    std::filesystem::remove(enPath);
}

// -----------------------------------------------------------------------------
// ResolveLocalizedText: @key:-prefix dispatch helper used by Quest / Item /
// dialogue choice ports — anywhere a single string field hosts either a
// literal or a translation key.
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, ResolveLocalizedTextDispatchesByPrefix)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_resolve.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    // Literal strings pass through unchanged.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedText("Iron Sword"), "Iron Sword");
    EXPECT_EQ(LocalizationManager::ResolveLocalizedText(""), "");

    // Prefixed strings get looked up.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedText("@key:ui.main_menu.play"), "Play");

    // Prefix without a body (just "@key:") is treated as literal — no key to look up.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedText("@key:"), "@key:");

    // Missing-key fallback applies inside the resolver too.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedText("@key:does.not.exist"), "???");

    std::filesystem::remove(enPath);
}

// -----------------------------------------------------------------------------
// Phase 5 — pseudo-loc / missing-key reporting / number formatting /
// negotiation / OS detect / persistence / dangling-param lint / save-to-disk.
// -----------------------------------------------------------------------------

TEST_F(LocalizationFixture, PseudoLocaleWrapsAndDecoratesValues)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_pseudo.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    ASSERT_TRUE(LocalizationManager::GeneratePseudoLocale("en"));
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("pseudo"));
    const std::string play = LocalizationManager::Get("ui.main_menu.play");
    EXPECT_TRUE(play.starts_with("[!! ")) << "pseudo prefix missing: '" << play << "'";
    EXPECT_TRUE(play.ends_with(" !!]")) << "pseudo suffix missing: '" << play << "'";
    // Parameter tokens must be preserved verbatim (PseudoifyAscii skips inside { ... }).
    const std::string dmg = LocalizationManager::Get("combat.damage_dealt");
    EXPECT_NE(dmg.find("{damage}"), std::string::npos);
    EXPECT_NE(dmg.find("{target}"), std::string::npos);

    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, MissingKeyReportingAccumulatesAcrossLookups)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_missing.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    EXPECT_TRUE(LocalizationManager::GetMissingKeysSnapshot().empty());

    (void)LocalizationManager::Get("nope.one");
    (void)LocalizationManager::Get("nope.two");
    (void)LocalizationManager::Get("nope.one"); // duplicate — should still show once
    (void)LocalizationManager::Format("nope.three", {});

    const auto missing = LocalizationManager::GetMissingKeysSnapshot();
    EXPECT_EQ(missing.size(), 3u);
    EXPECT_TRUE(std::find(missing.begin(), missing.end(), "nope.one") != missing.end());
    EXPECT_TRUE(std::find(missing.begin(), missing.end(), "nope.two") != missing.end());
    EXPECT_TRUE(std::find(missing.begin(), missing.end(), "nope.three") != missing.end());

    LocalizationManager::ClearMissingKeys();
    EXPECT_TRUE(LocalizationManager::GetMissingKeysSnapshot().empty());

    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, FormatNumberUsesLocaleSeparators)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_num.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_num.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        // German de.ololocale-style overrides — thousand="." decimal=","
        std::ofstream out(dePath);
        out << "locale: de\nname: Deutsch\nthousand_separator: \".\"\ndecimal_separator: \",\"\nstrings: { dummy: \"x\" }\n";
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("en"));

    EXPECT_EQ(LocalizationManager::FormatNumber(i64{ 1234567 }), "1,234,567");
    EXPECT_EQ(LocalizationManager::FormatNumber(i64{ -1234567 }), "-1,234,567");
    EXPECT_EQ(LocalizationManager::FormatNumber(i64{ 0 }), "0");
    EXPECT_EQ(LocalizationManager::FormatNumber(1234.5, 2), "1,234.50");

    // German formatting via explicit locale-code argument (active locale stays en).
    EXPECT_EQ(LocalizationManager::FormatNumber(i64{ 1234567 }, "de"), "1.234.567");
    EXPECT_EQ(LocalizationManager::FormatNumber(1234.5, 2, "de"), "1.234,50");

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, NegotiateLocaleFallsBackThroughLanguageTag)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_neg.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_neg.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    // Exact match: de → de.
    EXPECT_EQ(LocalizationManager::NegotiateLocale({ "de" }), "de");
    // Language-only fallback: de-AT → de.
    EXPECT_EQ(LocalizationManager::NegotiateLocale({ "de-AT" }), "de");
    // Reverse: preference "fr" is not loaded, no language match either.
    EXPECT_TRUE(LocalizationManager::NegotiateLocale({ "fr" }).empty());
    // Cascading list — first hit wins.
    EXPECT_EQ(LocalizationManager::NegotiateLocale({ "fr", "es", "de-AT" }), "de");

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, ActiveLocalePersistsAcrossSaveLoad)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_persist.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_persist.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << kGermanYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("de"));

    auto prefPath = std::filesystem::temp_directory_path() / "olo_locale_pref.yaml";
    ASSERT_TRUE(LocalizationManager::SaveActiveLocaleToFile(prefPath));

    // Switch to en, then load the saved pref — should snap back to de.
    ASSERT_TRUE(LocalizationManager::SetCurrentLocale("en"));
    ASSERT_TRUE(LocalizationManager::LoadActiveLocaleFromFile(prefPath));
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "de");

    // Pref file referencing an unloaded locale: silently no-ops.
    {
        std::ofstream out(prefPath);
        out << "locale: zz\n";
    }
    EXPECT_FALSE(LocalizationManager::LoadActiveLocaleFromFile(prefPath));
    EXPECT_EQ(LocalizationManager::GetCurrentLocale(), "de");

    std::filesystem::remove(prefPath);
    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, LintCatchesMissingAndExtraParameters)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_lint.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_lint.ololocale";
    {
        std::ofstream out(enPath);
        out << "locale: en\nstrings:\n";
        out << "  k.both: \"hit {damage} on {target}\"\n"; // source has two tokens
        out << "  k.singular: \"{count:item|items}\"\n";   // plural token counts as the bare `count`
    }
    {
        std::ofstream out(dePath);
        out << "locale: de\nstrings:\n";
        out << "  k.both: \"{target} getroffen\"\n";         // drops {damage}
        out << "  k.singular: \"{count} {n:item|items}\"\n"; // extra {n}, still has {count}
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    const auto issues = LocalizationLint::RunParameterDriftLint("en", { "de" });
    ASSERT_EQ(issues.size(), 2u);

    // Order isn't guaranteed; pick by key.
    const auto findIssue = [&](const std::string& k) -> const LocalizationLint::Issue*
    {
        for (const auto& i : issues)
            if (i.Key == k)
                return &i;
        return nullptr;
    };
    const auto* a = findIssue("k.both");
    const auto* b = findIssue("k.singular");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->MissingTokens.size(), 1u);
    EXPECT_TRUE(a->MissingTokens.contains("damage"));
    EXPECT_TRUE(a->ExtraTokens.empty());

    EXPECT_TRUE(b->MissingTokens.empty());
    EXPECT_EQ(b->ExtraTokens.size(), 1u);
    EXPECT_TRUE(b->ExtraTokens.contains("n"));

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, SaveLocaleToFileRoundTripsWithEdits)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_save.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    // Apply an edit through the same path the editor panel uses, then write
    // back to disk and re-load via Initialize to prove the file is parseable
    // and contains the edit.
    ASSERT_TRUE(LocalizationManager::SetKey("en", "ui.main_menu.play", "PLAY!"));
    ASSERT_TRUE(LocalizationManager::SaveLocaleToFile("en"));

    LocalizationManager::ResetForTesting();
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    EXPECT_EQ(LocalizationManager::Get("ui.main_menu.play", "en"), "PLAY!");

    std::filesystem::remove(enPath);
}

// -----------------------------------------------------------------------------
// Phase 6 — UTF-8, gender tokens, asset localization, metadata + max-length.
// -----------------------------------------------------------------------------

TEST(UTF8, DecodesAsciiAndMultiByteSequences)
{
    u32 cp = 0;
    sizet adv = 0;

    // ASCII
    OloEngine::UTF8::DecodeCodepoint("A", 0, cp, adv);
    EXPECT_EQ(cp, 0x41u);
    EXPECT_EQ(adv, 1u);

    // 2-byte (é)
    OloEngine::UTF8::DecodeCodepoint("\xC3\xA9", 0, cp, adv);
    EXPECT_EQ(cp, 0xE9u);
    EXPECT_EQ(adv, 2u);

    // 3-byte (日)
    OloEngine::UTF8::DecodeCodepoint("\xE6\x97\xA5", 0, cp, adv);
    EXPECT_EQ(cp, 0x65E5u);
    EXPECT_EQ(adv, 3u);

    // 4-byte (😀, U+1F600)
    OloEngine::UTF8::DecodeCodepoint("\xF0\x9F\x98\x80", 0, cp, adv);
    EXPECT_EQ(cp, 0x1F600u);
    EXPECT_EQ(adv, 4u);
}

TEST(UTF8, ReplacesInvalidSequencesWithReplacementCharacter)
{
    u32 cp = 0;
    sizet adv = 0;

    // Truncated 3-byte sequence (only the lead byte).
    OloEngine::UTF8::DecodeCodepoint("\xE6", 0, cp, adv);
    EXPECT_EQ(cp, 0xFFFDu);
    EXPECT_EQ(adv, 1u); // advance by 1 so the iterator makes progress

    // Continuation byte where a lead byte was expected.
    OloEngine::UTF8::DecodeCodepoint("\x80", 0, cp, adv);
    EXPECT_EQ(cp, 0xFFFDu);
    EXPECT_EQ(adv, 1u);

    // Overlong encoding of '/' (forbidden since RFC 3629).
    OloEngine::UTF8::DecodeCodepoint("\xC0\xAF", 0, cp, adv);
    EXPECT_EQ(cp, 0xFFFDu);
}

TEST(UTF8, CountCodepointsHandlesMixedAndInvalid)
{
    EXPECT_EQ(OloEngine::UTF8::CountCodepoints(""), 0u);
    EXPECT_EQ(OloEngine::UTF8::CountCodepoints("Hello"), 5u);
    // "日本語" — three 3-byte codepoints.
    EXPECT_EQ(OloEngine::UTF8::CountCodepoints("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"), 3u);
}

TEST(TextFormatter, GenderTokenSelectsByStringValue)
{
    const std::string pattern = "{gender:le|la|leur} chat";
    EXPECT_EQ(TextFormatter::Format(pattern, { { "gender", "masculine" } }), "le chat");
    EXPECT_EQ(TextFormatter::Format(pattern, { { "gender", "feminine" } }), "la chat");
    EXPECT_EQ(TextFormatter::Format(pattern, { { "gender", "neuter" } }), "leur chat");
    // Case-insensitive single-letter shorthand.
    EXPECT_EQ(TextFormatter::Format(pattern, { { "gender", "F" } }), "la chat");
    // Unknown value — token left literal so the bug is loud.
    EXPECT_EQ(TextFormatter::Format(pattern, { { "gender", "potato" } }), "{gender:le|la|leur} chat");
}

TEST_F(LocalizationFixture, ResolveLocalizedAssetPathPicksLocaleVariant)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_assetloc.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    auto dir = std::filesystem::temp_directory_path() / "olo_asset_loc";
    std::filesystem::create_directories(dir);
    const auto base = dir / "logo.png";
    const auto deVariant = dir / "logo.de.png";
    {
        std::ofstream out(base);
        out << "BASE";
    }
    {
        std::ofstream out(deVariant);
        out << "DE";
    }

    // Active locale 'en' — no en variant exists, base path wins.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedAssetPath(base), base);

    // Explicit 'de' override — variant exists.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedAssetPath(base, "de"), deVariant);

    // BCP-47 region fallback: 'de-AT' has no specific variant but the
    // language-only de.png does exist.
    EXPECT_EQ(LocalizationManager::ResolveLocalizedAssetPath(base, "de-AT"), deVariant);

    // Empty path stays empty.
    EXPECT_TRUE(LocalizationManager::ResolveLocalizedAssetPath({}).empty());

    std::filesystem::remove_all(dir);
    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, MaxLengthLintCatchesOverflowing)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_maxlen.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_maxlen.ololocale";
    {
        std::ofstream out(enPath);
        out << "locale: en\nstrings:\n";
        out << "  ui.button.play:\n";
        out << "    value: \"Play\"\n";
        out << "    context: \"Main menu CTA\"\n";
        out << "    max_length: 6\n";
    }
    {
        std::ofstream out(dePath);
        out << "locale: de\nstrings:\n";
        // German "Wiedergeben" (11 codepoints) exceeds the 6-codepoint budget.
        out << "  ui.button.play: \"Wiedergeben\"\n";
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    // Metadata round-trip on the source locale.
    const auto md = LocalizationManager::GetMetadata("ui.button.play", "en");
    EXPECT_EQ(md.Context, "Main menu CTA");
    EXPECT_EQ(md.MaxLength, 6u);

    const auto issues = LocalizationLint::RunMaxLengthLint("en", { "de", "en" });
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].Key, "ui.button.play");
    EXPECT_EQ(issues[0].TargetLocale, "de");
    EXPECT_NE(issues[0].Description.find("11 codepoints"), std::string::npos);
    EXPECT_NE(issues[0].Description.find("budget 6"), std::string::npos);

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

// -----------------------------------------------------------------------------
// Phase 7 — select tokens, currency, list, date/time, relative time.
// -----------------------------------------------------------------------------

TEST(TextFormatter, SelectTokenDispatchesByLabelWithElseFallback)
{
    const std::string pattern = "{role:warrior=knight|mage=wizard|else=hero} appears";

    EXPECT_EQ(TextFormatter::Format(pattern, { { "role", "warrior" } }), "knight appears");
    EXPECT_EQ(TextFormatter::Format(pattern, { { "role", "mage" } }), "wizard appears");
    // Unknown label hits the `else` fallback.
    EXPECT_EQ(TextFormatter::Format(pattern, { { "role", "rogue" } }), "hero appears");

    // Without an else=, an unknown label leaves the token literal.
    const std::string noFallback = "{role:warrior=knight|mage=wizard}";
    EXPECT_EQ(TextFormatter::Format(noFallback, { { "role", "rogue" } }), "{role:warrior=knight|mage=wizard}");
}

TEST_F(LocalizationFixture, FormatCurrencyHonoursLocaleSymbolAndPlacement)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_cur.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_cur.ololocale";
    auto jpPath = std::filesystem::temp_directory_path() / "olo_jp_cur.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << "locale: de\nthousand_separator: \".\"\ndecimal_separator: \",\"\n"
               "currency_symbol: \"\xE2\x82\xAC\"\ncurrency_symbol_before: false\nstrings: { dummy: \"x\" }\n";
    }
    {
        std::ofstream out(jpPath);
        out << "locale: ja\ncurrency_symbol: \"\xC2\xA5\"\ncurrency_symbol_before: true\ncurrency_decimals: 0\n"
               "strings: { dummy: \"x\" }\n";
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(jpPath));

    EXPECT_EQ(LocalizationManager::FormatCurrency(1234.5, "en"), "$1,234.50");
    EXPECT_EQ(LocalizationManager::FormatCurrency(1234.5, "de"), "1.234,50\xE2\x82\xAC");
    // MSVC's snprintf rounds half-to-even ("banker's rounding"), so 1234.5
    // → 1234 at 0 decimals. That's the same convention every accounting
    // system uses; we adopt whatever the C runtime gives us rather than
    // re-implementing rounding policy here.
    EXPECT_EQ(LocalizationManager::FormatCurrency(1234.5, "ja"), "\xC2\xA5"
                                                                 "1,234");
    // Symbol override (multi-currency game showing prices in a third currency).
    EXPECT_EQ(LocalizationManager::FormatCurrency(50.0, "en", "GP "), "GP 50.00");

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
    std::filesystem::remove(jpPath);
}

TEST_F(LocalizationFixture, FormatListJoinsWithLocaleJoiners)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_list.ololocale";
    auto dePath = std::filesystem::temp_directory_path() / "olo_de_list.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    {
        std::ofstream out(dePath);
        out << "locale: de\nlist_joiner: \", \"\nlist_last_joiner: \" und \"\nstrings: { dummy: \"x\" }\n";
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));
    ASSERT_TRUE(LocalizationManager::LoadLocale(dePath));

    EXPECT_EQ(LocalizationManager::FormatList({}, "en"), "");
    EXPECT_EQ(LocalizationManager::FormatList({ "apples" }, "en"), "apples");
    EXPECT_EQ(LocalizationManager::FormatList({ "apples", "oranges" }, "en"), "apples, and oranges");
    EXPECT_EQ(LocalizationManager::FormatList({ "apples", "oranges", "pears" }, "en"), "apples, oranges, and pears");
    EXPECT_EQ(LocalizationManager::FormatList({ "a", "b", "c", "d" }, "de"), "a, b, c und d");

    std::filesystem::remove(enPath);
    std::filesystem::remove(dePath);
}

TEST_F(LocalizationFixture, FormatDateUsesDefaultPatternsAndCustomOverrides)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_date.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    // Construct a known calendar time so the test isn't tz/locale-dependent
    // beyond the local-time conversion that's identical on every machine.
    std::tm tm{};
    tm.tm_year = 126; // 2026
    tm.tm_mon = 2;    // March (0-indexed)
    tm.tm_mday = 5;
    tm.tm_hour = 14;
    tm.tm_min = 30;
    tm.tm_sec = 15;
    tm.tm_isdst = -1;
    const std::time_t secs = std::mktime(&tm);
    const auto tp = std::chrono::system_clock::from_time_t(secs);

    // Default English styles.
    EXPECT_EQ(LocalizationManager::FormatDate(tp, LocalizationManager::DateStyle::Short, "en"), "3/5/26");
    EXPECT_EQ(LocalizationManager::FormatDate(tp, LocalizationManager::DateStyle::Medium, "en"), "Mar 5, 2026");
    EXPECT_EQ(LocalizationManager::FormatDate(tp, LocalizationManager::DateStyle::Long, "en"), "March 5, 2026");
    EXPECT_EQ(LocalizationManager::FormatTime(tp, LocalizationManager::TimeStyle::Short, "en"), "14:30");
    EXPECT_EQ(LocalizationManager::FormatTime(tp, LocalizationManager::TimeStyle::Medium, "en"), "14:30:15");

    // Custom locale-supplied pattern: author writes a different style than
    // the default. We inject one through SetKey.
    ASSERT_TRUE(LocalizationManager::SetKey("en", "date.format.short", "{dd}-{MM}-{yyyy}"));
    EXPECT_EQ(LocalizationManager::FormatDate(tp, LocalizationManager::DateStyle::Short, "en"), "05-03-2026");

    std::filesystem::remove(enPath);
}

TEST_F(LocalizationFixture, FormatRelativeTimePicksLargestUnit)
{
    auto enPath = std::filesystem::temp_directory_path() / "olo_en_rel.ololocale";
    {
        std::ofstream out(enPath);
        out << kEnglishYAML;
    }
    ASSERT_TRUE(LocalizationManager::LoadLocale(enPath));

    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::now();

    // Past
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now - 30s, "en"), "30 seconds ago");
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now - 1min, "en"), "1 minute ago");
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now - 3min, "en"), "3 minutes ago");
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now - 2h, "en"), "2 hours ago");

    // Future
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now + 5min, "en"), "in 5 minutes");
    EXPECT_EQ(LocalizationManager::FormatRelativeTime(now + 1h, "en"), "in 1 hour");

    std::filesystem::remove(enPath);
}

// -----------------------------------------------------------------------------
// LocalizedTextComponent
// -----------------------------------------------------------------------------

TEST(LocalizedTextComponent, EqualityCompares)
{
    LocalizedTextComponent a{ "ui.main_menu.play" };
    LocalizedTextComponent b{ "ui.main_menu.play" };
    LocalizedTextComponent c{ "ui.main_menu.quit" };
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
