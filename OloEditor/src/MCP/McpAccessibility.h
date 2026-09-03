#pragma once

// Header-only schema, parsing, and apply core for olo_accessibility_get/_set.
// Accessibility preferences are process-global, not scene state. The live handler
// should use DescribeGlobal()/ApplyGlobal(); Apply() remains available as the pure
// POD seam for exhaustive unit tests. No operation participates in CommandHistory:
// restoring a write means setting the reported `restoreWith` value.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpTokenNormalization.h"

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::AccessibilitySettingsTool
{
    using Json = nlohmann::json;

    enum class ValueType : u8
    {
        Bool,
        Float,
        Enum,
    };

    enum class Setting : u8
    {
        SubtitlesEnabled,
        SubtitleShowSpeaker,
        SubtitleFontSize,
        SubtitleBackgroundOpacity,
        UITextScale,
        MinimumFontSize,
        ColorBlind,
        ColorBlindMethod,
        ColorBlindSeverity,
    };

    struct EnumValue
    {
        std::string_view Token;
        i32 Value;
        std::string_view Description;
    };

    inline constexpr std::array<EnumValue, 4> kColorBlindValues = { {
        { "none", static_cast<i32>(ColorBlindMode::None), "Disable colour-vision adaptation." },
        { "protanopia", static_cast<i32>(ColorBlindMode::Protanopia), "Adapt for missing/impaired L-cone vision." },
        { "deuteranopia", static_cast<i32>(ColorBlindMode::Deuteranopia), "Adapt for missing/impaired M-cone vision." },
        { "tritanopia", static_cast<i32>(ColorBlindMode::Tritanopia), "Adapt for missing/impaired S-cone vision." },
    } };

    inline constexpr std::array<EnumValue, 2> kColorBlindMethodValues = { {
        { "correct", static_cast<i32>(ColorBlindAdaptation::Correct), "Daltonize colours to preserve distinctions." },
        { "simulate", static_cast<i32>(ColorBlindAdaptation::Simulate), "Preview the selected colour-vision deficiency." },
    } };

    struct FieldInfo
    {
        std::string_view Token;
        Setting Id;
        std::string_view Group;
        ValueType Type;
        f64 Min = 0.0;
        f64 Max = 0.0;
        std::span<const EnumValue> Values;
        std::string_view Description;
    };

    inline constexpr std::array<FieldInfo, 9> kFields = { {
        { "SubtitlesEnabled", Setting::SubtitlesEnabled, "subtitles", ValueType::Bool, 0.0, 1.0, {}, "Show the subtitle/caption overlay." },
        { "SubtitleShowSpeaker", Setting::SubtitleShowSpeaker, "subtitles", ValueType::Bool, 0.0, 1.0, {}, "Prefix subtitles with the speaker name." },
        { "SubtitleFontSize", Setting::SubtitleFontSize, "subtitles", ValueType::Float, 8.0, 200.0, {}, "Subtitle font size in screen pixels before UI scaling." },
        { "SubtitleBackgroundOpacity", Setting::SubtitleBackgroundOpacity, "subtitles", ValueType::Float, 0.0, 1.0, {}, "Opacity of the subtitle backing plate." },
        { "UITextScale", Setting::UITextScale, "text", ValueType::Float, kMinUITextScale, kMaxUITextScale, {}, "Process-global UI text-size multiplier." },
        { "MinimumFontSize", Setting::MinimumFontSize, "text", ValueType::Float, 0.0, kMaxMinimumFontSize, {}, "Post-scale minimum UI font size; zero disables the floor." },
        { "ColorBlind", Setting::ColorBlind, "colorvision", ValueType::Enum, 0.0, 0.0, kColorBlindValues, "Colour-vision deficiency to adapt for." },
        { "ColorBlindMethod", Setting::ColorBlindMethod, "colorvision", ValueType::Enum, 0.0, 0.0, kColorBlindMethodValues, "Correct colours for the viewer or simulate the deficiency." },
        { "ColorBlindSeverity", Setting::ColorBlindSeverity, "colorvision", ValueType::Float, 0.0, 1.0, {}, "Adaptation strength from identity (0) to full dichromacy (1)." },
    } };

    [[nodiscard]] inline const FieldInfo* FindField(std::string_view token)
    {
        const std::string key = NormalizeToken(token);
        if (key.empty())
            return nullptr;
        for (const FieldInfo& field : kFields)
        {
            if (NormalizeToken(field.Token) == key)
                return &field;
        }
        return nullptr;
    }

    [[nodiscard]] inline const EnumValue* FindEnumValue(const FieldInfo& field, std::string_view token)
    {
        const std::string key = NormalizeToken(token);
        for (const EnumValue& value : field.Values)
        {
            if (NormalizeToken(value.Token) == key)
                return &value;
        }
        return nullptr;
    }

    [[nodiscard]] inline std::string JoinFieldTokens()
    {
        std::string result;
        constexpr sizet fieldCount = kFields.size();
        for (sizet i = 0; i < fieldCount; ++i)
            result += (i == 0 ? "" : ", ") + std::string(kFields[i].Token);
        return result;
    }

    [[nodiscard]] inline std::string JoinEnumTokens(const FieldInfo& field)
    {
        std::string result;
        const sizet valueCount = field.Values.size();
        for (sizet i = 0; i < valueCount; ++i)
            result += (i == 0 ? "" : ", ") + std::string(field.Values[i].Token);
        return result;
    }

    [[nodiscard]] inline Json CurrentValue(const FieldInfo& field, const ::OloEngine::AccessibilitySettings& settings)
    {
        switch (field.Id)
        {
            case Setting::SubtitlesEnabled:
                return settings.SubtitlesEnabled;
            case Setting::SubtitleShowSpeaker:
                return settings.SubtitleShowSpeaker;
            case Setting::SubtitleFontSize:
                return settings.SubtitleFontSize;
            case Setting::SubtitleBackgroundOpacity:
                return settings.SubtitleBackgroundOpacity;
            case Setting::UITextScale:
                return settings.UITextScale;
            case Setting::MinimumFontSize:
                return settings.MinimumFontSize;
            case Setting::ColorBlind:
            {
                const i32 current = static_cast<i32>(settings.ColorBlind);
                for (const EnumValue& value : field.Values)
                    if (value.Value == current)
                        return std::string(value.Token);
                return "none";
            }
            case Setting::ColorBlindMethod:
            {
                const i32 current = static_cast<i32>(settings.ColorBlindMethod);
                for (const EnumValue& value : field.Values)
                    if (value.Value == current)
                        return std::string(value.Token);
                return "correct";
            }
            case Setting::ColorBlindSeverity:
                return settings.ColorBlindSeverity;
        }
        return nullptr;
    }

    [[nodiscard]] inline Json DescribeField(const FieldInfo& field, const ::OloEngine::AccessibilitySettings& settings)
    {
        Json result{
            { "setting", std::string(field.Token) },
            { "group", std::string(field.Group) },
            { "description", std::string(field.Description) },
            { "value", CurrentValue(field, settings) },
        };
        if (field.Type == ValueType::Float)
        {
            result["type"] = "number";
            result["min"] = field.Min;
            result["max"] = field.Max;
        }
        else if (field.Type == ValueType::Bool)
        {
            result["type"] = "boolean";
        }
        else
        {
            result["type"] = "enum";
            result["values"] = Json::array();
            for (const EnumValue& value : field.Values)
            {
                result["values"].push_back(Json{
                    { "token", std::string(value.Token) },
                    { "description", std::string(value.Description) },
                });
            }
        }
        if (field.Id == Setting::ColorBlind)
            result["rebuildsRenderGraph"] = true;
        return result;
    }

    [[nodiscard]] inline Json Describe(const ::OloEngine::AccessibilitySettings& settings)
    {
        Json fields = Json::array();
        for (const FieldInfo& field : kFields)
            fields.push_back(DescribeField(field, settings));
        return Json{
            { "scope", "process" },
            { "settings", std::move(fields) },
        };
    }

    [[nodiscard]] inline Json DescribeGlobal()
    {
        return Describe(::OloEngine::Accessibility::Get());
    }

    struct ApplyResult
    {
        bool Ok = false;
        std::string Error;
        Json Data;
        bool RequiresRenderGraphRebuild = false;
    };

    [[nodiscard]] inline ApplyResult Apply(const FieldInfo& field, const Json& value,
                                           ::OloEngine::AccessibilitySettings& settings)
    {
        ApplyResult result;
        const Json previous = CurrentValue(field, settings);
        ::OloEngine::AccessibilitySettings candidate = settings;
        bool clamped = false;

        if (field.Type == ValueType::Bool)
        {
            if (!value.is_boolean())
            {
                result.Error = "Invalid value for '" + std::string(field.Token) + "': expected a boolean.";
                return result;
            }
            const bool parsed = value.get<bool>();
            if (field.Id == Setting::SubtitlesEnabled)
                candidate.SubtitlesEnabled = parsed;
            else
                candidate.SubtitleShowSpeaker = parsed;
        }
        else if (field.Type == ValueType::Float)
        {
            if (!(value.is_number_float() || value.is_number_integer() || value.is_number_unsigned()))
            {
                result.Error = "Invalid value for '" + std::string(field.Token) + "': expected a finite number.";
                return result;
            }
            const f64 parsed = value.get<f64>();
            if (!std::isfinite(parsed))
            {
                result.Error = "Invalid value for '" + std::string(field.Token) + "': expected a finite number.";
                return result;
            }
            switch (field.Id)
            {
                case Setting::SubtitleFontSize:
                    candidate.SubtitleFontSize = static_cast<f32>(parsed);
                    break;
                case Setting::SubtitleBackgroundOpacity:
                    candidate.SubtitleBackgroundOpacity = static_cast<f32>(parsed);
                    break;
                case Setting::UITextScale:
                    candidate.UITextScale = static_cast<f32>(parsed);
                    break;
                case Setting::MinimumFontSize:
                    candidate.MinimumFontSize = static_cast<f32>(parsed);
                    break;
                case Setting::ColorBlindSeverity:
                    candidate.ColorBlindSeverity = static_cast<f32>(parsed);
                    break;
                default:
                    break;
            }
            SanitizeAccessibilitySettings(candidate);
            const Json sanitized = CurrentValue(field, candidate);
            const f32 requested = static_cast<f32>(parsed);
            clamped = std::fabs(sanitized.get<f32>() - requested) > 1.0e-6f;
        }
        else
        {
            if (!value.is_string())
            {
                result.Error = "Invalid value for '" + std::string(field.Token) + "': expected one of " +
                               JoinEnumTokens(field) + ".";
                return result;
            }
            const EnumValue* parsed = FindEnumValue(field, value.get<std::string>());
            if (parsed == nullptr)
            {
                result.Error = "Invalid value '" + value.get<std::string>() + "' for '" +
                               std::string(field.Token) + "'. Valid values: " + JoinEnumTokens(field) + ".";
                return result;
            }
            if (field.Id == Setting::ColorBlind)
                candidate.ColorBlind = static_cast<ColorBlindMode>(parsed->Value);
            else
                candidate.ColorBlindMethod = static_cast<ColorBlindAdaptation>(parsed->Value);
        }

        // Use the engine's sanitizer as the final authority even for bool/enum
        // writes, keeping this path identical to Accessibility::Set.
        SanitizeAccessibilitySettings(candidate);
        const Json applied = CurrentValue(field, candidate);
        const bool changed = applied != previous;
        settings = candidate;

        result.Ok = true;
        result.RequiresRenderGraphRebuild = field.Id == Setting::ColorBlind && changed;
        result.Data = Json{
            { "scope", "process" },
            { "setting", std::string(field.Token) },
            { "previousValue", previous },
            { "value", applied },
            { "changed", changed },
            { "restoreWith", previous },
        };
        if (field.Type == ValueType::Float)
        {
            result.Data["clamped"] = clamped;
            result.Data["range"] = Json{ { "min", field.Min }, { "max", field.Max } };
        }
        if (result.RequiresRenderGraphRebuild)
            result.Data["rebuildsRenderGraph"] = true;
        return result;
    }

    // The production handler contract: update the one process-global instance
    // through Accessibility::Set, which sanitizes again and becomes visible to
    // renderer/UI/subtitle consumers on the next frame.
    [[nodiscard]] inline ApplyResult ApplyGlobal(const FieldInfo& field, const Json& value)
    {
        ::OloEngine::AccessibilitySettings settings = ::OloEngine::Accessibility::Get();
        ApplyResult result = Apply(field, value, settings);
        if (result.Ok)
            ::OloEngine::Accessibility::Set(settings);
        return result;
    }

    [[nodiscard]] inline std::vector<std::string> FieldTokens()
    {
        std::vector<std::string> tokens;
        tokens.reserve(kFields.size());
        for (const FieldInfo& field : kFields)
            tokens.emplace_back(field.Token);
        return tokens;
    }

    [[nodiscard]] inline Json GetInputSchema()
    {
        return Schema::Object()
            .Prop("setting", Schema::String().EnumFrom(FieldTokens()).Desc("Optional setting token; omit to list all nine settings."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json SetInputSchema()
    {
        const Json valueSchema = {
            { "type", Json::array({ "boolean", "number", "string" }) },
            { "description", "Boolean, finite number, or enum token required by the selected setting." },
        };
        return Schema::Object()
            .Prop("setting", Schema::String().EnumFrom(FieldTokens()))
            .Prop("value", Schema::Raw(valueSchema))
            .Required({ "setting", "value" })
            .NoAdditional();
    }

    [[nodiscard]] inline std::optional<std::string> ParseGetArgs(const Json& args, const FieldInfo*& field)
    {
        field = nullptr;
        if (!args.contains("setting") || args["setting"].is_null())
            return std::nullopt;
        if (!args["setting"].is_string())
            return "Invalid 'setting': expected a string.";
        field = FindField(args["setting"].get<std::string>());
        if (field == nullptr)
            return "Unknown accessibility setting. Valid settings: " + JoinFieldTokens() + ".";
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseSetArgs(const Json& args, const FieldInfo*& field, Json& value)
    {
        field = nullptr;
        if (!args.contains("setting") || !args["setting"].is_string())
            return "Missing or invalid 'setting'. Valid settings: " + JoinFieldTokens() + ".";
        field = FindField(args["setting"].get<std::string>());
        if (field == nullptr)
            return "Unknown accessibility setting '" + args["setting"].get<std::string>() +
                   "'. Valid settings: " + JoinFieldTokens() + ".";
        if (!args.contains("value"))
            return "Missing required argument 'value' for '" + std::string(field->Token) + "'.";
        value = args["value"];
        return std::nullopt;
    }
} // namespace OloEngine::MCP::AccessibilitySettingsTool
