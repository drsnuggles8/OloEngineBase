#pragma once

// Which tools a session SEES, and what that costs in bytes (issue #1124).
//
// The rule: exposure is a LISTING concern only. Nothing here removes a tool, and
// nothing here affects dispatch — `tools/call` resolves against the full registry
// under every profile, so a client holding a tool name from a previous session, a
// prompt, or the docs keeps working after the default narrowed. That is the whole
// backward-compatibility story, and it is why the filter lives at the tools/list
// boundary rather than in the registry.
//
// Why it exists. `tools/list` grew from 66 tools / ~15k tokens (#673) to 93 / 265 009
// bytes / ~66k tokens — a third of a 200k context window spent before the session asks
// its first question, on a catalogue of which three or four entries are ever called.
// The growth is structural (~2.8 KB per new tool), so the fix has to be a cap on what
// is listed by default, not a one-off trim.
//
// Kept free of McpServer.h, httplib and the renderer — McpServer.h includes THIS — so
// the profile arithmetic unit-tests without a server, a window or a GL context
// (McpExposureProfileTest.cpp), the same split McpSchemaBuilder.h uses.

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP
{
    // How much of the tool surface `tools/list` advertises. Dispatch is unaffected by
    // all three — see the file header.
    enum class ExposureProfile : u8
    {
        // Default. The gateway plus a curated set of tools a session needs to orient
        // itself: see CoreToolNames() for the set and the criterion it was picked by.
        Core = 0,
        // Core plus every tool in the toolsets the host enabled (ExposurePolicy::Toolsets).
        // An empty enabled list therefore behaves exactly like Core rather than like
        // Full — "I selected no toolsets" must not silently mean "select everything".
        Toolset,
        // Everything, i.e. the behaviour before this profile existed.
        Full,
    };

    [[nodiscard]] constexpr std::string_view ToStringView(ExposureProfile profile)
    {
        switch (profile)
        {
            case ExposureProfile::Core:
                return "core";
            case ExposureProfile::Toolset:
                return "toolset";
            case ExposureProfile::Full:
                return "full";
        }
        return "core";
    }

    // Case-insensitive parse of a profile token; std::nullopt for anything else, so a
    // caller can report a typo rather than silently falling back (no-silent-fallbacks).
    [[nodiscard]] inline std::optional<ExposureProfile> ParseExposureProfile(std::string_view token)
    {
        std::string lowered;
        lowered.reserve(token.size());
        for (const char c : token)
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (lowered == "core")
            return ExposureProfile::Core;
        if (lowered == "toolset")
            return ExposureProfile::Toolset;
        if (lowered == "full")
            return ExposureProfile::Full;
        return std::nullopt;
    }

    // ---- the discovery gateway ------------------------------------------------
    //
    // Four tools that are listed under EVERY profile, because they are how a session
    // reaches everything the profile hid. Removing one from the listing would make the
    // narrowed default a dead end, so IsGatewayTool() is checked before the profile.
    inline constexpr std::string_view kGatewayCapabilityTool = "olo_capability";
    inline constexpr std::string_view kGatewaySearchTool = "olo_tool_search";
    inline constexpr std::string_view kGatewayDescribeTool = "olo_tool_describe";
    inline constexpr std::string_view kGatewayExecuteTool = "olo_tool_execute";

    [[nodiscard]] inline std::span<const std::string_view> GatewayToolNames()
    {
        static constexpr std::array kNames{ kGatewayCapabilityTool, kGatewaySearchTool, kGatewayDescribeTool,
                                            kGatewayExecuteTool };
        return kNames;
    }

    [[nodiscard]] inline bool IsGatewayTool(std::string_view name)
    {
        const auto names = GatewayToolNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // ---- the core set ---------------------------------------------------------
    //
    // THE CRITERION: a tool is core if a session that cannot see it would be unable to
    // work out what to ask next. That is orientation (what scene, which entities, what
    // just happened, is the editor even running frames), the two eyes on the frame
    // (screenshot + intermediate target capture) and the camera controls those need to
    // be useful, plus the two error channels an agent must check before believing any
    // of it (engine log, shader errors). Everything else — the 30-odd render
    // introspection tools, physics, perf, assets, scripting, the editor panels — is
    // reachable in one `olo_tool_search` call and is not needed to form the question.
    //
    // Deliberately NOT core: every write tool. A narrowed default should not advertise
    // mutation; a session that wants it is already configuring consent, and can ask.
    //
    // This list is central rather than a per-ToolDef flag so the decision is reviewable
    // in one place. The cost of that is drift when a tool is renamed, which
    // McpExposureProfileTest.cpp pins: every name here must resolve against the real
    // registered surface.
    [[nodiscard]] inline std::span<const std::string_view> CoreToolNames()
    {
        static constexpr std::array kNames{
            // orientation
            std::string_view{ "olo_scene_summary" },
            std::string_view{ "olo_scene_list_entities" },
            std::string_view{ "olo_scene_get_entity" },
            std::string_view{ "olo_events_tail" },
            // the subscription half of the tail (#1131): an agent that can list the
            // tail but not the wait polls, which is the loop the bus exists to end
            std::string_view{ "olo_events_wait" },
            // the two error channels
            std::string_view{ "olo_log_tail" },
            std::string_view{ "olo_shader_errors" },
            // eyes on the frame
            std::string_view{ "olo_screenshot" },
            std::string_view{ "olo_render_list_targets" },
            std::string_view{ "olo_render_capture_target" },
            // camera control, without which the two above only ever see one angle
            std::string_view{ "olo_camera_get" },
            std::string_view{ "olo_camera_set_pose" },
            // "is it actually running frames?" — a parked editor answers every read
            // tool with a stale frame, so this is the check that validates the rest
            std::string_view{ "olo_perf_snapshot" },
        };
        return kNames;
    }

    [[nodiscard]] inline bool IsCoreTool(std::string_view name)
    {
        const auto names = CoreToolNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // ---- the policy -----------------------------------------------------------

    // The facts one tool contributes to the listing decision. `UserProvided` covers a
    // project Lua script tool (ToolDef::ScriptOwned) and a bridged external tool
    // (ToolDef::ClientAlias): both exist only because this user configured them, there
    // are a handful at most, and hiding them would look like the configuration failed.
    // They are always listed.
    struct ToolExposureFacts
    {
        std::string_view Name;
        std::string_view Toolset;
        bool UserProvided = false;
    };

    struct ExposurePolicy
    {
        ExposureProfile Profile = ExposureProfile::Core;
        // Enabled toolsets, canonically lowercased. Meaningful only for Toolset.
        std::vector<std::string> Toolsets;

        [[nodiscard]] bool ShouldList(const ToolExposureFacts& tool) const
        {
            if (Profile == ExposureProfile::Full)
                return true;
            if (IsGatewayTool(tool.Name) || tool.UserProvided || IsCoreTool(tool.Name))
                return true;
            if (Profile != ExposureProfile::Toolset || tool.Toolset.empty())
                return false;

            std::string lowered;
            lowered.reserve(tool.Toolset.size());
            for (const char c : tool.Toolset)
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return std::find(Toolsets.begin(), Toolsets.end(), lowered) != Toolsets.end();
        }
    };

    // Split a comma/semicolon/whitespace-separated toolset list into canonical
    // lowercase names, dropping empties and duplicates. Order is the caller's; it only
    // ever feeds a membership test.
    [[nodiscard]] inline std::vector<std::string> ParseToolsetList(std::string_view list)
    {
        std::vector<std::string> out;
        std::string current;
        const auto flush = [&out, &current]
        {
            if (current.empty())
                return;
            if (std::find(out.begin(), out.end(), current) == out.end())
                out.push_back(current);
            current.clear();
        };
        for (const char c : list)
        {
            if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)) != 0)
                flush();
            else
                current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        flush();
        return out;
    }

    // ---- registry size metrics ------------------------------------------------

    // Tokens are reported as bytes/4 — the conversion #1124 measured the headline
    // number with (265 009 bytes ≈ 66k tokens). It is an ESTIMATE and labelled as one
    // everywhere it surfaces; the byte counts are exact and are what the regression
    // test asserts on.
    [[nodiscard]] constexpr sizet ApproxTokensForBytes(sizet bytes)
    {
        return (bytes + 3) / 4;
    }

    // What one `tools/list` costs, under the active profile and under `full`. Computed
    // by McpServer::ComputeRegistryMetrics() from the same serializer tools/list uses,
    // so ListedBytes is the exact payload size, not a model of it.
    struct ToolRegistryMetrics
    {
        ExposureProfile Profile = ExposureProfile::Core;
        sizet TotalTools = 0;
        sizet ListedTools = 0;
        sizet FullBytes = 0;   // tools/list under `full`
        sizet ListedBytes = 0; // tools/list under the active profile

        [[nodiscard]] sizet ApproxFullTokens() const
        {
            return ApproxTokensForBytes(FullBytes);
        }
        [[nodiscard]] sizet ApproxListedTokens() const
        {
            return ApproxTokensForBytes(ListedBytes);
        }
    };
} // namespace OloEngine::MCP
