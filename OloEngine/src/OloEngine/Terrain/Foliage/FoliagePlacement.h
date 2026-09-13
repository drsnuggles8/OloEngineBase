#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <vector>

namespace OloEngine
{
    class TerrainMaterial;
} // namespace OloEngine

// The placement half of foliage generation, split out of FoliageRenderer
// (issue #1230).
//
// This is pure CPU: heightfield sampling, slope and splatmap gating, and the
// deterministic jitter/scale/rotation hash. It creates no GL objects, so the
// identity contract it feeds can be pinned by a headless test that runs the
// REAL generator rather than a stand-in.
//
// FoliageRenderer::GenerateInstances is the only production caller; it takes
// these placements, uploads their rows to the instance VBO and hands the same
// placements to FoliageInstanceRegistry so a plant's id is keyed on its grid
// cell rather than its row in that buffer.
namespace OloEngine::FoliagePlacement
{
    struct Placement
    {
        // The generator grid cell this plant came from. Together with the
        // layer and the placement signature this IS the plant's identity —
        // see FoliageInstanceRegistry.
        u32 m_CellX = 0;
        u32 m_CellZ = 0;
        FoliageInstanceData m_Row;
    };

    // Deterministic placement hash — returns a float in [0, 1).
    [[nodiscard]] f32 HashPosition(f32 x, f32 z, u32 seed);

    // The per-layer generator seed. Derived from the layer's PHYSICAL index,
    // which is why reordering layers moves their plants (and therefore retires
    // their ids — see FoliageInstanceRegistry's placement signature).
    [[nodiscard]] constexpr u32 SeedForLayer(u32 layerIndex)
    {
        return layerIndex * 17u + 31u;
    }

    // Grid spacing in world units. Callers must have checked density > 0.
    [[nodiscard]] f32 SpacingForDensity(f32 density);

    // Appends every plant this layer places. `out` is cleared first.
    //
    // Takes the RAW height field rather than a TerrainData on purpose. Every
    // TerrainData height query calls SyncFromGPU(), so going through it would
    // both need a GL context (defeating the headless seam) and pay a
    // staleness check twice per grid cell. The caller reads the mirror once —
    // TerrainData::GetHeightData() — and passes it here.
    //
    // `heights` is row-major, resolution x resolution, values in [0, 1]; it is
    // sampled through TerrainData::SampleHeight / SampleNormal, so the
    // sampling convention stays shared with every other CPU consumer.
    void GenerateLayer(const FoliageLayer& layer, u32 layerIndex,
                       const std::vector<f32>& heights, u32 resolution,
                       const TerrainMaterial* material,
                       f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                       std::vector<Placement>& out);
} // namespace OloEngine::FoliagePlacement
