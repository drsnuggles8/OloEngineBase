#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine::RayTracing::VegetationDiagnostics
{
    // Render-thread-only, transient benchmark override. It changes update
    // frequency without changing canonical plants, raster LOD or wind state.
    // No scene/save-game/asset data owns this diagnostic state.
    void SetForceDetailed(bool enabled);
    [[nodiscard]] bool GetForceDetailed();
    // Stress lever (#1354): every vegetation budget behaves as though only
    // 1/divisor of it existed: the producer's plans and the cache's frame
    // budget alike. 1 (the default) is the real budget. For live verification
    // of the fallbacks a real scene rarely reaches; clamped to [1, 1024].
    void SetBudgetDivisor(u32 divisor);
    [[nodiscard]] u32 GetBudgetDivisor();
} // namespace OloEngine::RayTracing::VegetationDiagnostics
