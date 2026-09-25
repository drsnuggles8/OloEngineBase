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
//   * the SnowAccumulationParamsFS block (u_ClipmapCenterAndExtentFS,
//     u_AccumulationParamsFS, u_DisplacementParamsFS) and u_SnowDepthMapFS.
// =============================================================================

float oloTerrainSnowWeight(vec3 worldPosAbs, vec3 vertexNormal)
{
    float weight = oloSnowLayerCoverage(worldPosAbs, vertexNormal);
    if (oloSnowLayerEnabled() && u_DisplacementParamsFS.z > 0.5)
    {
        // Boost from the accumulation depth map: thicker snow, stronger cover.
        vec2 clipCenter = u_ClipmapCenterAndExtentFS[0].xy;
        float clipExtent = u_ClipmapCenterAndExtentFS[0].z;
        vec2 snowUV = (worldPosAbs.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float accumulatedDepth = texture(u_SnowDepthMapFS, snowUV).r;
            float maxDepth = u_AccumulationParamsFS.y;
            weight = max(weight, clamp(accumulatedDepth / max(maxDepth, 0.01), 0.0, 1.0));
        }
    }
    return weight;
}

#endif // TERRAIN_SNOW_WEIGHT_GLSL
