#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpExposure.h"
#include "MCP/McpSchemaBuilder.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// The deferred capability-discovery gateway (issue #1124): four tools that stay
// listed under every exposure profile, so a session whose `tools/list` was narrowed
// to the core set can still reach the whole surface.
//
//   olo_capability     — what am I connected to, how big is the surface, what did
//                        this profile hide, and how do I get at it.
//   olo_tool_search    — find tools by keyword/toolset. Returns NAMES AND SUMMARIES,
//                        not schemas: that is the entire saving, since the schemas
//                        are the bulk of the 269 KB `tools/list` this issue is about.
//   olo_tool_describe  — the full tools/list entry for the two or three tools the
//                        search picked out, paid for one at a time.
//   olo_tool_execute   — run any registered tool by name.
//
// The three-call loop (capability → search → describe → call) costs a few KB where
// materialising all 96 schemas costs ~67k tokens.
//
// olo_tool_execute has no working handler here on purpose. McpServer::HandleToolsCall
// rewrites the call into a plain tools/call for the target BEFORE dispatch, so the
// target passes the same schema validation, write-consent gate and cancellation scope
// as a direct call. It is still REGISTERED, rather than injected into tools/list, so
// that one registry remains the single source of truth for what exists — the
// alternative is a tool the server advertises but cannot find, which breaks
// tools/search, the docs-coverage test and every registry-wide sweep. The handler
// below is the unreachable branch, and says so if it ever runs.

namespace OloEngine::MCP
{
    namespace
    {
        std::string ToLowerAscii(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // How much of a tool's description a search hit carries. Long enough to tell
        // two neighbouring tools apart, short enough that a 25-hit search stays a few
        // KB. The full text is one olo_tool_describe away, which is the division of
        // labour the gateway is built on: search to FIND, describe to USE.
        constexpr sizet kSearchSummaryChars = 240;

        std::string Summarize(const std::string& description, bool& outTruncated)
        {
            outTruncated = description.size() > kSearchSummaryChars;
            if (!outTruncated)
                return description;
            // Cut on a word boundary when there is one nearby, so the summary does not
            // end mid-identifier.
            sizet cut = kSearchSummaryChars;
            const sizet space = description.rfind(' ', cut);
            if (space != std::string::npos && space + 40 > kSearchSummaryChars)
                cut = space;
            // ...and never inside a UTF-8 sequence. nlohmann::json::dump() THROWS
            // (type_error.316) on invalid UTF-8, so a summary split mid-codepoint would
            // not produce a mangled character — it would turn every olo_tool_search that
            // matched that one tool into "Tool failed". Descriptions here are ASCII
            // today, which is exactly why this would sit unnoticed until the first
            // em-dash. Continuation bytes are 10xxxxxx; back off to the lead byte.
            while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0) == 0x80)
                --cut;
            return description.substr(0, cut) + "...";
        }

        // Same matching rule as tools/search (#385): whitespace-separated terms are
        // ANDed, each a case-insensitive substring of name + title + description +
        // toolset. Kept identical on purpose — an agent that learned one should not
        // have to learn the other.
        bool MatchesTerms(const ToolDef& tool, const std::vector<std::string>& terms)
        {
            if (terms.empty())
                return true;
            const std::string haystack =
                ToLowerAscii(tool.Name + ' ' + tool.Title + ' ' + tool.Description + ' ' + tool.Toolset);
            return std::all_of(terms.begin(), terms.end(),
                               [&haystack](const std::string& t)
                               { return haystack.find(t) != std::string::npos; });
        }

        std::vector<std::string> SplitTerms(const std::string& query)
        {
            std::vector<std::string> terms;
            std::istringstream stream(ToLowerAscii(query));
            std::string term;
            while (stream >> term)
                terms.push_back(std::move(term));
            return terms;
        }

        // Deliberately delegates rather than restating the discriminators: a second
        // copy of "what makes a tool user-provided" would let this `listed` flag drift
        // out of agreement with the tools/list filter itself, and nothing would fail.
        bool IsListedUnder(const ExposurePolicy& policy, const ToolDef& tool)
        {
            return policy.ShouldList(McpServer::ExposureFactsOf(tool));
        }

        // ---- olo_capability ----------------------------------------------------

        ToolResult Handle_Capability(McpServer& server, const Json& /*arguments*/)
        {
            // ONE snapshot for the whole report: a concurrent script-tool reload
            // between two ToolsSnapshot() calls would leave `toolsets` describing a
            // different registry from `registry`/`hiddenTools`, and the totals would
            // not reconcile.
            const McpServer::ToolSnapshot snapshot = server.ToolsSnapshot();
            const ExposurePolicy policy = server.GetExposurePolicy();
            const ToolRegistryMetrics metrics = server.ComputeRegistryMetrics(snapshot);

            // Toolset catalogue over the FULL registry — the point of this tool is to
            // describe what exists, including what the profile is not listing.
            std::map<std::string, std::pair<sizet, sizet>> byToolset; // name -> {total, listed}
            for (const auto& tool : *snapshot)
            {
                const std::string key = ToLowerAscii(tool.Toolset);
                if (key.empty())
                    continue;
                auto& [total, listed] = byToolset[key];
                ++total;
                if (IsListedUnder(policy, tool))
                    ++listed;
            }

            Json toolsets = Json::array();
            for (const auto& [name, counts] : byToolset)
                toolsets.push_back(Json{ { "name", name }, { "count", counts.first }, { "listed", counts.second } });

            Json coreTools = Json::array();
            for (const std::string_view name : CoreToolNames())
                coreTools.push_back(std::string(name));

            Json enabledToolsets = Json::array();
            for (const std::string& name : policy.Toolsets)
                enabledToolsets.push_back(name);

            Json result;
            result["profile"] = std::string(ToStringView(metrics.Profile));
            result["profiles"] = Json::array({ "core", "toolset", "full" });
            result["enabledToolsets"] = std::move(enabledToolsets);
            result["coreTools"] = std::move(coreTools);
            result["toolsets"] = std::move(toolsets);
            result["registry"] = Json{ { "totalTools", metrics.TotalTools },
                                       { "listedTools", metrics.ListedTools },
                                       { "listedBytes", metrics.ListedBytes },
                                       { "fullBytes", metrics.FullBytes },
                                       { "approxListedTokens", metrics.ApproxListedTokens() },
                                       { "approxFullTokens", metrics.ApproxFullTokens() },
                                       { "tokenEstimateBasis", "bytes / 4" } };
            result["hiddenTools"] = metrics.TotalTools - metrics.ListedTools;
            result["howTo"] =
                "tools/list shows the '" + std::string(ToStringView(metrics.Profile)) +
                "' profile only. Every OTHER registered tool is still callable by name — exposure filters the "
                "LISTING, never dispatch. To reach one: olo_tool_search {query} to find it, olo_tool_describe "
                "{names} for its input schema, then call it directly (or via olo_tool_execute {tool, arguments} "
                "if your client refuses to call an unlisted name). The host can widen the listing itself with "
                "OLO_MCP_TOOL_PROFILE=full (or =toolset with OLO_MCP_TOOLSETS=render,physics) before launching "
                "the editor.";
            return ToolResult::Structured(result);
        }

        // ---- olo_tool_search ---------------------------------------------------

        ToolResult Handle_ToolSearch(McpServer& server, const Json& arguments)
        {
            const McpServer::ToolSnapshot snapshot = server.ToolsSnapshot();
            const ExposurePolicy policy = server.GetExposurePolicy();

            const std::string toolsetFilter =
                arguments.contains("toolset") ? ToLowerAscii(arguments["toolset"].get<std::string>()) : std::string{};
            const std::vector<std::string> terms =
                arguments.contains("query") ? SplitTerms(arguments["query"].get<std::string>())
                                            : std::vector<std::string>{};
            const sizet limit = arguments.contains("limit")
                                    ? static_cast<sizet>(arguments["limit"].get<std::int64_t>())
                                    : sizet{ 25 };
            const bool includeSchemas = arguments.value("includeSchemas", false);

            // The search is over the FULL registry, never the profile-filtered view:
            // a narrowed tools/list that also narrowed its own escape hatch would be a
            // dead end. Each hit reports whether it is currently listed so an agent can
            // tell "hidden by the profile" from "does not exist".
            Json matchedTools = Json::array();
            std::map<std::string, sizet> toolsetCounts;
            sizet matchCount = 0;
            for (const auto& tool : *snapshot)
            {
                const std::string toolsetKey = ToLowerAscii(tool.Toolset);
                if (!toolsetKey.empty())
                    ++toolsetCounts[toolsetKey];
                if (!toolsetFilter.empty() && toolsetKey != toolsetFilter)
                    continue;
                if (!MatchesTerms(tool, terms))
                    continue;

                ++matchCount;
                if (matchedTools.size() >= limit)
                    continue;

                if (includeSchemas)
                {
                    Json entry = McpServer::BuildToolEntry(tool);
                    entry["listed"] = IsListedUnder(policy, tool);
                    matchedTools.push_back(std::move(entry));
                    continue;
                }

                bool truncated = false;
                Json entry{ { "name", tool.Name },
                            { "summary", Summarize(tool.Description, truncated) },
                            { "listed", IsListedUnder(policy, tool) } };
                if (!tool.Title.empty())
                    entry["title"] = tool.Title;
                if (!tool.Toolset.empty())
                    entry["toolset"] = tool.Toolset;
                if (truncated)
                    entry["summaryTruncated"] = true;
                // The one behavioural bit worth carrying at search time: whether calling
                // this needs the human's write consent. Everything else waits for
                // olo_tool_describe.
                if (tool.ProjectWrite)
                    entry["projectWrite"] = true;
                matchedTools.push_back(std::move(entry));
            }

            Json toolsets = Json::array();
            for (const auto& [name, count] : toolsetCounts)
                toolsets.push_back(Json{ { "name", name }, { "count", count } });

            Json result;
            result["matched"] = matchCount;
            result["returned"] = matchedTools.size();
            result["truncated"] = matchCount > matchedTools.size();
            result["tools"] = std::move(matchedTools);
            result["toolsets"] = std::move(toolsets);
            result["next"] = includeSchemas
                                 ? "Call a hit directly by name."
                                 : "olo_tool_describe {\"names\": [...]} for the full description and input schema.";
            return ToolResult::Structured(result);
        }

        // ---- olo_tool_describe -------------------------------------------------

        ToolResult Handle_ToolDescribe(McpServer& server, const Json& arguments)
        {
            const McpServer::ToolSnapshot snapshot = server.ToolsSnapshot();
            const ExposurePolicy policy = server.GetExposurePolicy();

            std::vector<std::string> wanted;
            if (arguments.contains("names") && arguments["names"].is_array())
            {
                // The declared schema already constrains these to strings and
                // ValidateArguments enforces it before the handler runs; the guard is
                // belt-and-braces against a future schema edit, not a second contract.
                for (const Json& name : arguments["names"])
                {
                    if (name.is_string())
                        wanted.push_back(name.get<std::string>());
                }
            }
            if (arguments.contains("name") && arguments["name"].is_string())
                wanted.push_back(arguments["name"].get<std::string>());
            if (wanted.empty())
                return ToolResult::Error("Give 'names' (an array of tool names) or 'name' (one). "
                                         "Use olo_tool_search to find them.");

            Json entries = Json::array();
            Json notFound = Json::array();
            for (const std::string& name : wanted)
            {
                const auto it = std::find_if(snapshot->begin(), snapshot->end(),
                                             [&name](const ToolDef& t)
                                             { return t.Name == name; });
                if (it == snapshot->end())
                {
                    notFound.push_back(name);
                    continue;
                }
                Json entry = McpServer::BuildToolEntry(*it);
                entry["listed"] = IsListedUnder(policy, *it);
                entries.push_back(std::move(entry));
            }

            Json result{ { "tools", std::move(entries) } };
            // Only when non-empty: a permanently-present empty array trains a reader to
            // skip the field, which is exactly when it matters.
            if (!notFound.empty())
            {
                result["notFound"] = std::move(notFound);
                result["hint"] = "No tool by that name is registered. olo_tool_search {\"query\": \"...\"} "
                                 "searches the full registry, including tools this profile does not list.";
            }
            return ToolResult::Structured(result);
        }

        ToolResult Handle_ToolExecuteUnreachable(McpServer& /*server*/, const Json& /*arguments*/)
        {
            // McpServer::HandleToolsCall intercepts this tool's name and rewrites the
            // call before dispatch, so reaching the handler means that interception was
            // removed or bypassed. Fail loudly rather than pretending to run something.
            return ToolResult::Error("Internal error: '" + std::string(kGatewayExecuteTool) +
                                     "' was dispatched to its handler instead of being rewritten into a "
                                     "tools/call for the target tool. Call the target tool directly.");
        }
    } // namespace

    void RegisterGatewayTools(McpServer& server)
    {
        {
            ToolDef tool;
            // Spelled out rather than built from kGatewayCapabilityTool: the docs
            // coverage ratchet (McpDocsCoverageTest) scans these sources for the
            // literal `tool.Name = "olo_..."`, so a computed name would silently opt
            // this tool out of the "is it in the guide?" check. The static_assert
            // keeps the literal and the constant from drifting apart.
            static_assert(kGatewayCapabilityTool == "olo_capability");
            tool.Name = "olo_capability";
            tool.Title = "Capability overview";
            tool.Toolset = "gateway";
            tool.Description =
                "What this MCP server exposes and what it is currently hiding: the active exposure profile, the "
                "toolset catalogue with per-toolset counts, and the measured size of tools/list under this profile "
                "versus the full surface. Start here — the default profile lists a curated core set, and this "
                "reports how to reach the rest (olo_tool_search / olo_tool_describe). Every registered tool stays "
                "callable by name regardless of profile.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("profile", Schema::String().Desc("Active exposure profile: core, toolset or full."))
                    .Prop("profiles", Schema::Array(Schema::String()).Desc("Every profile this server understands."))
                    .Prop("enabledToolsets",
                          Schema::Array(Schema::String()).Desc("Toolsets listed in addition to core (profile=toolset)."))
                    .Prop("coreTools", Schema::Array(Schema::String()).Desc("The curated core set, always listed."))
                    .Prop("toolsets", Schema::Array(Schema::Object()
                                                        .Prop("name", Schema::String())
                                                        .Prop("count", Schema::Int().Desc("Tools in this toolset."))
                                                        .Prop("listed", Schema::Int().Desc("How many this profile lists.")))
                                          .Desc("Toolset catalogue over the full registry."))
                    .Prop("registry", Schema::Object()
                                          .Prop("totalTools", Schema::Int())
                                          .Prop("listedTools", Schema::Int())
                                          .Prop("listedBytes", Schema::Int().Desc("Exact tools/list payload size now."))
                                          .Prop("fullBytes", Schema::Int().Desc("Exact size under profile=full."))
                                          .Prop("approxListedTokens", Schema::Int())
                                          .Prop("approxFullTokens", Schema::Int())
                                          .Prop("tokenEstimateBasis", Schema::String())
                                          .Desc("Registry size metrics. Byte counts are exact; token counts are "
                                                "an estimate at bytes/4."))
                    .Prop("hiddenTools", Schema::Int().Desc("Registered but not listed under this profile."))
                    .Prop("howTo", Schema::String().Desc("How to reach a tool this profile does not list."))
                    .Required({ "profile", "registry", "howTo" });
            tool.Annotations = ReadOnlyAnnotations();
            tool.Handler = Handle_Capability;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            static_assert(kGatewaySearchTool == "olo_tool_search");
            tool.Name = "olo_tool_search";
            tool.Title = "Search tools";
            tool.Toolset = "gateway";
            tool.Description =
                "Find tools across the FULL registry by keyword and/or toolset, including tools the active exposure "
                "profile does not list. Returns names and short summaries rather than input schemas — the schemas "
                "are the bulk of the catalogue, so fetch only the ones you need with olo_tool_describe. Query terms "
                "are ANDed and matched case-insensitively against name, title, description and toolset; each hit "
                "reports whether it is currently listed in tools/list.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("query", Schema::String().Desc("Free text; whitespace-separated terms are ANDed. "
                                                         "Omit to browse a whole toolset."))
                    .Prop("toolset", Schema::String().Desc("Restrict to one toolset (case-insensitive exact match). "
                                                           "olo_capability lists the catalogue."))
                    .Prop("limit", Schema::Int().Min(1).Max(200).Desc("Maximum hits to return (default 25)."))
                    .Prop("includeSchemas",
                          Schema::Bool().Desc("Return the full tools/list entry per hit instead of a summary. "
                                              "Expensive — prefer olo_tool_describe for the two or three you want."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("matched", Schema::Int().Desc("Tools matching the filters, before `limit`."))
                    .Prop("returned", Schema::Int())
                    .Prop("truncated", Schema::Bool().Desc("True when `limit` cut the result short."))
                    .Prop("tools", Schema::Array(Schema::Object()
                                                     .Prop("name", Schema::String())
                                                     .Prop("title", Schema::String())
                                                     .Prop("toolset", Schema::String())
                                                     .Prop("summary", Schema::String().Desc("Description, truncated."))
                                                     .Prop("summaryTruncated", Schema::Bool())
                                                     .Prop("listed", Schema::Bool().Desc("In tools/list under the "
                                                                                         "active profile?"))
                                                     .Prop("projectWrite", Schema::Bool().Desc("Needs write consent."))))
                    .Prop("toolsets", Schema::Array(Schema::Object()
                                                        .Prop("name", Schema::String())
                                                        .Prop("count", Schema::Int()))
                                          .Desc("Full toolset catalogue, regardless of the active filter."))
                    .Prop("next", Schema::String())
                    .Required({ "matched", "returned", "tools" });
            tool.Annotations = ReadOnlyAnnotations();
            tool.Handler = Handle_ToolSearch;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            static_assert(kGatewayDescribeTool == "olo_tool_describe");
            tool.Name = "olo_tool_describe";
            tool.Title = "Describe tools";
            tool.Toolset = "gateway";
            tool.Description =
                "The full tools/list entry — description, inputSchema, outputSchema, annotations — for the named "
                "tools, whether or not the active exposure profile lists them. This is the paid-per-tool half of "
                "the discovery loop: olo_tool_search to find candidates, olo_tool_describe for the one or two you "
                "will actually call. The entries are byte-identical to what tools/list emits, so a tool can be "
                "called straight from a describe hit.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("names", Schema::Array(Schema::String()).MinItems(1).MaxItems(25).Desc(
                                       "Tool names to describe, e.g. [\"olo_shader_errors\"]."))
                    .Prop("name", Schema::String().Desc("Convenience single-name form; combines with 'names'."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("tools", Schema::Array(Schema::Object().Desc("A tools/list entry, plus a `listed` flag."))
                                       .Desc("One entry per resolved name, in the order asked."))
                    .Prop("notFound", Schema::Array(Schema::String()).Desc("Names with no registered tool. "
                                                                           "Omitted when every name resolved."))
                    .Prop("hint", Schema::String())
                    .Required({ "tools" });
            tool.Annotations = ReadOnlyAnnotations();
            tool.Handler = Handle_ToolDescribe;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            static_assert(kGatewayExecuteTool == "olo_tool_execute");
            tool.Name = "olo_tool_execute";
            tool.Title = "Execute a tool by name";
            tool.Toolset = "gateway";
            tool.Description =
                "Run any registered tool by name, including one the active exposure profile does not list. This is "
                "an alias for a normal tools/call — the target's input schema, write-consent gate and cancellation "
                "behaviour all apply unchanged, and the result is the target's result verbatim. You only need it if "
                "your MCP client refuses to call a name it did not see in tools/list; otherwise call the tool "
                "directly. Get the target's argument shape from olo_tool_describe first.";
            tool.InputSchema =
                Schema::Object()
                    .Prop("tool", Schema::String().Desc("Name of the tool to run, e.g. \"olo_shader_errors\"."))
                    .Prop("arguments", Schema::Object().Desc("Arguments for the target tool (default {})."))
                    .Required({ "tool" });
            // Deliberately NOT .NoAdditional(): this tool's arguments are never run
            // through ValidateArguments (HandleToolsCall rewrites the call before it
            // gets there), so declaring a closed object would advertise a strictness
            // nothing enforces. RewriteGatewayExecuteParams checks what it needs and
            // ignores the rest, which is what an open object says.
            // No OutputSchema: the result IS the target tool's result, so any schema
            // declared here would be a claim about 90-odd different shapes. Listed in
            // McpOutputSchemaCoverageTest's exemptions with that reason.
            //
            // readOnlyHint stays false and destructiveHint keeps its spec default of
            // true: this dispatches an arbitrary tool, so a client must not
            // auto-approve it on the gateway's hints — the target's own annotations
            // are what describe the actual call, and are visible via
            // olo_tool_describe.
            tool.Annotations = Json{ { "readOnlyHint", false }, { "openWorldHint", false } };
            // NOT ProjectWrite: the gate belongs to the TARGET, and HandleToolsCall
            // applies it after the rewrite. Setting it here would additionally refuse
            // a read-only tool routed through the gateway whenever writes are off.
            tool.Handler = Handle_ToolExecuteUnreachable;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
