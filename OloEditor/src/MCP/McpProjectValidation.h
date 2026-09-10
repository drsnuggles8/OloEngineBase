#pragma once

// Pure shaping for olo_project_validate (issue #1130, Epic H slice 2): one
// whole-project health check composed from the per-domain validators that
// already exist, with a structured report instead of four separate calls a
// caller has to remember to make and then merge.
//
// WHAT MAKES IT THE SAME ANSWER THE EDITOR GIVES. The composite does not
// re-derive anything. Each section INVOKES the standalone command's own handler
// (olo_assets_problems, olo_shader_errors, olo_script_get_last_errors,
// olo_render_validate) and reshapes what comes back, so the two can never drift
// — and those handlers read the very singletons the editor panels render from:
// the project asset registry, ShaderDebugger, the script-error ring, the live
// render graph. The functions here own only the reshaping.
//
// THE RULE THAT SHAPES THE REPORT: a validator that could not run is not a
// clean bill of health. A headless host has no render graph; a session with no
// project open has no asset manager. Either way the section comes back
// `unavailable` WITH ITS REASON, it is counted, and the report's `ok` is false
// — because the alternative, quietly dropping the section and totalling zero
// problems, is exactly the silent fallback that makes a green result worthless.
// `complete` says whether everything ran; `ok` says everything ran AND found
// nothing.
//
// Free functions over JSON with no editor, renderer or engine dependency, so
// they are unit tested headlessly (OloEngine/tests/MCP/McpProjectValidationTest.cpp).

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace OloEngine::MCP::ProjectValidation
{
    using Json = nlohmann::json;

    enum class SectionStatus : u8
    {
        Ok = 0,      // the validator ran and found nothing
        Problems,    // the validator ran and found something
        Unavailable, // the validator could not run here — Reason says why
    };

    [[nodiscard]] inline const char* SectionStatusName(SectionStatus status)
    {
        switch (status)
        {
            case SectionStatus::Ok:
                return "ok";
            case SectionStatus::Problems:
                return "problems";
            case SectionStatus::Unavailable:
                return "unavailable";
        }
        return "unknown";
    }

    struct Section
    {
        std::string Name;    // "assets", "shaders", "scripts", "renderGraph"
        std::string Source;  // the command whose handler produced this, verbatim
        std::string Subject; // one sentence: what this section checks
        SectionStatus Status = SectionStatus::Unavailable;
        std::string Reason; // why, when Unavailable
        Json Problems = Json::array();
    };

    // ---- per-source extraction ---------------------------------------------
    //
    // Each standalone command names its problem array differently (`problems`,
    // `errors`), and olo_render_validate reports four kinds across four arrays.
    // These normalize all of that into one array of objects that each carry a
    // `kind`, so a caller iterates one shape.

    [[nodiscard]] inline Json NamedArray(const Json& payload, const char* key)
    {
        if (!payload.is_object())
            return Json::array();
        const auto found = payload.find(key);
        if (found == payload.end() || !found->is_array())
            return Json::array();
        return *found;
    }

    // olo_assets_problems: {"count":n, "problems":[{handle,type,path,status}]}
    [[nodiscard]] inline Json ProblemsFromAssets(const Json& payload)
    {
        Json out = Json::array();
        for (const Json& entry : NamedArray(payload, "problems"))
        {
            Json problem = entry;
            problem["kind"] = "AssetLoadFailure";
            out.push_back(std::move(problem));
        }
        return out;
    }

    // olo_shader_errors: {"count":n, "errors":[{name,errorMessage}]}
    [[nodiscard]] inline Json ProblemsFromShaders(const Json& payload)
    {
        Json out = Json::array();
        for (const Json& entry : NamedArray(payload, "errors"))
        {
            Json problem = entry;
            problem["kind"] = "ShaderCompileError";
            out.push_back(std::move(problem));
        }
        return out;
    }

    // olo_script_get_last_errors: {"count":n, "errors":[{language,scriptName,message,...}]}
    [[nodiscard]] inline Json ProblemsFromScripts(const Json& payload)
    {
        Json out = Json::array();
        for (const Json& entry : NamedArray(payload, "errors"))
        {
            Json problem = entry;
            problem["kind"] = "ScriptError";
            out.push_back(std::move(problem));
        }
        return out;
    }

    // olo_render_validate's default sweep. Its own `ok` verdict rests on
    // hazards + resolveFailures + consumedButUnbacked, so those three are the
    // problems; the barrier and build diagnostics are informational there and
    // stay informational here rather than being promoted into failures.
    [[nodiscard]] inline Json ProblemsFromRenderGraph(const Json& payload)
    {
        Json out = Json::array();
        for (const Json& entry : NamedArray(payload, "hazards"))
        {
            Json problem = entry;
            problem["kind"] = "RenderGraphHazard";
            out.push_back(std::move(problem));
        }
        for (const Json& entry : NamedArray(payload, "resolveFailures"))
        {
            Json problem = entry;
            problem["kind"] = "RenderGraphResolveFailure";
            out.push_back(std::move(problem));
        }
        for (const Json& entry : NamedArray(payload, "consumedButUnbacked"))
        {
            Json problem;
            problem["kind"] = "ResourceConsumedButUnbacked";
            problem["resource"] = entry.is_string() ? entry.get<std::string>() : entry.dump();
            out.push_back(std::move(problem));
        }
        return out;
    }

    // ---- report assembly ---------------------------------------------------

    // Build a section that ran, from its extracted problem array.
    [[nodiscard]] inline Section RanSection(std::string name, std::string source, std::string subject, Json problems)
    {
        Section section;
        section.Name = std::move(name);
        section.Source = std::move(source);
        section.Subject = std::move(subject);
        section.Status = problems.empty() ? SectionStatus::Ok : SectionStatus::Problems;
        section.Problems = std::move(problems);
        return section;
    }

    [[nodiscard]] inline Section UnavailableSection(std::string name, std::string source, std::string subject,
                                                    std::string reason)
    {
        Section section;
        section.Name = std::move(name);
        section.Source = std::move(source);
        section.Subject = std::move(subject);
        section.Status = SectionStatus::Unavailable;
        section.Reason = std::move(reason);
        return section;
    }

    // Assemble the whole report. `maxPerSection` caps how many problems each
    // section lists; anything past it is COUNTED in `omitted`, never dropped
    // quietly — the count in `problemCount` always reflects everything found.
    [[nodiscard]] inline Json BuildReport(const std::vector<Section>& sections, sizet maxPerSection)
    {
        Json out;
        Json sectionArray = Json::array();
        sizet problemCount = 0;
        sizet unavailable = 0;

        for (const Section& section : sections)
        {
            Json entry;
            entry["name"] = section.Name;
            entry["source"] = section.Source;
            entry["subject"] = section.Subject;
            entry["status"] = SectionStatusName(section.Status);

            if (section.Status == SectionStatus::Unavailable)
            {
                ++unavailable;
                entry["reason"] = section.Reason;
                entry["problemCount"] = 0;
                entry["problems"] = Json::array();
                sectionArray.push_back(std::move(entry));
                continue;
            }

            const sizet found = section.Problems.size();
            problemCount += found;
            entry["problemCount"] = found;

            Json listed = Json::array();
            for (const Json& problem : section.Problems)
            {
                if (listed.size() >= maxPerSection)
                    break;
                listed.push_back(problem);
            }
            const sizet omitted = found - listed.size();
            entry["problems"] = std::move(listed);
            entry["omitted"] = omitted;
            if (omitted > 0)
                entry["truncated"] = true;
            sectionArray.push_back(std::move(entry));
        }

        out["sections"] = std::move(sectionArray);
        out["sectionCount"] = sections.size();
        out["sectionsUnavailable"] = unavailable;
        out["problemCount"] = problemCount;
        // `complete` and `ok` are deliberately two flags, not one. A caller that
        // only wants "is anything broken?" reads `ok`; a caller that has to know
        // whether the answer is trustworthy reads `complete` and, when it is
        // false, the per-section `reason`.
        out["complete"] = unavailable == 0;
        out["ok"] = unavailable == 0 && problemCount == 0;
        return out;
    }
} // namespace OloEngine::MCP::ProjectValidation
