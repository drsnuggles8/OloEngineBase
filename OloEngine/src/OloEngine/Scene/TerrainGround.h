#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>

namespace OloEngine
{
    class Scene;

    // The world height of the terrain surface under (x, z), from the first
    // TerrainComponent whose loaded height field covers that point; nullopt when
    // none does (off every terrain, or no height field built yet). The field is
    // sampled in the terrain entity's own space, the mapping the nav-mesh bake
    // and the terrain chunks use, so a moved or Y-turned terrain answers
    // correctly. The height field is built by the rendered scene update, so a
    // headless scene has none. Issue #1259: ground-following animals and the
    // acceptance scene's camera-clearance contract both read it.
    [[nodiscard]] std::optional<f32> SampleTerrainGroundHeight(const Scene& scene, f32 x, f32 z);
} // namespace OloEngine
