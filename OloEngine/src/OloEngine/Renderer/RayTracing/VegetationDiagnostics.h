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
    // #1578's two sweep levers. BudgetScale multiplies the per-frame WORK
    // limits (builds, vertices, triangles) of the producers' plans, the cache
    // and the backend alike, clamped to [1, 16]; 1 is the shipped budget.
    // FarErrorReference is the distance past which a casting proxy's error
    // bound grows (VegetationPolicy::FarErrorScale); 0 turns it off; it starts
    // at VegetationPolicy::FarErrorReferenceDistance.
    void SetBudgetScale(u32 scale);
    [[nodiscard]] u32 GetBudgetScale();
    void SetFarErrorReference(f32 metres);
    [[nodiscard]] f32 GetFarErrorReference();
} // namespace OloEngine::RayTracing::VegetationDiagnostics
