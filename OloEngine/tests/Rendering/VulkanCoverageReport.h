#pragma once

// =============================================================================
// VulkanCoverageReport — "did this run actually test Vulkan?", issue #1300.
//
// A device-gated Vulkan test SKIPS on a machine with no loader, no capable
// device, or a driver below the ADR 0010 contract. That is correct behaviour,
// and it is why `OloEngine-Tests` is green on a hosted runner. But gtest
// reports a skipped test the same way whether the backend was exercised and
// found correct or never touched at all: the run says `[  PASSED  ]`, the
// skips scroll past hundreds of lines earlier, and nobody reads them.
//
// So "green" carried no information about Vulkan. This records the gate's
// verdict per test and prints a verdict banner AFTER gtest's own summary — the
// last thing on screen, in the output a developer actually reads — plus
// `--olo-require-vulkan`, which turns "never exercised" into a non-zero exit
// for a job whose whole purpose is the Vulkan coverage.
//
// The tally is driven from two places and neither can drift from the other:
//
//   * `OLO_VULKAN_DEVICE_OR_SKIP()` (VulkanTestSupport.h) marks the running
//     test as device-gated and records the gate's reason when it refuses.
//   * The listener's `OnTestEnd` classifies every marked test by whether it
//     actually ran, so a test that passed the gate and then skipped for a
//     reason of its own (bring-up refused, an optional extension missing,
//     `OloEditor/` not found) counts as "not exercised" without having to
//     report itself a second time.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>

namespace OloEngine::Tests::VulkanCoverage
{
    // Everything the banner says, as data. The listener fills one of these and
    // hands it to FormatBanner; nothing else about the verdict is computed at
    // print time. The split is what lets the report's own contract be tested
    // on a machine with no Vulkan device — which is precisely the machine the
    // report exists for, and would otherwise be the one place it is unverified.
    struct Tally
    {
        // Tests that went through OLO_VULKAN_DEVICE_OR_SKIP at all.
        u32 Gated = 0;
        // Of those, the ones that reached the end of their body.
        u32 Executed = 0;
        // Of those, the ones that skipped — at the gate or past it.
        u32 NotExercised = 0;
        // The gate's own refusal text, if the gate refused.
        std::string GateRefusal;
        // The device the gate admitted, if it admitted one.
        std::string DeviceName;
        // The first skip that happened AFTER the gate said yes, and the test
        // it happened in. A different story from "no device here".
        std::string FirstDownstreamSkip;
        std::string FirstDownstreamSkipTest;
    };

    // The banner, or an empty string when no device-gated test was selected at
    // all (a --gtest_filter run, or OLO_WITH_VULKAN=OFF). Saying "Vulkan was
    // not exercised" there would be true and useless: nothing asked for it.
    [[nodiscard]] std::string FormatBanner(const Tally& tally);

    // Mark the currently-running test as device-gated. Called by
    // OLO_VULKAN_DEVICE_OR_SKIP() on every path, including the passing one.
    void MarkCurrentTestDeviceGated();

    // The gate refused. `reason` is the gate's own text; the first one seen
    // wins, because on a machine where the gate refuses it refuses identically
    // every time and repeating it 58 times is what buried it in the first place.
    void RecordGateRefusal(std::string_view reason);

    // The gate admitted this run, on this device. Idempotent.
    void RecordGateAdmission(std::string_view deviceName);

    // --olo-require-vulkan.
    [[nodiscard]] bool Required();

    // How many marked tests actually executed. `main` reads it to turn
    // --olo-require-vulkan into an exit code; nothing else should need it.
    [[nodiscard]] u32 ExecutedCount();

    // Register the banner listener. Call after InitGoogleTest, and after the
    // other listeners, so the banner lands below gtest's own summary.
    void RegisterListener();
} // namespace OloEngine::Tests::VulkanCoverage
