#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace OloEngine
{
    struct StreamingSettings
    {
        bool Enabled = false;
        f32 DefaultLoadRadius = 200.0f;
        f32 DefaultUnloadRadius = 250.0f;
        u32 MaxLoadedRegions = 16;
        std::string RegionDirectory; // Relative to working directory

        // Byte budgets (issue #1365), consulted next to MaxLoadedRegions. 0 means
        // "no byte budget". Megabytes as f32 because that is what a person types;
        // SceneStreamer converts them to bytes once per frame.
        //
        // MaxResidentMegabytes bounds the estimated bytes of loaded and loading
        // regions: admission defers a request that would cross it, and eviction
        // unloads least-recently-used regions while the resident bytes exceed it.
        // A region larger than the whole budget is rejected, not deferred.
        f32 MaxResidentMegabytes = 0.0f;
        // MaxAdmittedMegabytesPerFrame bounds the estimated bytes admitted in one
        // frame; one region is always admitted per frame so a region larger than
        // this still loads.
        f32 MaxAdmittedMegabytesPerFrame = 0.0f;

        // Groom/vegetation detail working set (#1257). This GPU sub-budget is
        // independent of region file estimates above. The complete drawable
        // fallback and required simulation/binding data remain pinned and are
        // reported separately. Retiring detail allocations still spend it.
        // All three zeroes retain the eager loading policy used by older scenes.
        f32 RepresentationResidentMegabytes = 0.0f;
        f32 RepresentationUploadMegabytesPerFrame = 0.0f;
        // CPU preparation (queued, running, cancelled-running and ready results),
        // shared across both families; never added to the GPU byte figure.
        f32 RepresentationStagingMegabytes = 0.0f;
    };

    // 1 TiB. Far above any machine this runs on, and small enough that the
    // megabytes-to-bytes conversion cannot overflow a u64.
    inline constexpr f32 kMaxStreamingBudgetMegabytes = 1024.0f * 1024.0f;

    [[nodiscard]] inline f32 SanitizeStreamingBudgetMegabytes(f32 megabytes)
    {
        if (!std::isfinite(megabytes) || megabytes <= 0.0f)
            return 0.0f;
        return std::min(megabytes, kMaxStreamingBudgetMegabytes);
    }

    [[nodiscard]] inline u64 StreamingBudgetMegabytesToBytes(f32 megabytes)
    {
        const f32 sanitized = SanitizeStreamingBudgetMegabytes(megabytes);
        return static_cast<u64>(static_cast<f64>(sanitized) * 1024.0 * 1024.0);
    }

    [[nodiscard]] inline bool IsRepresentationStreamingEnabled(const StreamingSettings& settings)
    {
        return StreamingBudgetMegabytesToBytes(settings.RepresentationResidentMegabytes) != 0 ||
               StreamingBudgetMegabytesToBytes(settings.RepresentationUploadMegabytesPerFrame) != 0 ||
               StreamingBudgetMegabytesToBytes(settings.RepresentationStagingMegabytes) != 0;
    }

    inline void SanitizeStreamingSettings(StreamingSettings& ss)
    {
        if (!std::isfinite(ss.DefaultLoadRadius))
            ss.DefaultLoadRadius = 200.0f;
        if (!std::isfinite(ss.DefaultUnloadRadius))
            ss.DefaultUnloadRadius = 250.0f;

        ss.DefaultLoadRadius = std::max(ss.DefaultLoadRadius, 1.0f);
        ss.DefaultUnloadRadius = std::max(ss.DefaultUnloadRadius, ss.DefaultLoadRadius + 1.0f);
        ss.MaxLoadedRegions = std::max(ss.MaxLoadedRegions, 1u);

        // A NaN, infinite or negative budget is "no budget", the same as the default:
        // turning a corrupt value into a tiny budget would silently stop streaming.
        ss.MaxResidentMegabytes = SanitizeStreamingBudgetMegabytes(ss.MaxResidentMegabytes);
        ss.MaxAdmittedMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(ss.MaxAdmittedMegabytesPerFrame);
        ss.RepresentationResidentMegabytes = SanitizeStreamingBudgetMegabytes(ss.RepresentationResidentMegabytes);
        ss.RepresentationUploadMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(ss.RepresentationUploadMegabytesPerFrame);
        ss.RepresentationStagingMegabytes = SanitizeStreamingBudgetMegabytes(ss.RepresentationStagingMegabytes);
    }
} // namespace OloEngine
