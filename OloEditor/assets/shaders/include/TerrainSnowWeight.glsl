#ifndef TERRAIN_SNOW_WEIGHT_GLSL
#define TERRAIN_SNOW_WEIGHT_GLSL

// =============================================================================
// TerrainSnowWeight.glsl — the terrain's snow cover, ONE definition for its
// forward (Terrain_PBR) and G-Buffer (Terrain_GBuffer) programs (issue #1451).
//
// The procedural coverage every snow surface uses (height, slope and wind
// drift — include/SnowLayer.glsl), raised to the depth the snow accumulation
// clipmap has built up there. The two terrain programs used to compute it
// separately, and the G-Buffer one left the wind drift out, so the same slope
// carried different snow on Forward and Deferred.
//
// REQUIREMENTS, declared by the including fragment stage first:
//   * include/SnowLayer.glsl;
//   * u_SnowDepthMapFS.
//
// The clipmap constants are PARAMETERS, not a uniform block, because both
// callers receive them as flat varyings from their tessellation-evaluation
// stage (#1565): a SnowAccumulationParams block in the fragment stage was one
// of the blocks that took Terrain_PBR past NVIDIA's 14 on the raw bindless route.
//   snowClip      xy = ring-0 centre (world XZ), z = ring-0 extent, w = max depth
//                 (SnowAccumulationParams u_ClipmapCenterAndExtent[0].xyz and
//                 u_AccumulationParams.y)
//   accumulating  the clipmap is live (u_DisplacementParams.z > 0.5)
// =============================================================================

float oloTerrainSnowWeight(vec3 worldPosAbs, vec3 vertexNormal, vec4 snowClip, bool accumulating)
{
    float weight = oloSnowLayerCoverage(worldPosAbs, vertexNormal);
    if (oloSnowLayerEnabled() && accumulating)
    {
        // Boost from the accumulation depth map: thicker snow, stronger cover.
        vec2 clipCenter = snowClip.xy;
        float clipExtent = snowClip.z;
        vec2 snowUV = (worldPosAbs.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float accumulatedDepth = texture(u_SnowDepthMapFS, snowUV).r;
            float maxDepth = snowClip.w;
            weight = max(weight, clamp(accumulatedDepth / max(maxDepth, 0.01), 0.0, 1.0));
        }
    }
    return weight;
}

#endif // TERRAIN_SNOW_WEIGHT_GLSL
