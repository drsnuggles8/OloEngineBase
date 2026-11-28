#pragma once

#include "OloEngine/Core/Base.h"

// The fork/inline decision vocabulary shared by RendererAPI's region telemetry
// and CommandBucket's replay policy (#1340). Its own header so the bucket does
// not pull in RendererAPI.h.
namespace OloEngine
{
    // Why one RecordParallel region forked or stayed inline (#1340). Every
    // inline outcome has its own value so telemetry names the decision instead
    // of leaving "Parallel == false" to be interpreted.
    enum class ParallelRegionDecision : u8
    {
        Parallel = 0,         ///< Forked onto task workers.
        InlineBelowThreshold, ///< The caller's cost policy judged the region too small to repay a fork.
        InlineRejectedWork,   ///< The work holds an item the replay cannot fork (query, observed decal, unsupported kind).
        InlineSingleItem,     ///< Fewer than two items: nothing to overlap.
        InlineLeverOff,       ///< OLO_VK_PARALLEL_RECORDING=0.
        InlineNested,         ///< Called from inside another region's item, or while one is in flight.
        InlineUnsupported,    ///< No backend support now: no device or recording, open query, no frame clock, compute queue.
        InlineNoSecondary,    ///< The secondary pool could not hand out a command buffer for every item.
    };

    [[nodiscard]] constexpr const char* ToString(const ParallelRegionDecision decision)
    {
        switch (decision)
        {
            case ParallelRegionDecision::Parallel:
                return "parallel";
            case ParallelRegionDecision::InlineBelowThreshold:
                return "inlineBelowThreshold";
            case ParallelRegionDecision::InlineRejectedWork:
                return "inlineRejectedWork";
            case ParallelRegionDecision::InlineSingleItem:
                return "inlineSingleItem";
            case ParallelRegionDecision::InlineLeverOff:
                return "inlineLeverOff";
            case ParallelRegionDecision::InlineNested:
                return "inlineNested";
            case ParallelRegionDecision::InlineUnsupported:
                return "inlineUnsupported";
            case ParallelRegionDecision::InlineNoSecondary:
                return "inlineNoSecondary";
        }
        return "unknown";
    }

    // What a caller knows about a region before it asks to fork it (#1340):
    // which policy family it belongs to, how much work it carries in that
    // family's unit, and whether the caller's policy already decided to keep it
    // inline. The backend only records it; the decision stays the caller's.
    struct ParallelRegionHint
    {
        const char* Family = nullptr; ///< Static string naming the policy family; null for an unhinted region.
        u32 WorkUnits = 0;            ///< Work in the family's unit (packets for bucket replay); 0 = not estimated
                                      ///< (a shadow region that could not fork skips its estimate).
        u32 MinWorkUnitsToFork = 0;   ///< The threshold the caller applied, for the record.
        ParallelRegionDecision PolicyDecision = ParallelRegionDecision::Parallel;
    };
} // namespace OloEngine
