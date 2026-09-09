#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpProjectValidation.h"
#include "MCP/McpSchemaBuilder.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

// Whole-project validation: olo_project_validate (issue #1130, Epic H slice 2).
//
// One call that runs every per-domain validator the editor already has and
// returns one structured report, instead of four calls a caller has to remember
// to make, in the right order, and then merge by hand.
//
// It composes by INVOKING the standalone commands' handlers (the Collect*
// entry points in McpToolsCommon.h), never by re-deriving their verdicts — so
// "the validation slice reports the same problems the editor panels show" holds
// by construction. The reshaping into a single section vocabulary lives in
// MCP/McpProjectValidation.h, which is where the report's rules about
// incomplete answers are stated and unit tested.

namespace OloEngine::MCP
{
    namespace
    {
        namespace PV = OloEngine::MCP::ProjectValidation;

        // One section: its name, the command it runs, and one sentence on what
        // it covers. The extractor turns that command's payload into the common
        // problem shape; see McpProjectValidation.h for why each source needs
        // its own (they name their arrays differently).
        struct SectionSpec
        {
            const char* Name;
            const char* Source;
            const char* Subject;
            ToolResult (*Collect)(IAutomationHost&);
            Json (*Extract)(const Json&);
        };

        const SectionSpec kSections[] = {
            { "assets", "olo_assets_problems",
              "Registered assets that failed to load or are missing/invalid (what the Content Browser shows as a "
              "bad asset).",
              &CollectAssetProblems, &PV::ProblemsFromAssets },
            { "shaders", "olo_shader_errors",
              "Shaders whose last compile or link failed (what the Shader editor panel shows in red).",
              &CollectShaderProblems, &PV::ProblemsFromShaders },
            // The ring this reads is append-only for the session: ScriptErrorBuffer
            // has a Clear() with no call site anywhere in the tree, so an error
            // from a script that has since been fixed still counts. The subject
            // says so rather than the section quietly implying "right now" —
            // a stale `ok:false` a reader cannot explain is worse than a wordier
            // sentence.
            { "scripts", "olo_script_get_last_errors",
              "C#/Lua script errors recorded SINCE THE EDITOR STARTED (the ring the console renders; it is never "
              "cleared, so a fixed script's earlier error still counts).",
              &CollectScriptProblems, &PV::ProblemsFromScripts },
            { "renderGraph", "olo_render_validate",
              "The live render graph's compiled resource hazards, execute-path resolve failures and "
              "consumed-but-unbacked resources.",
              &CollectRenderGraphProblems, &PV::ProblemsFromRenderGraph },
        };

        // The section names, derived from kSections so the declared schema's enum
        // and the table dispatch actually walks cannot drift. A hand-restated
        // copy of a table goes stale silently — #702 shipped a renderer setting
        // that parsed, applied and described correctly while the schema gate
        // still rejected it, which reads as a missing feature rather than as an
        // un-updated list.
        [[nodiscard]] std::vector<std::string> SectionNames()
        {
            std::vector<std::string> names;
            names.reserve(std::size(kSections));
            for (const SectionSpec& spec : kSections)
                names.emplace_back(spec.Name);
            return names;
        }

        // An error result carries its reason as text; that text is what the
        // section reports as `reason`, so a caller learns WHY a validator could
        // not run rather than seeing the section vanish.
        [[nodiscard]] std::string ReasonFrom(const ToolResult& result)
        {
            if (result.Content.is_array() && !result.Content.empty())
            {
                const Json& block = result.Content.front();
                if (block.is_object() && block.contains("text") && block["text"].is_string())
                    return block["text"].get<std::string>();
            }
            return "The validator reported an error with no message.";
        }

        ToolResult Handle_ProjectValidate(IAutomationHost& host, const Json& args)
        {
            std::vector<std::string> requested;
            if (args.contains("sections"))
            {
                if (!args["sections"].is_array() || args["sections"].empty())
                    return ToolResult::Error("Invalid 'sections': expected a non-empty array of section names.");
                for (const Json& entry : args["sections"])
                {
                    if (!entry.is_string())
                        return ToolResult::Error("Invalid entry in 'sections': expected a section name string.");
                    const std::string name = entry.get<std::string>();
                    const bool known = std::any_of(std::begin(kSections), std::end(kSections),
                                                   [&name](const SectionSpec& spec) { return name == spec.Name; });
                    if (!known)
                    {
                        std::string valid;
                        for (const SectionSpec& spec : kSections)
                        {
                            if (!valid.empty())
                                valid += ", ";
                            valid += spec.Name;
                        }
                        // The declared schema's enum is generated from kSections
                        // (see SectionNames below), so dispatch normally rejects a
                        // bad name before reaching here. This stays as the
                        // enforcement rather than the schema: a typo'd section
                        // would otherwise narrow the check silently and still
                        // report `complete`, and a caller invoking through the
                        // registry with a loosened schema must get the same
                        // refusal the MCP adapter gives.
                        return ToolResult::Error("Unknown section '" + name + "'. Valid sections: " + valid + ".");
                    }
                    requested.push_back(name);
                }
            }

            const auto maxPerSection =
                static_cast<sizet>(std::clamp<long long>(args.value("maxPerSection", 50LL), 1LL, 500LL));

            std::vector<PV::Section> sections;
            for (const SectionSpec& spec : kSections)
            {
                if (!requested.empty() &&
                    std::find(requested.begin(), requested.end(), spec.Name) == requested.end())
                    continue;

                const ToolResult collected = spec.Collect(host);
                if (collected.IsError)
                {
                    sections.push_back(
                        PV::UnavailableSection(spec.Name, spec.Source, spec.Subject, ReasonFrom(collected)));
                    continue;
                }
                if (!collected.StructuredContent.is_object())
                {
                    sections.push_back(PV::UnavailableSection(
                        spec.Name, spec.Source, spec.Subject,
                        std::string(spec.Source) + " returned no structured payload to validate against."));
                    continue;
                }
                sections.push_back(PV::RanSection(spec.Name, spec.Source, spec.Subject,
                                                  spec.Extract(collected.StructuredContent)));
            }

            return ToolResult::Structured(PV::BuildReport(sections, maxPerSection));
        }
    } // namespace

    void RegisterValidationTools(AutomationRegistry& registry)
    {
        ToolDef tool;
        tool.Name = "olo_project_validate";
        tool.Toolset = "validation";
        tool.Title = "Validate the project";
        tool.DualAudienceContent = true;
        tool.Annotations = ReadOnlyAnnotations();
        tool.Description =
            "Run every project validator at once — asset registry problems, shader compile/link errors, recent "
            "script errors and the live render graph's hazard sweep — and return one structured report. Each "
            "section invokes the standalone command's own handler (olo_assets_problems, olo_shader_errors, "
            "olo_script_get_last_errors, olo_render_validate), so it reports exactly what the editor panels show. "
            "A validator that cannot run here (no project open, no render graph in a headless or 2D session) comes "
            "back as 'unavailable' with its reason and makes the report's 'ok' false — an unrun check is never a "
            "clean bill of health.";
        tool.InputSchema =
            Schema::Object()
                .Prop("sections", Schema::Array(Schema::String().EnumFrom(SectionNames()))
                                      .Desc("Which sections to run. Omit for all of them; an unknown name is an "
                                            "error, never a silently narrowed check."))
                .Prop("maxPerSection", Schema::Int().Min(1).Max(500).Desc(
                                           "Cap on problems listed per section (default 50). Any excess is counted "
                                           "in the section's 'omitted', never dropped from 'problemCount'."))
                .NoAdditional();
        tool.OutputSchema =
            Schema::Object()
                .Prop("ok", Schema::Bool().Desc("Every requested section ran AND found nothing."))
                .Prop("complete", Schema::Bool().Desc("Every requested section ran. False means the verdict is "
                                                      "partial — read each section's 'reason'."))
                .Prop("problemCount", Schema::Int().Min(0).Desc("Total problems across the sections that ran."))
                .Prop("sectionCount", Schema::Int().Min(1))
                .Prop("sectionsUnavailable", Schema::Int().Min(0))
                .Prop("sections",
                      Schema::Array(Schema::Object()
                                        .Prop("name", Schema::String())
                                        .Prop("source", Schema::String().Desc("The command that produced it."))
                                        .Prop("subject", Schema::String())
                                        .Prop("status", Schema::String().Enum({ "ok", "problems", "unavailable" }))
                                        .Prop("reason", Schema::String().Desc("Present only when unavailable."))
                                        .Prop("problemCount", Schema::Int().Min(0))
                                        .Prop("omitted", Schema::Int().Min(0))
                                        .Prop("problems", Schema::Array(Schema::Object().Desc(
                                                  "One problem; 'kind' says which validator classed it.")))))
                .Required({ "ok", "complete", "problemCount", "sectionCount", "sectionsUnavailable", "sections" });
        tool.Handler = Handle_ProjectValidate;
        registry.Register(std::move(tool));
    }
} // namespace OloEngine::MCP
