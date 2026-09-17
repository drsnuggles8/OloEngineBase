#pragma once

namespace OloEngine::RayTracing::VegetationDiagnostics
{
    // Render-thread-only, transient benchmark override. It changes update
    // frequency without changing canonical plants, raster LOD or wind state.
    // No scene/save-game/asset data owns this diagnostic state.
    void SetForceDetailed(bool enabled);
    [[nodiscard]] bool GetForceDetailed();
} // namespace OloEngine::RayTracing::VegetationDiagnostics
