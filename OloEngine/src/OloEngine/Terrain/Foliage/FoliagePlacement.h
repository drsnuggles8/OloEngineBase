#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    class TerrainMaterial;
} // namespace OloEngine

// The placement half of foliage generation, split out of FoliageRenderer
// (issue #1230).
//
// This is pure CPU: heightfield sampling, slope and splatmap gating, the
// species habitat rules and clump field of #1254, and the deterministic
// jitter/scale/rotation hash. It creates no GL objects, so the identity
// contract it feeds can be pinned by a headless test that runs the REAL
// generator rather than a stand-in.
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
    //
    // LEGACY, and kept bit-for-bit because every foliage scene on disk was
    // placed with it. It reinterprets a scaled float's bits and finishes with
    // one multiply-and-shift, with the seed XORed in BEFORE that multiply, so
    // two draws whose seeds differ by a small constant are near-affine images
    // of each other. See FoliageLayer::DecorrelatedVariation for the measured
    // consequence and HashCell below for the replacement.
    [[nodiscard]] f32 HashPosition(f32 x, f32 z, u32 seed);

    // Integer avalanche hash of (cell x, cell z, seed). Every input bit is
    // mixed through two multiply-xorshift rounds AFTER the seed is folded in,
    // which is the part HashPosition does not do — so seed, seed + 3 and
    // seed + 7 give independent streams.
    [[nodiscard]] u32 HashCell(u32 cellX, u32 cellZ, u32 seed);

    // HashCell as a float in [0, 1). 24 bits of mantissa, taken from the TOP
    // of the hash word.
    [[nodiscard]] f32 HashCellUnit(u32 cellX, u32 cellZ, u32 seed);

    // The per-layer generator seed. Derived from the layer's PHYSICAL index,
    // which is why reordering layers moves their plants (and therefore retires
    // their ids — see FoliageInstanceRegistry's placement signature).
    [[nodiscard]] constexpr u32 SeedForLayer(u32 layerIndex)
    {
        return layerIndex * 17u + 31u;
    }

    // Which clump field a layer reads (issue #1254). A negative group means
    // "my own", derived from the layer seed; a shared non-negative group makes
    // two species clump TOGETHER, which is what moves a habitat boundary as one
    // thing instead of as N independent noises that average back to uniform.
    [[nodiscard]] constexpr u32 SeedForClumpGroup(i32 clumpGroup, u32 layerSeed)
    {
        return clumpGroup < 0 ? (layerSeed ^ 0x9E3779B9u)
                              : (static_cast<u32>(clumpGroup) * 0x85EBCA77u + 0x01234567u);
    }

    // Grid spacing in world units. Callers must have checked density > 0.
    [[nodiscard]] f32 SpacingForDensity(f32 density);

    // The clump field: smooth two-octave value noise over the world, in
    // [0, 1]. `scale` is roughly one patch across, in world units.
    //
    // Two octaves rather than one because a single value-noise lattice reads as
    // a GRID of patches at landscape scale — the repeated distribution
    // acceptance criterion 2 rules out. The second octave is at an irrational
    // frequency ratio so the two lattices never line up.
    [[nodiscard]] f32 ClumpField(f32 worldX, f32 worldZ, f32 scale, u32 seed);

    // The topographic moisture PROXY, in [0, 1]. Low, flat ground is wet; high,
    // steep ground is dry. `normalizedHeight` is the raw heightfield value in
    // [0, 1] and `upDot` is the surface normal's y.
    //
    // Deliberately not a hydrology simulation — the issue's scope boundary
    // excludes one, and an author needs a repeatable number they can band on,
    // not a flow accumulation they cannot predict.
    [[nodiscard]] f32 MoistureAt(f32 normalizedHeight, f32 upDot);

    // How far a plant sinks so its downhill side is not in the air. Returns a
    // value <= 0, in world units. See FoliageLayer::SlopeSinkFactor.
    [[nodiscard]] f32 GroundSinkFor(f32 slopeSinkFactor, f32 upDot, f32 scale);

    // The multiplier an exclusion splatmap channel applies, in [0, 1]: 1 where
    // nothing is painted, 0 where the painted weight has reached `threshold`.
    // See FoliageLayer::ExclusionSplatmapChannel.
    [[nodiscard]] f32 ExclusionWeight(f32 paintedWeight, f32 threshold);

    // The layer's habitat suitability at one sample, in [0, 1] — the product of
    // the slope, altitude and moisture gates, each feathered. 1 means "fully
    // suitable", 0 means "this species does not grow here".
    //
    // Does NOT include the splatmap mask or the clump field: those need data
    // this signature does not carry. GenerateLayer multiplies all four.
    [[nodiscard]] f32 HabitatSuitability(const FoliageLayer& layer, f32 altitude,
                                         f32 normalizedHeight, f32 upDot);

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
                       const TArray<f32>& heights, u32 resolution,
                       const TerrainMaterial* material,
                       f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                       TArray<Placement>& out);
} // namespace OloEngine::FoliagePlacement
