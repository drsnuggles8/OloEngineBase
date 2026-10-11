#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <optional>

namespace OloEngine::GroomCoatShadow
{
    // The coat-volume budget (#1259): a resident set of up to MaxCount volumes
    // of any size, and past that as many more as keep the set within MaxBytes.
    struct CoatVolumeBudget
    {
        u32 MaxCount = 0;
        u64 MaxBytes = 0;
    };

    // Whether a newcomer volume of `newcomerBytes` may be built. It fits while
    // the resident set is under the count, or past it while the bytes still
    // fit. When it does not, `reclaim()` frees one least-recently-used volume
    // not drawn this frame and returns its bytes (nullopt when there is none),
    // and admission repeats until the newcomer fits by either measure.
    //
    // Repeating is the point: freeing one small volume and granting the slot
    // regardless let a close-up coat take the slot of a 300 KiB herd volume and
    // push the set hundreds of megabytes past MaxBytes. `residentCount` and
    // `residentBytes` are updated through every reclaim; `residentBytes` only
    // has to be accurate once the count is at MaxCount.
    template<typename ReclaimFn>
    [[nodiscard]] bool AdmitCoatVolume(u32& residentCount, u64& residentBytes, u64 newcomerBytes,
                                       const CoatVolumeBudget& budget, ReclaimFn&& reclaim)
    {
        const auto fits = [&]
        { return residentCount < budget.MaxCount || residentBytes + newcomerBytes <= budget.MaxBytes; };
        while (!fits())
        {
            const std::optional<u64> freed = reclaim();
            if (!freed)
                break;
            if (residentCount > 0u)
                --residentCount;
            residentBytes -= std::min(residentBytes, *freed);
        }
        return fits();
    }
} // namespace OloEngine::GroomCoatShadow
