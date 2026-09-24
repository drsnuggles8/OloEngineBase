#include "OloEnginePCH.h"
#include "StateMachineCoverage.h"

#include "RendererStateMachineManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace OloEngine::Tests::StateMachine::Coverage
{
    namespace
    {
        struct RowTally
        {
            u32 Comparisons = 0;
            u32 Fallbacks = 0;
            u32 Vacuous = 0;
        };

        struct OwnerResult
        {
            bool Selected = false;
            bool Skipped = false;
            std::string SkipReason;
        };

        struct State
        {
            std::mutex Mutex;
            std::map<std::string, RowTally, std::less<>> Rows;
            // Keyed by "Suite.Test", the manifest's Owner spelling.
            std::map<std::string, OwnerResult, std::less<>> Owners;
        };

        State& Get()
        {
            static State s;
            return s;
        }

        std::string SkipMessageOf(const ::testing::TestResult& result)
        {
            for (int i = 0; i < result.total_part_count(); ++i)
            {
                const auto& part = result.GetTestPartResult(i);
                if (part.type() == ::testing::TestPartResult::kSkip)
                    return part.message();
            }
            return {};
        }

        // The first line of a skip message: the fixture's skip carries the GL
        // probe's detail after a newline, which belongs in the log, not here.
        std::string FirstLine(const std::string& text)
        {
            const auto newline = text.find('\n');
            return newline == std::string::npos ? text : text.substr(0, newline);
        }

        [[nodiscard]] bool IsOwnerName(std::string_view name)
        {
            // A Required row's Owner is a test; a Prerequisite's is an issue.
            return !name.empty() && name.front() != '#' && name.find('.') != std::string_view::npos;
        }

        // Called with State::Mutex held.
        RowReport Resolve(std::string_view id, RowStatus status, std::string_view owner)
        {
            RowReport report;
            report.Id = std::string(id);
            if (status == RowStatus::Prerequisite)
            {
                report.Outcome = RowOutcome::Prerequisite;
                report.Reason = "blocked by " + std::string(owner);
                return report;
            }
            if (status == RowStatus::LiveOnly)
            {
                report.Outcome = RowOutcome::LiveOnly;
                report.Reason = std::string(owner);
                return report;
            }

            auto& state = Get();
            if (const auto it = state.Rows.find(id); it != state.Rows.end())
            {
                report.Comparisons = it->second.Comparisons;
                report.DistributionFallbacks = it->second.Fallbacks;
                report.Vacuous = it->second.Vacuous;
            }
            const auto ownerIt = state.Owners.find(owner);
            if (ownerIt == state.Owners.end() || !ownerIt->second.Selected)
            {
                report.Outcome = RowOutcome::NotSelected;
                return report;
            }
            if (ownerIt->second.Skipped)
            {
                report.Outcome = RowOutcome::Skipped;
                report.Reason = FirstLine(ownerIt->second.SkipReason);
                return report;
            }
            report.Outcome = report.Comparisons > 0u ? RowOutcome::Executed : RowOutcome::NotExercised;
            return report;
        }

        std::vector<RowReport> CollectRows()
        {
            std::vector<RowReport> rows;
            for (const PairRow& pair : kPairs)
                rows.push_back(Resolve(pair.Id, pair.Status, pair.Owner));
            for (const NegativeControlRow& control : kNegativeControls)
            {
                const RowStatus status = control.Where == Placement::LiveVulkan ? RowStatus::LiveOnly : RowStatus::Required;
                rows.push_back(Resolve(control.Id, status, control.Owner));
            }
            return rows;
        }

        class Listener final : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                const std::string name = std::string(info.test_suite_name()) + "." + info.name();
                auto& state = Get();
                const std::scoped_lock lock(state.Mutex);
                const auto it = state.Owners.find(name);
                if (it == state.Owners.end())
                    return;
                it->second.Selected = true;
                const auto* result = info.result();
                if (result != nullptr && result->Skipped())
                {
                    it->second.Skipped = true;
                    it->second.SkipReason = SkipMessageOf(*result);
                }
            }

            void OnTestProgramEnd(const ::testing::UnitTest&) override
            {
                std::vector<RowReport> rows;
                {
                    const std::scoped_lock lock(Get().Mutex);
                    rows = CollectRows();
                }
                const std::string banner = FormatBanner(rows);
                if (banner.empty())
                    return;
                std::fputs(banner.c_str(), stdout);
                std::fflush(stdout);
            }
        };

        const char* OutcomeLabel(RowOutcome outcome)
        {
            switch (outcome)
            {
                case RowOutcome::Executed:
                    return "executed";
                case RowOutcome::Skipped:
                    return "SKIPPED";
                case RowOutcome::NotExercised:
                    return "NOT EXERCISED";
                case RowOutcome::NotSelected:
                    return "not selected";
                case RowOutcome::Prerequisite:
                    return "prerequisite";
                case RowOutcome::LiveOnly:
                    return "live-only";
            }
            return "?";
        }
    } // namespace

    void RecordComparison(std::string_view rowId, bool distributionFallback)
    {
        auto& state = Get();
        const std::scoped_lock lock(state.Mutex);
        auto& row = state.Rows[std::string(rowId)];
        ++row.Comparisons;
        if (distributionFallback)
            ++row.Fallbacks;
    }

    void RecordVacuous(std::string_view rowId)
    {
        auto& state = Get();
        const std::scoped_lock lock(state.Mutex);
        ++state.Rows[std::string(rowId)].Vacuous;
    }

    u32 ComparisonCount(std::string_view rowId)
    {
        auto& state = Get();
        const std::scoped_lock lock(state.Mutex);
        const auto it = state.Rows.find(rowId);
        return it == state.Rows.end() ? 0u : it->second.Comparisons;
    }

    std::string FormatBanner(std::span<const RowReport> rows)
    {
        // Quiet unless some row's owner actually took part in this run.
        const bool anySelected = std::ranges::any_of(rows,
                                                     [](const RowReport& row)
                                                     {
                                                         return row.Outcome == RowOutcome::Executed ||
                                                                row.Outcome == RowOutcome::Skipped ||
                                                                row.Outcome == RowOutcome::NotExercised;
                                                     });
        if (!anySelected)
            return {};

        std::string out = "\n[ STATE MACHINE ] renderer state-machine coverage (#1349)\n";
        u32 executed = 0;
        u32 skipped = 0;
        u32 missing = 0;
        for (const RowReport& row : rows)
        {
            char line[512];
            std::string detail;
            switch (row.Outcome)
            {
                case RowOutcome::Executed:
                    ++executed;
                    detail = std::to_string(row.Comparisons) + " comparison(s)";
                    if (row.DistributionFallbacks > 0u)
                        detail += ", " + std::to_string(row.DistributionFallbacks) + " at distribution level";
                    if (row.Vacuous > 0u)
                        detail += ", " + std::to_string(row.Vacuous) + " vacuous (lever had nothing to change)";
                    break;
                case RowOutcome::Skipped:
                    ++skipped;
                    detail = row.Reason;
                    break;
                case RowOutcome::NotExercised:
                    ++missing;
                    detail = "its test ran but never compared anything";
                    break;
                case RowOutcome::NotSelected:
                    detail = "its test was filtered out of this run";
                    break;
                case RowOutcome::Prerequisite:
                case RowOutcome::LiveOnly:
                    detail = row.Reason;
                    break;
            }
            std::snprintf(line, sizeof(line), "  %-14s %-34s %s\n", OutcomeLabel(row.Outcome), row.Id.c_str(), detail.c_str());
            out += line;
        }
        out += "  " + std::to_string(executed) + " executed, " + std::to_string(skipped) + " skipped, " +
               std::to_string(missing) + " not exercised.";
        if (skipped > 0u)
            out += " A skipped row verified nothing on this machine.";
        out += "\n";
        return out;
    }

    void RegisterListener()
    {
        auto& state = Get();
        {
            const std::scoped_lock lock(state.Mutex);
            for (const PairRow& pair : kPairs)
            {
                if (IsOwnerName(pair.Owner))
                    state.Owners.try_emplace(std::string(pair.Owner));
            }
            for (const NegativeControlRow& control : kNegativeControls)
            {
                if (IsOwnerName(control.Owner))
                    state.Owners.try_emplace(std::string(control.Owner));
            }
        }
        ::testing::UnitTest::GetInstance()->listeners().Append(new Listener());
    }
} // namespace OloEngine::Tests::StateMachine::Coverage
