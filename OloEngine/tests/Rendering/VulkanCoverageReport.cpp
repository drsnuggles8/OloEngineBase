#include "OloEnginePCH.h"

#include "VulkanCoverageReport.h"

#include "../TestOptions.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace OloEngine::Tests::VulkanCoverage
{
    namespace
    {
        // The run's live tally, plus the one piece of per-test state the
        // listener needs: "the test running right now went through the gate".
        struct State
        {
            Tally Totals;
            bool CurrentTestGated = false;
            // Whether the gate ADMITTED the running test. Separate from
            // Skipped(), because `--olo-require-vulkan` turns a refusal into a
            // FAILURE rather than a skip — and a test that failed at the gate
            // exercised the backend exactly as much as one that skipped there,
            // which is not at all.
            bool CurrentTestAdmitted = false;
        };

        State& Get()
        {
            static State s;
            return s;
        }

        // gtest records GTEST_SKIP as a skip-typed test part carrying the
        // streamed message. Take the first one; a test only skips once.
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

        class Listener final : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo&) override
            {
                auto& state = Get();
                state.CurrentTestGated = false;
                state.CurrentTestAdmitted = false;
            }

            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                auto& state = Get();
                if (!state.CurrentTestGated)
                    return;
                const bool admitted = state.CurrentTestAdmitted;
                state.CurrentTestGated = false;
                state.CurrentTestAdmitted = false;
                ++state.Totals.Gated;

                const auto* result = info.result();
                if (admitted && (result == nullptr || !result->Skipped()))
                {
                    ++state.Totals.Executed;
                    return;
                }

                ++state.Totals.NotExercised;
                // The gate's own refusal is recorded by the macro, which knows
                // the reason first-hand. Anything else is a skip PAST the gate,
                // and means this machine does have a device.
                if (state.Totals.GateRefusal.empty() && state.Totals.FirstDownstreamSkip.empty())
                {
                    state.Totals.FirstDownstreamSkip = SkipMessageOf(*result);
                    state.Totals.FirstDownstreamSkipTest =
                        std::string(info.test_suite_name()) + "." + info.name();
                }
            }

            void OnTestProgramEnd(const ::testing::UnitTest&) override
            {
                const std::string banner = FormatBanner(Get().Totals);
                if (banner.empty())
                    return;
                std::fputs(banner.c_str(), stdout);
                std::fflush(stdout);
            }
        };

        constexpr const char* kRule =
            "=============================================================================\n";
    } // namespace

    std::string FormatBanner(const Tally& tally)
    {
        if (tally.Gated == 0)
            return {};

        char line[512];
        std::string out = "\n";
        out += kRule;

        if (tally.Executed == tally.Gated)
        {
            std::snprintf(line, sizeof(line),
                          "Vulkan backend coverage: EXERCISED - %u/%u device-gated tests ran.\n",
                          tally.Executed, tally.Gated);
            out += line;
            if (!tally.DeviceName.empty())
                out += "  device: " + tally.DeviceName + "\n";
        }
        else if (tally.Executed == 0)
        {
            std::snprintf(line, sizeof(line),
                          "Vulkan backend coverage: NOT EXERCISED - none of the %u device-gated tests ran.\n",
                          tally.Gated);
            out += line;
            const std::string& reason =
                !tally.GateRefusal.empty() ? tally.GateRefusal : tally.FirstDownstreamSkip;
            out += "  reason: " + (reason.empty() ? std::string("(unreported)") : reason) + "\n";
            out += "  This run says NOTHING about the Vulkan backend. Green here is green for\n"
                   "  OpenGL only. Pass --olo-require-vulkan to make it a failure instead.\n"
                   "  See docs/agent-rules/testing-architecture.md section 10.\n";
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          "Vulkan backend coverage: PARTIAL - %u of %u device-gated tests ran, %u skipped.\n",
                          tally.Executed, tally.Gated, tally.NotExercised);
            out += line;
            if (!tally.DeviceName.empty())
                out += "  device: " + tally.DeviceName + "\n";
            if (!tally.FirstDownstreamSkip.empty())
            {
                out += "  first skip past the gate: " + tally.FirstDownstreamSkipTest + "\n    " +
                       tally.FirstDownstreamSkip + "\n";
                out += "  The device is present, so these skipped for a reason of their own - an\n"
                       "  optional extension, a refused bring-up, a missing asset root.\n";
            }
            // A refusal recorded on a run that ALSO executed something means
            // the gate's own answer moved mid-run. Never silently drop it: it
            // is the more surprising of the two reasons, not the less.
            if (!tally.GateRefusal.empty())
                out += "  the gate ALSO refused at least once: " + tally.GateRefusal + "\n";
        }

        out += kRule;
        return out;
    }

    void MarkCurrentTestDeviceGated()
    {
        Get().CurrentTestGated = true;
    }

    void RecordGateRefusal(std::string_view reason)
    {
        auto& totals = Get().Totals;
        // First one wins. On a machine where the gate refuses it refuses
        // identically every time, and repeating the same sentence 58 times is
        // what buried it in the scrollback to begin with.
        if (totals.GateRefusal.empty())
            totals.GateRefusal.assign(reason);
    }

    void RecordGateAdmission(std::string_view deviceName)
    {
        auto& totals = Get().Totals;
        Get().CurrentTestAdmitted = true;
        if (totals.DeviceName.empty() && !deviceName.empty())
            totals.DeviceName.assign(deviceName);
    }

    bool Required()
    {
        return Options().RequireVulkan;
    }

    u32 ExecutedCount()
    {
        return Get().Totals.Executed;
    }

    void RegisterListener()
    {
        ::testing::UnitTest::GetInstance()->listeners().Append(new Listener());
    }
} // namespace OloEngine::Tests::VulkanCoverage
