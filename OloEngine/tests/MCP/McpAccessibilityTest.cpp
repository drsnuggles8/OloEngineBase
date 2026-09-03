#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpAccessibility.h"
#include "MCP/McpTokenNormalization.h"

#include <cmath>
#include <limits>
#include <set>

// OLO_TEST_LAYER: unit

namespace
{
    namespace A11y = OloEngine::MCP::AccessibilitySettingsTool;
    using OloEngine::Accessibility;
    using OloEngine::AccessibilitySettings;
    using OloEngine::ColorBlindAdaptation;
    using OloEngine::ColorBlindMode;
    using Json = nlohmann::json;

    A11y::ApplyResult Apply(std::string_view token, const Json& value, AccessibilitySettings& settings)
    {
        const A11y::FieldInfo* field = A11y::FindField(token);
        EXPECT_NE(field, nullptr);
        return field ? A11y::Apply(*field, value, settings) : A11y::ApplyResult{};
    }

    class McpAccessibilityGlobalTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Accessibility::Reset();
        }
        void TearDown() override
        {
            Accessibility::Reset();
        }
    };
} // namespace

TEST(McpAccessibility, CatalogueHasAllNineUniqueSettings)
{
    ASSERT_EQ(A11y::kFields.size(), 9u);
    std::set<std::string> tokens;
    for (const A11y::FieldInfo& field : A11y::kFields)
    {
        EXPECT_FALSE(field.Token.empty());
        EXPECT_TRUE(tokens.insert(OloEngine::MCP::NormalizeToken(field.Token)).second);
        EXPECT_FALSE(field.Description.empty());
    }
    EXPECT_NE(A11y::FindField("subtitle_font-size"), nullptr);
    EXPECT_NE(A11y::FindField("COLOR BLIND METHOD"), nullptr);
}

TEST(McpAccessibility, SchemasAreClosedAndDerivedFromCatalogue)
{
    const Json get = A11y::GetInputSchema();
    const Json set = A11y::SetInputSchema();
    EXPECT_EQ(get["additionalProperties"], false);
    EXPECT_EQ(set["additionalProperties"], false);
    EXPECT_EQ(get["properties"]["setting"]["enum"].size(), A11y::kFields.size());
    EXPECT_EQ(set["properties"]["setting"]["enum"].size(), A11y::kFields.size());
    EXPECT_EQ(set["properties"]["value"]["type"], Json::array({ "boolean", "number", "string" }));
    EXPECT_EQ(set["required"], Json::array({ "setting", "value" }));
}

TEST(McpAccessibility, AppliesBothBooleanSettingsWithStrictTypes)
{
    AccessibilitySettings settings;
    auto subtitles = Apply("SubtitlesEnabled", true, settings);
    ASSERT_TRUE(subtitles.Ok) << subtitles.Error;
    EXPECT_TRUE(settings.SubtitlesEnabled);
    EXPECT_EQ(subtitles.Data["scope"], "process");
    EXPECT_EQ(subtitles.Data["restoreWith"], false);

    auto speaker = Apply("SubtitleShowSpeaker", false, settings);
    ASSERT_TRUE(speaker.Ok) << speaker.Error;
    EXPECT_FALSE(settings.SubtitleShowSpeaker);

    EXPECT_FALSE(Apply("SubtitlesEnabled", 1, settings).Ok);
    EXPECT_FALSE(Apply("SubtitleShowSpeaker", "false", settings).Ok);
}

TEST(McpAccessibility, NumericSettingsUseEngineSanitizerBounds)
{
    AccessibilitySettings settings;
    struct Case
    {
        const char* Token;
        f64 Input;
        f64 Expected;
    };
    const std::array cases = {
        Case{ "SubtitleFontSize", 500.0, 200.0 },
        Case{ "SubtitleBackgroundOpacity", -2.0, 0.0 },
        Case{ "UITextScale", 0.1, static_cast<f64>(OloEngine::kMinUITextScale) },
        Case{ "MinimumFontSize", 150.0, static_cast<f64>(OloEngine::kMaxMinimumFontSize) },
        Case{ "ColorBlindSeverity", 2.0, 1.0 },
    };

    for (const Case& test : cases)
    {
        const auto result = Apply(test.Token, test.Input, settings);
        ASSERT_TRUE(result.Ok) << test.Token << ": " << result.Error;
        EXPECT_NEAR(result.Data["value"].get<f64>(), test.Expected, 1.0e-6) << test.Token;
        EXPECT_EQ(result.Data["clamped"], true) << test.Token;
    }
}

TEST(McpAccessibility, NumericSettingsRejectWrongAndNonFiniteTypesWithoutMutation)
{
    AccessibilitySettings settings;
    const AccessibilitySettings before = settings;

    EXPECT_FALSE(Apply("UITextScale", true, settings).Ok);
    EXPECT_EQ(settings, before);
    EXPECT_FALSE(Apply("UITextScale", "2", settings).Ok);
    EXPECT_EQ(settings, before);

    const Json nan = Json(std::numeric_limits<f64>::quiet_NaN());
    EXPECT_FALSE(Apply("UITextScale", nan, settings).Ok);
    EXPECT_EQ(settings, before);
    const Json infinity = Json(std::numeric_limits<f64>::infinity());
    EXPECT_FALSE(Apply("ColorBlindSeverity", infinity, settings).Ok);
    EXPECT_EQ(settings, before);
}

TEST(McpAccessibility, EnumSettingsAreNamedAndStrict)
{
    AccessibilitySettings settings;
    auto mode = Apply("ColorBlind", "Deuter-anopia", settings);
    ASSERT_TRUE(mode.Ok) << mode.Error;
    EXPECT_EQ(settings.ColorBlind, ColorBlindMode::Deuteranopia);
    EXPECT_EQ(mode.Data["value"], "deuteranopia");

    auto method = Apply("ColorBlindMethod", "simulate", settings);
    ASSERT_TRUE(method.Ok) << method.Error;
    EXPECT_EQ(settings.ColorBlindMethod, ColorBlindAdaptation::Simulate);

    const AccessibilitySettings before = settings;
    EXPECT_FALSE(Apply("ColorBlind", 2, settings).Ok);
    EXPECT_EQ(settings, before);
    EXPECT_FALSE(Apply("ColorBlindMethod", "preview", settings).Ok);
    EXPECT_EQ(settings, before);
}

TEST(McpAccessibility, OnlyChangedColorBlindModeRequestsRenderGraphRebuild)
{
    AccessibilitySettings settings;
    auto mode = Apply("ColorBlind", "protanopia", settings);
    ASSERT_TRUE(mode.Ok);
    EXPECT_TRUE(mode.RequiresRenderGraphRebuild);
    EXPECT_EQ(mode.Data["rebuildsRenderGraph"], true);

    auto sameMode = Apply("ColorBlind", "protanopia", settings);
    ASSERT_TRUE(sameMode.Ok);
    EXPECT_FALSE(sameMode.RequiresRenderGraphRebuild);
    EXPECT_FALSE(sameMode.Data.contains("rebuildsRenderGraph"));

    auto severity = Apply("ColorBlindSeverity", 0.4, settings);
    ASSERT_TRUE(severity.Ok);
    EXPECT_FALSE(severity.RequiresRenderGraphRebuild);
    auto method = Apply("ColorBlindMethod", "simulate", settings);
    ASSERT_TRUE(method.Ok);
    EXPECT_FALSE(method.RequiresRenderGraphRebuild);
}

TEST_F(McpAccessibilityGlobalTest, ApplyGlobalMutatesTheProcessGlobalAndRestoresPriorValue)
{
    const A11y::FieldInfo* field = A11y::FindField("UITextScale");
    ASSERT_NE(field, nullptr);
    const auto applied = A11y::ApplyGlobal(*field, 2.25);
    ASSERT_TRUE(applied.Ok) << applied.Error;
    EXPECT_FLOAT_EQ(Accessibility::Get().UITextScale, 2.25f);
    EXPECT_EQ(A11y::DescribeGlobal()["scope"], "process");

    const auto restored = A11y::ApplyGlobal(*field, applied.Data["restoreWith"]);
    ASSERT_TRUE(restored.Ok) << restored.Error;
    EXPECT_FLOAT_EQ(Accessibility::Get().UITextScale, 1.0f);
}

TEST(McpAccessibility, ParseHelpersRejectMissingAndUnknownSettings)
{
    const A11y::FieldInfo* field = nullptr;
    Json value;
    EXPECT_TRUE(A11y::ParseSetArgs(Json::object(), field, value).has_value());
    EXPECT_TRUE(A11y::ParseSetArgs(Json{ { "setting", "bogus" }, { "value", true } }, field, value).has_value());
    EXPECT_TRUE(A11y::ParseSetArgs(Json{ { "setting", "SubtitlesEnabled" } }, field, value).has_value());
    EXPECT_FALSE(A11y::ParseSetArgs(Json{ { "setting", "SubtitlesEnabled" }, { "value", true } }, field, value).has_value());
    ASSERT_NE(field, nullptr);

    EXPECT_FALSE(A11y::ParseGetArgs(Json::object(), field).has_value());
    EXPECT_EQ(field, nullptr);
    EXPECT_TRUE(A11y::ParseGetArgs(Json{ { "setting", 1 } }, field).has_value());
}
