// OLO_TEST_LAYER: meta
// =============================================================================
// VulkanCoverageReportTest — the #1300 criterion "a run that skipped for want
// of a device is distinguishable from a run that executed and passed" is
// itself a contract, and it must hold on the machine that HAS no device. So
// these tests are device-free by construction: they drive the pure
// VulkanCoverage::FormatBanner over hand-built tallies.
//
// This is deliberately the one Vulkan-adjacent file that does NOT go through
// OLO_VULKAN_DEVICE_OR_SKIP. Gating the report's own tests on a device would
// reproduce the bug: the guard against "green means nothing here" would only
// be checked where green already means something.
//
// Issue #1294 ("make skipped visible" for the L7 ray-query gap) can reuse
// FormatBanner and this file's shape rather than growing a second reporter.
// =============================================================================

#include "OloEnginePCH.h"

#include "VulkanCoverageReport.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
    using OloEngine::Tests::VulkanCoverage::FormatBanner;
    using Tally = OloEngine::Tests::VulkanCoverage::Tally;

    [[nodiscard]] bool Contains(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    // Nothing selected a device-gated test. A banner here would be noise on
    // every `--gtest_filter=MathTest.*` run in the repo.
    TEST(VulkanCoverageReport, SaysNothingWhenNoDeviceGatedTestWasSelected)
    {
        EXPECT_EQ(FormatBanner(Tally{}), std::string{});
    }

    // The case the issue is about: 58 tests skipped, and the run reports
    // PASSED. The banner has to contradict that in as many words.
    TEST(VulkanCoverageReport, AllSkippedReadsAsNotExercisedAndNamesTheGateReason)
    {
        Tally tally;
        tally.Gated = 58;
        tally.Executed = 0;
        tally.NotExercised = 58;
        tally.GateRefusal = "No Vulkan loader on this machine.";

        const std::string banner = FormatBanner(tally);
        EXPECT_TRUE(Contains(banner, "NOT EXERCISED")) << banner;
        EXPECT_TRUE(Contains(banner, "58")) << banner;
        EXPECT_TRUE(Contains(banner, "No Vulkan loader on this machine.")) << banner;
        // The actionable half: a reader must be told how to make this fail.
        EXPECT_TRUE(Contains(banner, "--olo-require-vulkan")) << banner;
        // And it must not be mistakable for the good outcome. Matched on the
        // full verdict phrase, because "NOT EXERCISED" contains "EXERCISED".
        EXPECT_FALSE(Contains(banner, "coverage: EXERCISED")) << banner;
    }

    // The three gate refusals the suite can produce — no device, no loader,
    // driver below the contract — are one code path with three texts, and the
    // banner must carry whichever one it was rather than a generic sentence.
    TEST(VulkanCoverageReport, EveryGateRefusalReasonSurvivesIntoTheBanner)
    {
        for (const char* reason : { "No Vulkan loader on this machine.",
                                    "vkCreateInstance failed (no Vulkan 1.4-capable ICD available).",
                                    "No device satisfies the ADR 0010 capability contract here — the "
                                    "gate would refuse --rhi=vulkan.",
                                    "No GPU / GL 4.5+ context available in this environment (GL backend "
                                    "pinned to 'none' by --olo-gl-backend=none; the Vulkan gate honours "
                                    "it too)." })
        {
            Tally tally;
            tally.Gated = 12;
            tally.NotExercised = 12;
            tally.GateRefusal = reason;
            const std::string banner = FormatBanner(tally);
            EXPECT_TRUE(Contains(banner, "NOT EXERCISED")) << reason;
            EXPECT_TRUE(Contains(banner, reason)) << banner;
        }
    }

    // The good outcome names the hardware. "EXERCISED" on an unnamed device is
    // half an answer: two developers' boxes are not the same evidence, and a
    // PR that says "the Vulkan tests pass" should be able to say where.
    TEST(VulkanCoverageReport, AllExecutedReadsAsExercisedAndNamesTheDevice)
    {
        Tally tally;
        tally.Gated = 58;
        tally.Executed = 58;
        tally.DeviceName = "NVIDIA GeForce RTX 4090";

        const std::string banner = FormatBanner(tally);
        EXPECT_TRUE(Contains(banner, "coverage: EXERCISED")) << banner;
        EXPECT_FALSE(Contains(banner, "NOT EXERCISED")) << banner;
        EXPECT_TRUE(Contains(banner, "58/58")) << banner;
        EXPECT_TRUE(Contains(banner, "NVIDIA GeForce RTX 4090")) << banner;
    }

    // A device IS present and some tests still skipped — an optional extension
    // (VK_EXT_mesh_shader), a refused bring-up, a missing asset root. Telling
    // this developer "no device" would send them hunting for a driver they
    // already have, so the third state is its own sentence.
    TEST(VulkanCoverageReport, PartialNamesTheFirstSkipPastTheGateAndNotTheDevice)
    {
        Tally tally;
        tally.Gated = 58;
        tally.Executed = 57;
        tally.NotExercised = 1;
        tally.DeviceName = "NVIDIA GeForce RTX 4090";
        tally.FirstDownstreamSkip = "VK_EXT_mesh_shader (task+mesh) not enabled on this device";
        tally.FirstDownstreamSkipTest = "VulkanPassSuite.VirtualGeometryMeshTasksMatchTheMdiPath";

        const std::string banner = FormatBanner(tally);
        EXPECT_TRUE(Contains(banner, "PARTIAL")) << banner;
        EXPECT_TRUE(Contains(banner, "57 of 58")) << banner;
        EXPECT_TRUE(Contains(banner, "VulkanPassSuite.VirtualGeometryMeshTasksMatchTheMdiPath")) << banner;
        EXPECT_TRUE(Contains(banner, "VK_EXT_mesh_shader")) << banner;
        EXPECT_TRUE(Contains(banner, "NVIDIA GeForce RTX 4090")) << banner;
        EXPECT_FALSE(Contains(banner, "NOT EXERCISED")) << banner;
    }

    // With no gate refusal recorded and no downstream text either, the banner
    // must still be honest rather than printing an empty `reason:` line that
    // reads like a truncated message.
    TEST(VulkanCoverageReport, MissingReasonIsReportedAsUnreportedRatherThanBlank)
    {
        Tally tally;
        tally.Gated = 3;
        tally.NotExercised = 3;

        const std::string banner = FormatBanner(tally);
        EXPECT_TRUE(Contains(banner, "NOT EXERCISED")) << banner;
        EXPECT_TRUE(Contains(banner, "(unreported)")) << banner;
    }

    // A run where the gate refused SOMETIMES and admitted others: its answer
    // moved mid-run, which is the most surprising thing the banner can have to
    // say. The PARTIAL verdict used to swallow it — the downstream-skip slot
    // was suppressed by the recorded refusal and the refusal itself was never
    // printed, leaving "the device is present, so these skipped for a reason of
    // their own" as the only explanation offered, which is the opposite of what
    // happened.
    TEST(VulkanCoverageReport, PartialStillReportsAGateRefusalThatHappenedMidRun)
    {
        Tally tally;
        tally.Gated = 4;
        tally.Executed = 3;
        tally.NotExercised = 1;
        tally.DeviceName = "NVIDIA GeForce RTX 4090";
        tally.GateRefusal = "vkCreateInstance failed (no Vulkan 1.4-capable ICD available).";

        const std::string banner = FormatBanner(tally);
        EXPECT_TRUE(Contains(banner, "PARTIAL")) << banner;
        EXPECT_TRUE(Contains(banner, "vkCreateInstance failed")) << banner;
        // And it must NOT claim the device was present throughout, which is the
        // sentence that only belongs to a skip past an admitting gate.
        EXPECT_FALSE(Contains(banner, "The device is present")) << banner;
    }

    // The three verdicts must be mutually exclusive on their own text, because
    // a reader (or a grep in a CI log) distinguishes them by exactly that.
    TEST(VulkanCoverageReport, TheThreeVerdictsAreDistinguishableByText)
    {
        Tally none;
        none.Gated = 4;
        none.NotExercised = 4;
        none.GateRefusal = "No Vulkan loader on this machine.";

        Tally all;
        all.Gated = 4;
        all.Executed = 4;

        Tally partial;
        partial.Gated = 4;
        partial.Executed = 3;
        partial.NotExercised = 1;

        const std::string noneBanner = FormatBanner(none);
        const std::string allBanner = FormatBanner(all);
        const std::string partialBanner = FormatBanner(partial);

        EXPECT_NE(noneBanner, allBanner);
        EXPECT_NE(noneBanner, partialBanner);
        EXPECT_NE(allBanner, partialBanner);
        EXPECT_FALSE(Contains(allBanner, "PARTIAL"));
        EXPECT_FALSE(Contains(partialBanner, "NOT EXERCISED"));
        EXPECT_FALSE(Contains(noneBanner, "PARTIAL"));
    }
} // namespace
