// Layer-2 GPU contract for the production helpers used by issue #722.
#version 460 core

#include "../include/DepthAwareClusterCommon.glsl"

layout(local_size_x = 1) in;

layout(std430, binding = 1) writeonly buffer Outputs
{
    uint values[];
};

void main()
{
    values[0] = oloDepthCell(2.0, 2.0, 18.0);
    values[1] = oloDepthCell(10.0, 2.0, 18.0);
    values[2] = oloDepthCell(18.0, 2.0, 18.0);
    values[3] = oloDepthRangeMask(4.9, 5.1, 0.0, 32.0);
    values[4] = oloTilePixelBoundary(1u, 32u, 1366u);
    values[5] = oloTileForPixelCenter(20u, 32u, 164u);
    values[6] = floatBitsToUint(oloReconstructViewDepth(0.25, mat4(1.0)));
    values[7] = oloSliceForViewDepth(0.1, 24u, 0.1, 1000.0);
    values[8] = oloSphereIntersectsOccupiedDepth(-5.0, 0.25, 0.0, 32.0, 1u << 5u) ? 1u : 0u;
}
