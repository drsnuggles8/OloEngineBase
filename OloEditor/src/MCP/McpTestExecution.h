#pragma once

// Pure parsing, classification and reconciliation for the structured test-run
// commands olo_tests_list / olo_tests_run (issue #1130).
//
// The agent loop "change something, run the thing that proves it, read a
// structured result" was closed: the only way to run OloEngine-Tests was to
// shell out and scrape console text. These commands run the SAME binary but
// take their answer from gtest's own JSON report (`--gtest_output=json:`),
// which carries a per-case status, timing, source file/line and the verbatim
// failure text — so no caller has to parse `[  FAILED  ]` banners.
//
// THE INVARIANT THIS FILE EXISTS FOR: a test run must never be silently
// partial. Three things can make it one, and each is an error rather than a
// green run with a small number in it:
//
//   1. The selection matched no cases at all (a typo'd filter reads as "this
//      area is clean" otherwise).
//   2. The child died — crashed, was killed on timeout, was cancelled — so
//      gtest never wrote its report, or wrote a truncated one.
//   3. The report is intact but does not account for every case the selection
//      named. Reconcile() diffs the two sets by name and NAMES what is missing;
//      a count alone cannot tell "nothing ran" from "everything passed".
//
// Point 3 is why every run does a LIST pass first: the list is the expected
// set, and without it there is nothing to reconcile against. gtest's own
// top-level `tests` field cannot serve — in list mode it reports the whole
// registered count, ignoring the filter entirely (7616 for a one-suite
// selection), so reading it as "how many I selected" would be wrong by three
// orders of magnitude.
//
// Everything here is free functions over PODs with NO editor, renderer or
// process dependency — only nlohmann::json and the stdlib — so it is unit
// tested headlessly (OloEngine/tests/MCP/McpTestExecutionTest.cpp). The
// process spawning, binary discovery and file reads live in the handler
// (MCP/McpToolsTesting.cpp); this is the same split McpRenderValidate.h /
// McpFrameBreakdown.h use.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine::MCP::TestExecution
{
    using Json = nlohmann::json;

    // The gtest filter is passed on the child's command line, and Windows caps
    // one command line at 32767 characters including the executable path. A
    // selection expanded to explicit case names (what `layer` does) can blow
    // through that — 3000 unit-test names is ~135 KB — so it is checked against
    // this ceiling and REFUSED with the arithmetic rather than truncated into a
    // filter that silently runs a subset.
    inline constexpr sizet kMaxFilterChars = 30000;

    // ---- classification (test_catalogue.json + the in-file marker) ---------

    // What an in-file `// OLO_TEST_LAYER: <id>` scan found. Mirrors
    // generate_test_catalogue.py's extract_layer_marker: repeated IDENTICAL
    // markers are tolerated, two different ones are a conflict and classify
    // nothing.
    struct LayerMarker
    {
        std::string Layer;     // the marker's value; empty when the file carries none
        bool Conflict = false; // two or more markers disagreed
    };

    // Scan a test file's text for the marker. Recognizes the same shape the
    // generator's regex does — `^[ \t]*//[ \t]*OLO_TEST_LAYER:[ \t]*([A-Za-z0-9_]+)`,
    // anywhere in the file, not only near the top.
    [[nodiscard]] inline LayerMarker ExtractLayerMarker(std::string_view fileText)
    {
        LayerMarker found;
        sizet cursor = 0;
        while (cursor <= fileText.size())
        {
            const sizet lineEnd = std::min(fileText.find('\n', cursor), fileText.size());
            std::string_view line = fileText.substr(cursor, lineEnd - cursor);
            cursor = lineEnd + 1;

            sizet i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '/')
                continue;
            i += 2;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;

            constexpr std::string_view kKey = "OLO_TEST_LAYER:";
            if (line.substr(i, kKey.size()) != kKey)
                continue;
            i += kKey.size();
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;

            const sizet valueStart = i;
            while (i < line.size() &&
                   ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
                    (line[i] >= '0' && line[i] <= '9') || line[i] == '_'))
                ++i;
            if (i == valueStart)
                continue; // `// OLO_TEST_LAYER:` with no id is not a marker

            const std::string value(line.substr(valueStart, i - valueStart));
            if (found.Layer.empty())
                found.Layer = value;
            else if (found.Layer != value)
            {
                found.Conflict = true;
                found.Layer.clear();
                return found; // conflicting markers classify nothing, as in the generator
            }
        }
        return found;
    }

    // A file's resolved layer, plus why it has none. `Problem` is a sentence for
    // a human; it is REPORTED, never swallowed — an unclassified test file is
    // exactly the blind spot the classification gate exists to prevent, and a
    // command that quietly labelled it "" would hide it again.
    struct LayerResolution
    {
        std::string Layer;
        std::string Problem;
    };

    // Merge the two classification mechanisms under the repo's rule (see
    // generate_test_catalogue.py::classify_files): a file uses an in-file marker
    // OR a `file_layer_map` entry, never both; the marker wins where present;
    // an unknown id and an unclassified file are both errors.
    //
    // `fileLayerMap` is test_catalogue.json's `file_layer_map` object and
    // `knownIds` its layer ids (plus the unit / Functional tags).
    [[nodiscard]] inline LayerResolution ResolveLayer(std::string_view repoRelativeFile, const LayerMarker& marker,
                                                      const Json& fileLayerMap, const std::set<std::string>& knownIds)
    {
        LayerResolution out;
        const std::string key(repoRelativeFile);

        if (marker.Conflict)
        {
            out.Problem = key + ": multiple conflicting // OLO_TEST_LAYER markers.";
            return out;
        }

        std::string mapped;
        if (fileLayerMap.is_object())
        {
            if (const auto it = fileLayerMap.find(key); it != fileLayerMap.end() && it->is_string())
                mapped = it->get<std::string>();
        }

        // Reported when a file carries BOTH mechanisms. It still classifies by
        // the marker (the file IS classified; the duplicate registration is the
        // defect), but it falls through to the same range check below rather
        // than returning early — an unknown id must be rejected here exactly as
        // it is on the single-mechanism path, or the two would disagree about
        // what a valid layer is.
        if (!marker.Layer.empty() && !mapped.empty())
        {
            out.Problem = key + ": has an // OLO_TEST_LAYER:" + marker.Layer +
                          " marker AND a file_layer_map entry - remove the JSON entry (the marker supersedes it).";
        }

        out.Layer = !marker.Layer.empty() ? marker.Layer : mapped;
        if (out.Layer.empty())
        {
            out.Problem = key + ": unclassified test file (no // OLO_TEST_LAYER marker and no file_layer_map entry).";
            return out;
        }
        if (!knownIds.empty() && !knownIds.contains(out.Layer))
        {
            out.Problem = key + ": layer '" + out.Layer + "' is not a known id in test_catalogue.json.";
            out.Layer.clear();
        }
        return out;
    }

    // The layer ids test_catalogue.json declares, plus the two tags the
    // generator adds implicitly (`unit`, and `functional_tag`, normally
    // "Functional"). An empty set means "do not range-check", which is what a
    // caller with no catalogue gets.
    [[nodiscard]] inline std::set<std::string> KnownLayerIds(const Json& catalogue)
    {
        std::set<std::string> ids;
        if (!catalogue.is_object())
            return ids;
        if (const auto layers = catalogue.find("layers"); layers != catalogue.end() && layers->is_array())
        {
            for (const auto& layer : *layers)
            {
                if (layer.is_object() && layer.contains("id") && layer["id"].is_string())
                    ids.insert(layer["id"].get<std::string>());
            }
        }
        if (ids.empty())
            return ids; // no declared layers => no range check at all
        ids.insert("unit");
        ids.insert(catalogue.value("functional_tag", std::string("Functional")));
        return ids;
    }

    // Normalize gtest's `file` field to the repo-relative key `file_layer_map`
    // is written in. gtest reports `__FILE__` exactly as the compiler saw it,
    // which under a CMake build tree is a path relative to the BUILD directory
    // with native separators: "..\\OloEngine\\tests\\Foo\\BarTest.cpp". Both the
    // `..` prefixes and the separators have to go, so the anchor is the test
    // root substring rather than any kind of path arithmetic — that also copes
    // with an absolute `__FILE__` from a different generator.
    //
    // Returns empty when the path does not lie under `testRoot`, which the
    // caller reports rather than guessing a key.
    [[nodiscard]] inline std::string NormalizeTestFile(std::string_view rawPath, std::string_view testRoot)
    {
        std::string normalized(rawPath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (testRoot.empty())
            return normalized;

        std::string anchor(testRoot);
        std::replace(anchor.begin(), anchor.end(), '\\', '/');
        while (!anchor.empty() && anchor.back() == '/')
            anchor.pop_back();
        anchor.push_back('/');

        const sizet at = normalized.find(anchor);
        if (at == std::string::npos)
            return {};
        return normalized.substr(at);
    }

    // ---- the selected set (a `--gtest_list_tests` report) ------------------

    struct TestCase
    {
        std::string Suite;
        std::string Name;
        std::string File; // repo-relative; empty when it could not be normalized
        int Line = 0;
        std::string Layer; // "" when unclassified — the report says which
        bool Disabled = false;

        [[nodiscard]] std::string FullName() const
        {
            return Suite + "." + Name;
        }
    };

    // True for a name gtest treats as disabled: the DISABLED_ prefix on either
    // half of the full name. Such a case is still LISTED and still appears in a
    // run report (status NOTRUN / result SUPPRESSED), so it reconciles like any
    // other — it just never executes.
    [[nodiscard]] inline bool IsDisabledName(std::string_view suite, std::string_view name)
    {
        constexpr std::string_view kPrefix = "DISABLED_";
        return suite.substr(0, kPrefix.size()) == kPrefix || name.substr(0, kPrefix.size()) == kPrefix;
    }

    // Parse the document `--gtest_list_tests --gtest_output=json:<file>` writes.
    // Sets `error` and returns empty when the document is not that shape — a
    // half-written file from a child that died mid-flush must not read as an
    // empty selection.
    [[nodiscard]] inline std::vector<TestCase> ParseListReport(const Json& doc, std::string& error)
    {
        error.clear();
        std::vector<TestCase> cases;
        if (!doc.is_object() || !doc.contains("testsuites") || !doc["testsuites"].is_array())
        {
            error = "gtest list report is missing its 'testsuites' array.";
            return cases;
        }
        for (const auto& suite : doc["testsuites"])
        {
            if (!suite.is_object() || !suite.contains("name") || !suite["name"].is_string())
                continue;
            const std::string suiteName = suite["name"].get<std::string>();
            if (!suite.contains("testsuite") || !suite["testsuite"].is_array())
                continue;
            for (const auto& entry : suite["testsuite"])
            {
                if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
                    continue;
                TestCase testCase;
                testCase.Suite = suiteName;
                testCase.Name = entry["name"].get<std::string>();
                if (entry.contains("file") && entry["file"].is_string())
                    testCase.File = entry["file"].get<std::string>();
                if (entry.contains("line") && entry["line"].is_number_integer())
                    testCase.Line = entry["line"].get<int>();
                testCase.Disabled = IsDisabledName(testCase.Suite, testCase.Name);
                cases.push_back(std::move(testCase));
            }
        }
        return cases;
    }

    // ---- the run report ----------------------------------------------------

    enum class CaseStatus : u8
    {
        Passed = 0,
        Failed,
        Skipped,  // GTEST_SKIP() at runtime — the GPU-absent guard every visual test uses
        Disabled, // never ran: DISABLED_ prefix (gtest status NOTRUN / result SUPPRESSED)
    };

    [[nodiscard]] inline const char* CaseStatusName(CaseStatus status)
    {
        switch (status)
        {
            case CaseStatus::Passed:
                return "passed";
            case CaseStatus::Failed:
                return "failed";
            case CaseStatus::Skipped:
                return "skipped";
            case CaseStatus::Disabled:
                return "disabled";
        }
        return "unknown";
    }

    struct CaseResult
    {
        std::string Suite;
        std::string Name;
        std::string File;
        int Line = 0;
        std::string Layer;
        CaseStatus Status = CaseStatus::Passed;
        f64 Seconds = 0.0;
        // Verbatim gtest failure text (one entry per failed assertion), or the
        // GTEST_SKIP reason for a skip. This is the whole point of the command:
        // the caller reads the message instead of scraping the console.
        std::vector<std::string> Messages;

        [[nodiscard]] std::string FullName() const
        {
            return Suite + "." + Name;
        }
    };

    // gtest writes a duration as "17.444s" / "0s". Returns 0 for anything else
    // rather than throwing — a malformed duration is cosmetic, and the run's
    // correctness never rests on it.
    [[nodiscard]] inline f64 ParseGTestDuration(std::string_view text)
    {
        if (!text.empty() && text.back() == 's')
            text.remove_suffix(1);
        if (text.empty())
            return 0.0;
        try
        {
            sizet consumed = 0;
            const f64 value = std::stod(std::string(text), &consumed);
            if (consumed != text.size() || !std::isfinite(value) || value < 0.0)
                return 0.0;
            return value;
        }
        catch (...)
        {
            return 0.0;
        }
    }

    // A failure gtest recorded OUTSIDE any test case — a fatal assertion in
    // SetUpTestSuite / TearDownTestSuite, or in the global environment. gtest
    // reports these as pseudo-entries with an EMPTY `name`, either inside the
    // owning suite's array or under a synthetic `NonTestSuiteFailure` suite.
    //
    // They must be kept out of the case list. Read as cases they carry the full
    // name "Suite." — which no listing ever contains — so reconciliation would
    // call them `unexpected` and report a genuine SetUpTestSuite failure as
    // "the run was PARTIAL", burying the real cause under a plumbing complaint.
    struct SuiteFailure
    {
        std::string Suite; // "NonTestSuiteFailure" for a global-environment failure
        std::vector<std::string> Messages;
    };

    // The synthetic suite gtest invents for a failure that belongs to no suite.
    inline constexpr std::string_view kNonTestSuiteFailure = "NonTestSuiteFailure";

    struct RunReport
    {
        std::vector<CaseResult> Cases;
        std::vector<SuiteFailure> SuiteFailures;
        std::string Error; // non-empty => the document was not the shape gtest emits
    };

    // Parse the document `--gtest_output=json:<file>` writes after a run.
    [[nodiscard]] inline RunReport ParseRunReport(const Json& doc)
    {
        RunReport report;
        if (!doc.is_object() || !doc.contains("testsuites") || !doc["testsuites"].is_array())
        {
            report.Error = "gtest run report is missing its 'testsuites' array.";
            return report;
        }
        for (const auto& suite : doc["testsuites"])
        {
            if (!suite.is_object() || !suite.contains("name") || !suite["name"].is_string())
                continue;
            const std::string suiteName = suite["name"].get<std::string>();
            if (!suite.contains("testsuite") || !suite["testsuite"].is_array())
                continue;
            for (const auto& entry : suite["testsuite"])
            {
                if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
                    continue;

                // A suite-level failure, not a case. See SuiteFailure.
                if (const std::string entryName = entry["name"].get<std::string>();
                    entryName.empty() || suiteName == kNonTestSuiteFailure)
                {
                    SuiteFailure suiteFailure;
                    suiteFailure.Suite = suiteName;
                    if (const auto failures = entry.find("failures");
                        failures != entry.end() && failures->is_array())
                    {
                        for (const auto& failure : *failures)
                        {
                            if (failure.is_object() && failure.contains("failure") &&
                                failure["failure"].is_string())
                                suiteFailure.Messages.push_back(failure["failure"].get<std::string>());
                        }
                    }
                    report.SuiteFailures.push_back(std::move(suiteFailure));
                    continue;
                }

                CaseResult result;
                result.Suite = suiteName;
                result.Name = entry["name"].get<std::string>();
                if (entry.contains("file") && entry["file"].is_string())
                    result.File = entry["file"].get<std::string>();
                if (entry.contains("line") && entry["line"].is_number_integer())
                    result.Line = entry["line"].get<int>();
                if (entry.contains("time") && entry["time"].is_string())
                    result.Seconds = ParseGTestDuration(entry["time"].get<std::string>());

                const std::string status = entry.value("status", std::string("RUN"));
                const std::string outcome = entry.value("result", std::string("COMPLETED"));
                if (status != "RUN" || outcome == "SUPPRESSED")
                    result.Status = CaseStatus::Disabled;
                else if (outcome == "SKIPPED")
                    result.Status = CaseStatus::Skipped;
                else
                    result.Status = CaseStatus::Passed;

                if (const auto failures = entry.find("failures");
                    failures != entry.end() && failures->is_array() && !failures->empty())
                {
                    // A failure recorded against a case outranks its `result`
                    // field: gtest reports a test that both skipped and failed
                    // as SKIPPED, and calling that anything but a failure would
                    // lose a real red.
                    result.Status = CaseStatus::Failed;
                    for (const auto& failure : *failures)
                    {
                        if (failure.is_object() && failure.contains("failure") && failure["failure"].is_string())
                            result.Messages.push_back(failure["failure"].get<std::string>());
                    }
                }
                else if (const auto skipped = entry.find("skipped");
                         skipped != entry.end() && skipped->is_array())
                {
                    for (const auto& skip : *skipped)
                    {
                        if (skip.is_object() && skip.contains("message") && skip["message"].is_string())
                            result.Messages.push_back(skip["message"].get<std::string>());
                    }
                }

                report.Cases.push_back(std::move(result));
            }
        }
        return report;
    }

    // ---- reconciliation: the "never silently partial" gate -----------------

    struct Reconciliation
    {
        std::vector<std::string> Missing;    // selected by the filter, absent from the report
        std::vector<std::string> Unexpected; // in the report, never selected
        sizet Expected = 0;
        sizet Reported = 0;

        [[nodiscard]] bool Complete() const
        {
            return Missing.empty() && Unexpected.empty() && Expected == Reported;
        }
    };

    // Diff the selected set against the reported one BY NAME. A count-only
    // check passes a run that lost one case and gained another, and gives a
    // caller nothing to act on when it does fail; the names are what turn "the
    // run was partial" into "these 3 cases never reported, the last one to
    // start was X".
    [[nodiscard]] inline Reconciliation Reconcile(const std::vector<TestCase>& selected,
                                                  const std::vector<CaseResult>& reported)
    {
        Reconciliation out;
        out.Expected = selected.size();
        out.Reported = reported.size();

        std::set<std::string> reportedNames;
        for (const auto& result : reported)
            reportedNames.insert(result.FullName());

        std::set<std::string> selectedNames;
        for (const auto& testCase : selected)
        {
            const std::string full = testCase.FullName();
            selectedNames.insert(full);
            if (!reportedNames.contains(full))
                out.Missing.push_back(full);
        }
        for (const auto& result : reported)
        {
            if (const std::string full = result.FullName(); !selectedNames.contains(full))
                out.Unexpected.push_back(full);
        }
        return out;
    }

    // ---- filter assembly ---------------------------------------------------

    // gtest filter tokens carry no escaping mechanism, so a name outside this
    // set cannot be expressed in one at all. Every name gtest itself generates
    // is within it (suite/case identifiers, the `Instantiation/Suite.Case/0`
    // shape of a parameterized test). A name that is not is REFUSED by the
    // caller rather than pasted into a filter that would silently select
    // something else.
    [[nodiscard]] inline bool IsFilterSafeName(std::string_view name)
    {
        if (name.empty())
            return false;
        return std::all_of(name.begin(), name.end(),
                           [](char c)
                           {
                               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                      c == '_' || c == '.' || c == '/';
                           });
    }

    // A whole gtest FILTER EXPRESSION, which additionally carries the wildcard
    // and negation punctuation a bare name may not: `*` `?` `:` `-`.
    //
    // This is the one selection input a caller writes freehand, and it is pasted
    // into the child's command line. A `"` in it would close the quoting and let
    // the rest be read as further arguments — including a second
    // `--gtest_output`, which redirects the report and makes the run fail as
    // "exited without writing a report". So the charset is checked rather than
    // the quoting made clever: there is no escape syntax inside a gtest filter
    // anyway, so nothing legitimate is being refused.
    [[nodiscard]] inline bool IsFilterSafeExpression(std::string_view filter)
    {
        if (filter.empty())
            return false;
        return std::all_of(filter.begin(), filter.end(),
                           [](char c)
                           {
                               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                      c == '_' || c == '.' || c == '/' || c == '*' || c == '?' || c == ':' ||
                                      c == '-';
                           });
    }

    // Join full test names into one gtest positive filter.
    [[nodiscard]] inline std::string JoinFilter(const std::vector<std::string>& fullNames)
    {
        std::string filter;
        for (const auto& name : fullNames)
        {
            if (!filter.empty())
                filter.push_back(':');
            filter += name;
        }
        return filter;
    }

    // ---- console text (progress + the tail that explains a dead child) -----

    // How many cases the child has STARTED, counted from gtest's `[ RUN      ]`
    // banners in the captured console text. Progress only: the results
    // themselves always come from the JSON report, never from this.
    [[nodiscard]] inline sizet CountStartedCases(std::string_view consoleText)
    {
        constexpr std::string_view kBanner = "[ RUN      ]";
        sizet count = 0;
        for (sizet at = consoleText.find(kBanner); at != std::string_view::npos;
             at = consoleText.find(kBanner, at + kBanner.size()))
            ++count;
        return count;
    }

    // The last `maxChars` of `text`, marked when anything was dropped. This is
    // what a caller reads when the child died before writing a report, so it
    // keeps the END of the log — where the crash is — not the start.
    [[nodiscard]] inline std::string TailOf(std::string_view text, sizet maxChars)
    {
        if (maxChars == 0 || text.size() <= maxChars)
            return std::string(text);
        return "...[" + std::to_string(text.size() - maxChars) + " earlier characters omitted]...\n" +
               std::string(text.substr(text.size() - maxChars));
    }

    // Clamp one failure message, marking the cut. A silently truncated
    // assertion message reads as a complete one that simply did not say much.
    [[nodiscard]] inline std::string Clamp(std::string_view text, sizet maxChars)
    {
        if (maxChars == 0 || text.size() <= maxChars)
            return std::string(text);
        return std::string(text.substr(0, maxChars)) + "\n...[truncated, " + std::to_string(text.size() - maxChars) +
               " more characters]";
    }

    // ---- shaping -----------------------------------------------------------

    [[nodiscard]] inline Json CaseJson(const CaseResult& result, sizet maxMessageChars)
    {
        Json entry;
        entry["suite"] = result.Suite;
        entry["name"] = result.Name;
        entry["fullName"] = result.FullName();
        entry["status"] = CaseStatusName(result.Status);
        entry["seconds"] = result.Seconds;
        if (!result.File.empty())
        {
            entry["file"] = result.File;
            entry["line"] = result.Line;
        }
        if (!result.Layer.empty())
            entry["layer"] = result.Layer;
        if (!result.Messages.empty())
        {
            Json messages = Json::array();
            for (const auto& message : result.Messages)
                messages.push_back(Clamp(message, maxMessageChars));
            entry["messages"] = std::move(messages);
        }
        return entry;
    }

    struct RunCounts
    {
        sizet Total = 0;
        sizet Passed = 0;
        sizet Failed = 0;
        sizet Skipped = 0;
        sizet Disabled = 0;

        // The four buckets are exhaustive by construction, so a total that does
        // not add up means the shaping lost a case — a bug, not a test result,
        // and the caller reports it as one.
        [[nodiscard]] bool AddsUp() const
        {
            return Passed + Failed + Skipped + Disabled == Total;
        }
    };

    [[nodiscard]] inline RunCounts CountCases(const std::vector<CaseResult>& cases)
    {
        RunCounts counts;
        counts.Total = cases.size();
        for (const auto& result : cases)
        {
            switch (result.Status)
            {
                case CaseStatus::Passed:
                    ++counts.Passed;
                    break;
                case CaseStatus::Failed:
                    ++counts.Failed;
                    break;
                case CaseStatus::Skipped:
                    ++counts.Skipped;
                    break;
                case CaseStatus::Disabled:
                    ++counts.Disabled;
                    break;
            }
        }
        return counts;
    }

    // Cases grouped by their OLO_TEST_LAYER classification, for a caller that
    // wants "how much of the renderer pyramid did this touch?" without walking
    // the array. Unclassified cases are counted under an explicit key rather
    // than dropped.
    [[nodiscard]] inline Json LayerCountsJson(const std::vector<TestCase>& cases)
    {
        std::unordered_map<std::string, sizet> counts;
        for (const auto& testCase : cases)
            ++counts[testCase.Layer.empty() ? std::string("unclassified") : testCase.Layer];
        Json out = Json::object();
        for (const auto& [layer, count] : counts)
            out[layer] = count;
        return out;
    }
} // namespace OloEngine::MCP::TestExecution
