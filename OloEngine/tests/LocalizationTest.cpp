// =============================================================================
// LocalizationTest -- contracts of the localization foundation.
//
// Covers StringTable YAML loading, TextFormatter substitution + plural-form
// resolution, and LocalizationManager locale switching / event firing /
// missing-key fallback.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Localization/LocaleDefinition.h"
#include "OloEngine/Localization/LocalizationEvents.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Localization/StringTable.h"
#include "OloEngine/Localization/TextFormatter.h"

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
        {{"damage", "42"}, {"target", "Wolf"}});
    EXPECT_EQ(s, "You dealt 42 damage to Wolf.");
}

TEST(TextFormatter, MissingParameterLeavesTokenLiteral)
{
    const std::string s = TextFormatter::Format("Hello {who}!", {});
    EXPECT_EQ(s, "Hello {who}!");
}

TEST(TextFormatter, DoubleBracesEmitLiteralBraces)
{
    const std::string s = TextFormatter::Format("{{not a token}} but {real}", {{"real", "yes"}});
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
    const std::string s = TextFormatter::Format("{count:a|b}", {{"count", "abc"}});
    EXPECT_EQ(s, "{count:a|b}");
}

TEST(TextFormatter, UnclosedBraceLeavesRemainderLiteral)
{
    const std::string s = TextFormatter::Format("Hello {unclosed", {{"unclosed", "should-not-substitute"}});
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
        LocalizationManager::Format("combat.damage_dealt", {{"damage", "42"}, {"target", "Wolf"}}),
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

    std::atomic<int> eventCount{0};
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

    std::atomic<bool> stop{false};
    std::atomic<u64> readCount{0};
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
// LocalizedTextComponent
// -----------------------------------------------------------------------------

TEST(LocalizedTextComponent, EqualityCompares)
{
    LocalizedTextComponent a{"ui.main_menu.play"};
    LocalizedTextComponent b{"ui.main_menu.play"};
    LocalizedTextComponent c{"ui.main_menu.quit"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
