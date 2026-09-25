#pragma once

// =============================================================================
// Executed/skipped accounting for the renderer state-machine harness (#1349).
//
// The device half of the harness skips on any machine without a GL 4.6
// context, which is every hosted CI runner. That is correct, and it is exactly
// how a required case disappears: gtest prints the skip once, hundreds of
// lines above a `[  PASSED  ]`, and the run reads as green. So every manifest
// row (RendererStateMachineManifest.h) is accounted for here and printed AFTER
// gtest's summary, the same place the Vulkan coverage banner goes:
//
//   [ STATE MACHINE ] renderer state-machine coverage (#1349)
//     executed      cached-vs-rebuild.gl        312 comparisons, 40 at distribution level
//     SKIPPED       alias-vs-noalias.gl         no GL 4.6 context
//     prerequisite  isolated-vs-shared-view     blocked by #1352
//     live-only     sequences.vulkan            PR body: live editor A/B
//
// A Required row whose owning test RAN but recorded no execution is printed as
// NOT EXERCISED: the owner is expected to fail itself in that case (every
// owner asserts its own rows at the end), and the banner makes the gap visible
// even if one does not.
//
// Nothing prints when no owning test was selected, so an unrelated
// --gtest_filter run stays quiet.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <span>
#include <string>
#include <string_view>

namespace OloEngine::Tests::StateMachine::Coverage
{
    enum class RowOutcome : u8
    {
        Executed,
        Skipped,
        NotExercised, // the owner ran, the row was never executed
        NotSelected,  // the owner was filtered out of this run
        Prerequisite,
        LiveOnly
    };

    struct RowReport
    {
        std::string Id;
        RowOutcome Outcome = RowOutcome::NotSelected;
        u32 Comparisons = 0;
        u32 DistributionFallbacks = 0;
        u32 Vacuous = 0;    // comparisons skipped because the lever had nothing to change in that state
        std::string Reason; // skip reason, blocking issue, or live-only evidence
    };

    // A comparison of this manifest row ran and meant something.
    // `distributionFallback`: an exact-criterion comparison that had to fall
    // back to distribution level on at least one target (see the manifest).
    void RecordComparison(std::string_view rowId, bool distributionFallback = false);
    // A comparison of this row was skipped because the lever could not change
    // anything in the current state (no shared alias slot, nothing batchable).
    // Counted and printed, never counted as an execution.
    void RecordVacuous(std::string_view rowId);

    [[nodiscard]] u32 ComparisonCount(std::string_view rowId);

    // Every manifest row, with the outcome this run gave it. Pure given the
    // recorded tallies; exposed so the report's own contract is testable.
    [[nodiscard]] std::string FormatBanner(std::span<const RowReport> rows);

    // Register the listener that prints the banner. Call after the other
    // listeners so the banner lands below gtest's own summary.
    void RegisterListener();
} // namespace OloEngine::Tests::StateMachine::Coverage
