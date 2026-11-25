#ifndef OLO_FOLIAGE_CULL_COMPACTION_GLSL
#define OLO_FOLIAGE_CULL_COMPACTION_GLSL

// A tile contains at most 256 rows. Three independent counts therefore fit
// in one scan word without carrying into the next nine-bit field.
const uint kFoliageCountMask = 0x1ffu;
const uint kFoliageOneMesh = 1u << 9u;
const uint kFoliageOneTested = 1u << 18u;
const uint kFoliageKeepCard = 1u << 31u;
const uint kFoliageKeepMesh = 1u << 30u;

uint oloFoliageTileCount()
{
    return (s_InstanceCount - 1u) / gl_WorkGroupSize.x + 1u;
}

uint oloFoliageTileOffset(uint tile)
{
    return s_ScratchOffset + s_InstanceCount + 2u * tile;
}

struct FoliageInstanceRow
{
    vec4 PositionScale;
    vec4 RotationHeight;
    vec4 ColorAlpha;
};

layout(std430, binding = 16) readonly buffer FoliageCullInput
{
    FoliageInstanceRow b_SrcInstances[];
};

#endif
