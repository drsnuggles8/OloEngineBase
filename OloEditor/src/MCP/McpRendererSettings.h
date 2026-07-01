#pragma once

// Shared, editor-side logic behind the consented MCP write tool
// `olo_renderer_settings_set` (issue #306 item C): set a multi-valued, session-
// global renderer / post-process setting — the FSR1 spatial-upscale mode, the
// tone-map operator, or the rendering path — so an agent can verify a rendering
// feature LIVE at each setting over MCP (the motivating case is #480's FSR1
// EASU/RCAS "Spatial Upscale" dropdown, which the read-only server couldn't drive).
//
// It is the ENUM-valued sibling of the boolean `olo_render_toggle_pass`: the
// toggle tool flips on/off fields, this one selects one of several named values
// the toggle shape can't express (upscale quality preset, tone-map operator,
// forward/forward+/deferred path).
//
// Restore semantics — restore-PRIOR-VALUE, NOT CommandHistory. Unlike the entity
// field writes (olo_set_collision_layer / olo_entity_set_field), which push an
// undoable ComponentChangeCommand onto the editor's undo stack, these are GLOBAL
// renderer settings, not scene/ECS data — an undo-stack entry would be wrong. So
// the tool snapshots the prior value and reports it as `previousValue`; to restore,
// the agent calls again with that token (the change is reverted by setting it back).
// The settings are session-global and never written to the project, so a scene
// reload also restores them — the same ephemeral boundary the render-override /
// sun tools respect.
//
// Everything here is httplib/editor/McpServer-free: it mutates the plain POD
// settings structs (PostProcessSettings / RendererSettings) by reference + the
// schema-builder DSL + nlohmann::json, so the MCP test binary (which deliberately
// does NOT link McpTools.cpp) exercises the real schema + parse + apply code, the
// same split McpSetCollisionLayer.h / McpRenderOverrides.h use. The renderer-bound
// side effect a render-path switch needs (rebuild the render-graph topology via
// Renderer3D::ApplyRendererSettings) stays in the McpTools.cpp handler — this
// header only signals it via ApplyResult::PathChanged.
//
// The enum value tables reference the engine enums directly (UpscaleMode /
// TonemapOperator / RenderingPath), so the token->int mapping can never drift out
// of sync with the renderer — a reordered enum is a compile-time change here.

#include "MCP/McpSchemaBuilder.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::RendererSettings
{
    using Json = nlohmann::json;

    // The multi-valued renderer settings this tool can set. Each maps to exactly one
    // enum field; the field binding lives in Apply()/Describe() below (still header-
    // only — the structs are plain POD).
    enum class Setting
    {
        Upscale,    // PostProcessSettings::Upscale  (FSR1 spatial-upscale quality preset)
        Tonemap,    // PostProcessSettings::Tonemap  (tone-map operator)
        RenderPath, // RendererSettings::Path        (forward / forward+ / deferred)
    };

    // One allowed value of an enum-valued setting: the stable, user-facing token the
    // agent passes, the underlying engine enum integer, and a human description.
    struct EnumValue
    {
        std::string_view Token;
        i32 Value;
        std::string_view Description;
    };

    // Value tables. Each references the engine enum directly, so the token->int
    // mapping is pinned to the renderer at compile time (a reordered/renumbered enum
    // fails to compile here rather than silently mis-mapping).
    inline constexpr std::array<EnumValue, 5> kUpscaleValues = { {
        { "off", static_cast<i32>(UpscaleMode::Off), "Native resolution — no FSR1 upscale (render scale 1.0)" },
        { "quality", static_cast<i32>(UpscaleMode::Quality), "FSR1 Quality (0.667x linear render scale)" },
        { "balanced", static_cast<i32>(UpscaleMode::Balanced), "FSR1 Balanced (0.59x linear render scale)" },
        { "performance", static_cast<i32>(UpscaleMode::Performance), "FSR1 Performance (0.5x linear render scale)" },
        { "ultraperformance", static_cast<i32>(UpscaleMode::UltraPerformance), "FSR1 Ultra Performance (0.333x linear render scale)" },
    } };

    inline constexpr std::array<EnumValue, 4> kTonemapValues = { {
        { "none", static_cast<i32>(TonemapOperator::None), "No tone mapping (raw HDR clamp)" },
        { "reinhard", static_cast<i32>(TonemapOperator::Reinhard), "Reinhard" },
        { "aces", static_cast<i32>(TonemapOperator::ACES), "ACES filmic" },
        { "uncharted2", static_cast<i32>(TonemapOperator::Uncharted2), "Uncharted 2 filmic" },
    } };

    inline constexpr std::array<EnumValue, 3> kRenderPathValues = { {
        { "forward", static_cast<i32>(RenderingPath::Forward), "Classic forward (all lights via UBO loop; low overhead)" },
        { "forwardplus", static_cast<i32>(RenderingPath::ForwardPlus), "Tiled Forward+ (compute-culled per-tile light lists)" },
        { "deferred", static_cast<i32>(RenderingPath::Deferred), "G-buffer + tiled deferred lighting (enables SSR/SSGI)" },
    } };

    // Setting token + description + which struct field it targets (documented only).
    struct SettingInfo
    {
        std::string_view Token;
        Setting Id;
        std::string_view Description;
    };

    inline constexpr std::array<SettingInfo, 3> kSettings = { {
        { "upscale", Setting::Upscale,
          "FSR1 spatial-upscale quality preset (PostProcess.Upscale). Off is native resolution; the other presets render "
          "below display resolution and EASU-upscale the HDR scene colour back to display res (#480)." },
        { "tonemap", Setting::Tonemap, "Tone-mapping operator applied to the HDR scene colour (PostProcess.Tonemap)." },
        { "renderpath", Setting::RenderPath,
          "High-level rendering path (RendererSettings.Path). Switching rebuilds the render-graph topology; Deferred is "
          "required for SSR / SSGI." },
    } };

    // Lowercase + drop every non-alphanumeric character so "Ultra Performance",
    // "ultra_performance" and "ultra-performance" all collapse to one key. Pure;
    // makes token matching forgiving, mirroring RenderOverrides::Normalize.
    [[nodiscard]] inline std::string Normalize(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0)
                out.push_back(static_cast<char>(std::tolower(uc)));
        }
        return out;
    }

    // The allowed values of a setting.
    [[nodiscard]] inline std::span<const EnumValue> SettingValues(Setting setting)
    {
        switch (setting)
        {
            case Setting::Upscale:
                return kUpscaleValues;
            case Setting::Tonemap:
                return kTonemapValues;
            case Setting::RenderPath:
                return kRenderPathValues;
        }
        return {};
    }

    // Canonical token for a Setting (reported back in the tool JSON).
    [[nodiscard]] inline std::string_view SettingToken(Setting setting)
    {
        for (const auto& info : kSettings)
        {
            if (info.Id == setting)
                return info.Token;
        }
        return "unknown";
    }

    // Resolve a setting token (case / separator insensitive). Returns false for an
    // unknown token.
    [[nodiscard]] inline bool ParseSetting(std::string_view token, Setting& out)
    {
        const std::string key = Normalize(token);
        if (key.empty())
            return false;
        for (const auto& info : kSettings)
        {
            if (Normalize(info.Token) == key)
            {
                out = info.Id;
                return true;
            }
        }
        return false;
    }

    // Resolve a value token within one setting's allowed values (case / separator
    // insensitive) to its engine enum integer. Returns false for an unknown token.
    [[nodiscard]] inline bool ParseValue(Setting setting, std::string_view token, i32& out)
    {
        const std::string key = Normalize(token);
        if (key.empty())
            return false;
        for (const auto& value : SettingValues(setting))
        {
            if (Normalize(value.Token) == key)
            {
                out = value.Value;
                return true;
            }
        }
        return false;
    }

    // Canonical token for a setting's current integer value ("unknown" if the live
    // value is outside the known set — never expected, but reported honestly).
    [[nodiscard]] inline std::string ValueToken(Setting setting, i32 value)
    {
        for (const auto& v : SettingValues(setting))
        {
            if (v.Value == value)
                return std::string(v.Token);
        }
        return "unknown";
    }

    // ", "-joined token lists for error / help text.
    [[nodiscard]] inline std::string JoinSettingTokens()
    {
        std::string out;
        for (const auto& info : kSettings)
        {
            if (!out.empty())
                out += ", ";
            out += std::string(info.Token);
        }
        return out;
    }

    [[nodiscard]] inline std::string JoinValueTokens(Setting setting)
    {
        std::string out;
        for (const auto& v : SettingValues(setting))
        {
            if (!out.empty())
                out += ", ";
            out += std::string(v.Token);
        }
        return out;
    }

    // The tool's inputSchema. Both properties are optional at the schema level: no
    // arguments = introspection (list every setting + its current value + allowed
    // values). The value-token-for-this-setting constraint can't be expressed in
    // JSON-Schema (it is conditional on `setting`), so ParseArgs enforces it and the
    // server's schema check only guarantees `setting` is one of the known tokens.
    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("setting", Schema::String().Enum({ "upscale", "tonemap", "renderpath" }).Desc("Which renderer / post-process setting to set. Omit both arguments to list every setting with its current value and allowed values."))
            .Prop("value", Schema::String().Desc("The new value token for the chosen setting — upscale: off|quality|balanced|performance|ultraperformance; tonemap: none|reinhard|aces|uncharted2; renderpath: forward|forwardplus|deferred. Call with no arguments to discover the valid tokens for each setting."))
            .NoAdditional();
    }

    // Parse the tool arguments. On success, `isIntrospect` true means "list all"
    // (no `setting` given); otherwise `setting` + `value` are filled. The server
    // validates against InputSchema() first (#423), but this stays defensive (it is
    // also reached directly from tests). Returns a human-readable error, or nullopt.
    [[nodiscard]] inline std::optional<std::string> ParseArgs(const Json& args, bool& isIntrospect, Setting& setting, i32& value)
    {
        const bool hasSetting = args.contains("setting") && !args["setting"].is_null();
        if (!hasSetting)
        {
            isIntrospect = true;
            // A value without a setting is a mistake — reject it rather than silently
            // listing everything and ignoring the value.
            if (args.contains("value") && !args["value"].is_null())
                return "Provide 'setting' when providing 'value' (omit both to list every setting).";
            return std::nullopt;
        }

        isIntrospect = false;
        if (!args["setting"].is_string())
            return "Invalid 'setting': expected a string.";
        const std::string settingStr = args["setting"].get<std::string>();
        if (!ParseSetting(settingStr, setting))
            return "Unknown setting '" + settingStr + "'. Valid settings: " + JoinSettingTokens() + ".";

        if (!args.contains("value") || args["value"].is_null())
            return "Missing required argument 'value' for setting '" + std::string(SettingToken(setting)) +
                   "'. Valid values: " + JoinValueTokens(setting) + ".";
        if (!args["value"].is_string())
            return "Invalid 'value': expected a string token.";
        const std::string valueStr = args["value"].get<std::string>();
        if (!ParseValue(setting, valueStr, value))
            return "Invalid value '" + valueStr + "' for setting '" + std::string(SettingToken(setting)) +
                   "'. Valid values: " + JoinValueTokens(setting) + ".";
        return std::nullopt;
    }

    // Outcome of applying a settings write. On success `Data` is the structured
    // result payload; `PathChanged` is true only when a RenderPath write actually
    // changed the path, signalling the handler to rebuild the render-graph topology
    // (Renderer3D::ApplyRendererSettings) — that renderer-bound call stays out of
    // this header.
    struct ApplyResult
    {
        bool Ok = false;
        std::string Error;
        Json Data;
        bool PathChanged = false;
    };

    // Apply `value` (an engine enum integer) to `setting`, mutating the matching
    // field of `pp` / `rs` and reporting the prior value so the change can be
    // restored by setting it back (restore-prior-value, no undo stack). MUST run on
    // the main (game) thread — the caller passes the live Renderer3D settings.
    // NOTE: the engine struct `OloEngine::RendererSettings` must be spelled fully
    // qualified here — unqualified `RendererSettings` would name THIS namespace
    // (OloEngine::MCP::RendererSettings), not the type.
    [[nodiscard]] inline ApplyResult Apply(Setting setting, i32 value, PostProcessSettings& pp, ::OloEngine::RendererSettings& rs)
    {
        ApplyResult result;
        i32 previous = 0;
        switch (setting)
        {
            case Setting::Upscale:
                previous = static_cast<i32>(pp.Upscale);
                pp.Upscale = static_cast<UpscaleMode>(value);
                break;
            case Setting::Tonemap:
                previous = static_cast<i32>(pp.Tonemap);
                pp.Tonemap = static_cast<TonemapOperator>(value);
                break;
            case Setting::RenderPath:
                previous = static_cast<i32>(rs.Path);
                rs.Path = static_cast<RenderingPath>(static_cast<u8>(value));
                break;
        }

        const bool changed = previous != value;
        if (setting == Setting::RenderPath)
            result.PathChanged = changed;

        result.Ok = true;
        result.Data = Json{
            { "setting", std::string(SettingToken(setting)) },
            { "previousValue", ValueToken(setting, previous) },
            { "value", ValueToken(setting, value) },
            { "changed", changed },
            // Restore hint: these are session-global settings, so a revert is just
            // this same tool with `value` = previousValue (no CommandHistory / Ctrl-Z
            // entry, unlike the entity field writes).
            { "restoreWith", ValueToken(setting, previous) },
        };
        return result;
    }

    // Introspection payload: every setting with its live current value and the full
    // allowed-value catalogue. The handler reads the live Renderer3D settings and
    // hands them here.
    [[nodiscard]] inline Json Describe(const PostProcessSettings& pp, const ::OloEngine::RendererSettings& rs)
    {
        Json arr = Json::array();
        for (const auto& info : kSettings)
        {
            i32 current = 0;
            switch (info.Id)
            {
                case Setting::Upscale:
                    current = static_cast<i32>(pp.Upscale);
                    break;
                case Setting::Tonemap:
                    current = static_cast<i32>(pp.Tonemap);
                    break;
                case Setting::RenderPath:
                    current = static_cast<i32>(rs.Path);
                    break;
            }

            Json values = Json::array();
            for (const auto& v : SettingValues(info.Id))
                values.push_back(Json{ { "token", std::string(v.Token) }, { "description", std::string(v.Description) } });

            arr.push_back(Json{
                { "setting", std::string(info.Token) },
                { "description", std::string(info.Description) },
                { "currentValue", ValueToken(info.Id, current) },
                { "values", std::move(values) },
            });
        }
        return Json{ { "settings", std::move(arr) } };
    }
} // namespace OloEngine::MCP::RendererSettings
