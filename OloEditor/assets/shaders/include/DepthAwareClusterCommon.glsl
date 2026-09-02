#ifndef OLO_DEPTH_AWARE_CLUSTER_COMMON_GLSL
#define OLO_DEPTH_AWARE_CLUSTER_COMMON_GLSL

// Shared 2.5D tile-depth math for issue #722. The 32 cells are linear over
// each tile's reduced min/max depth, as specified by the original technique.
const uint OLO_DEPTH_CELL_COUNT = 32u;

uint oloDepthCell(float viewDepth, float tileMinDepth, float tileMaxDepth)
{
    float span = tileMaxDepth - tileMinDepth;
    if (span <= 1e-6)
        return 0u;

    float normalised = clamp((viewDepth - tileMinDepth) / span, 0.0, 1.0);
    return min(uint(normalised * float(OLO_DEPTH_CELL_COUNT)), OLO_DEPTH_CELL_COUNT - 1u);
}

uint oloDepthRangeMask(float depthA, float depthB, float tileMinDepth, float tileMaxDepth)
{
    float rangeMin = min(depthA, depthB);
    float rangeMax = max(depthA, depthB);
    if (rangeMax < tileMinDepth || rangeMin > tileMaxDepth || tileMaxDepth < tileMinDepth)
        return 0u;

    if (tileMaxDepth - tileMinDepth <= 1e-6)
        return 1u;

    uint firstCell = oloDepthCell(max(rangeMin, tileMinDepth), tileMinDepth, tileMaxDepth);
    uint lastCell = oloDepthCell(min(rangeMax, tileMaxDepth), tileMinDepth, tileMaxDepth);
    uint cellCount = lastCell - firstCell + 1u;
    if (cellCount >= OLO_DEPTH_CELL_COUNT)
        return 0xFFFFFFFFu;
    return ((1u << cellCount) - 1u) << firstCell;
}

float oloReconstructViewDepth(float deviceDepth, mat4 inverseProjection)
{
    float ndcDepth = deviceDepth * 2.0 - 1.0;
    // Cluster culling assumes a perspective projection. Its inverse has no
    // x/y contribution to view z or w, so avoid constructing the unused x/y
    // rows for every sampled depth texel.
    float viewZ = inverseProjection[2].z * ndcDepth + inverseProjection[3].z;
    float viewW = inverseProjection[2].w * ndcDepth + inverseProjection[3].w;
    return max(-viewZ / viewW, 0.0);
}

uint oloSliceForViewDepth(float viewDepth, uint sliceCount, float nearPlane, float farPlane)
{
    float logFOverN = log2(farPlane / nearPlane);
    float scale = float(sliceCount) / logFOverN;
    float bias = -float(sliceCount) * log2(nearPlane) / logFOverN;
    int slice = int(floor(log2(max(viewDepth, 0.005)) * scale + bias));
    return uint(clamp(slice, 0, int(sliceCount) - 1));
}

bool oloSphereIntersectsOccupiedDepth(float viewSpaceCenterZ, float radius,
                                      float tileMinDepth, float tileMaxDepth,
                                      uint occupancyMask)
{
    float nearestDepth = -viewSpaceCenterZ - radius;
    float farthestDepth = -viewSpaceCenterZ + radius;
    if (farthestDepth < tileMinDepth || nearestDepth > tileMaxDepth)
        return false;
    return (oloDepthRangeMask(nearestDepth, farthestDepth,
                              tileMinDepth, tileMaxDepth) & occupancyMask) != 0u;
}

// Inverse of the fragment-side floor((pixel + 0.5) * tileCount / extent).
// Boundary 0 maps to 0 and boundary tileCount maps exactly to extent.
uint oloTilePixelBoundary(uint boundary, uint tileCount, uint extent)
{
    if (boundary == 0u || tileCount == 0u)
        return 0u;
    return (2u * boundary * extent + tileCount - 1u) / (2u * tileCount);
}

// Integer form of floor((pixel + 0.5) * tileCount / extent). Keeping both
// sides integer avoids float round-down at exact boundaries (for example
// pixel 20 of a 164-wide viewport on the 32-tile grid).
uint oloTileForPixelCenter(uint pixel, uint tileCount, uint extent)
{
    if (tileCount == 0u || extent == 0u)
        return 0u;
    return min(((2u * pixel + 1u) * tileCount) / (2u * extent), tileCount - 1u);
}

#endif
