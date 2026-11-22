// =============================================================================
// Terrain splat-layer SAMPLING for the fragment paths (issue #1721).
//
// One body for Terrain_PBR.glsl (Forward, Forward+) and Terrain_GBuffer.glsl
// (Deferred). It used to be a hand-kept copy in each, and the two had already
// been edited separately once; everything a textured layer needs — where it is
// sampled, at which two scales, how its three projections and their normals
// combine, which layers are skipped — is here so the paths cannot drift.
//
// The virtual-texture tile bake (compute/TerrainVTTileBake.comp) is the third
// evaluator and keeps its own sampling, because it has no derivatives and no
// view distance; it shares the MATH through include/TerrainLayerBlend.glsl.
//
// Include after, in the including stage:
//   - the camera block (u_CameraPosition is not read here; the caller passes
//     the view distance),
//   - include/TerrainParamsBlock.glsl (binding 10),
//   - include/TerrainLayerBlend.glsl,
//   - the sampler declarations u_TerrainSplatmap0, u_TerrainSplatmap1,
//     u_TerrainAlbedoArray, u_TerrainNormalArray, u_TerrainARMArray and
//     u_TerrainMacroMap (slot-based or OLO_BINDLESS macros).
// =============================================================================

#ifndef OLO_TERRAIN_LAYER_SAMPLING_GLSL
#define OLO_TERRAIN_LAYER_SAMPLING_GLSL

// The three projections' coordinates (oloTerrainProjection) and their
// screen-space gradients.
//
// THE GRADIENTS ARE TAKEN HERE, ONCE, and every layer sample is a textureGrad.
// Layer sampling sits inside per-pixel branches — the virtual-texture fallback,
// the planar/triplanar switch, the dead-layer skip, the two-scale blend — and
// an implicit-derivative texture() inside non-uniform control flow has an
// undefined LOD along every edge of those branches, which shows as a line of
// wrong-mip pixels. Call oloTerrainLayerCoords() at the top of main(), before
// any branch.
struct OloTerrainLayerCoords
{
    OloTerrainProjection P;
    vec2 TopDx;
    vec2 TopDy;
    vec2 SideXDx;
    vec2 SideXDy;
    vec2 SideZDx;
    vec2 SideZDy;
};

// `worldPosAbsY` is the ABSOLUTE world height: the camera-relative position
// plus u_RenderOrigin (issue #429).
OloTerrainLayerCoords oloTerrainLayerCoords(vec2 terrainUV, float worldPosAbsY)
{
    OloTerrainLayerCoords c;
    c.P = oloTerrainProjection(terrainUV, worldPosAbsY, u_WorldSizeAndHeightScale.xy);
    c.TopDx = dFdx(c.P.Top);
    c.TopDy = dFdy(c.P.Top);
    c.SideXDx = dFdx(c.P.SideX);
    c.SideXDy = dFdy(c.P.SideX);
    c.SideZDx = dFdx(c.P.SideZ);
    c.SideZDy = dFdy(c.P.SideZ);
    return c;
}

// One array, one projection, both scales. `farBlend` is 0 inside the near
// distance and 1 beyond the far one, so in either range only one scale is
// fetched.
vec4 oloTerrainSampleScaled(sampler2DArray layers, int layer, vec2 coord, vec2 dx, vec2 dy, float tiling,
                            float farTiling, float farBlend)
{
    vec4 nearSample = vec4(0.0);
    vec4 farSample = vec4(0.0);
    if (farBlend < 1.0)
        nearSample = textureGrad(layers, vec3(coord * tiling, float(layer)), dx * tiling, dy * tiling);
    if (farBlend > 0.0)
        farSample = textureGrad(layers, vec3(coord * farTiling, float(layer)), dx * farTiling, dy * farTiling);
    return mix(nearSample, farSample, farBlend);
}

// One array, all the projections the pixel uses. `w` are the applied weights
// (oloTerrainProjectionWeights); a projection with zero weight is not fetched.
vec4 oloTerrainSampleLayer(sampler2DArray layers, int layer, OloTerrainLayerCoords c, vec3 w, bool useTriplanar,
                           float tiling, float farTiling, float farBlend)
{
    if (!useTriplanar)
        return oloTerrainSampleScaled(layers, layer, c.P.Top, c.TopDx, c.TopDy, tiling, farTiling, farBlend);

    vec4 result = vec4(0.0);
    if (w.y > 0.0)
        result += oloTerrainSampleScaled(layers, layer, c.P.Top, c.TopDx, c.TopDy, tiling, farTiling, farBlend) * w.y;
    if (w.x > 0.0)
        result += oloTerrainSampleScaled(layers, layer, c.P.SideX, c.SideXDx, c.SideXDy, tiling, farTiling, farBlend) * w.x;
    if (w.z > 0.0)
        result += oloTerrainSampleScaled(layers, layer, c.P.SideZ, c.SideZDx, c.SideZDy, tiling, farTiling, farBlend) * w.z;
    return result;
}

// A layer's normal, in the top projection's tangent frame (`tbn`).
vec3 oloTerrainSampleLayerNormal(int layer, OloTerrainLayerCoords c, vec3 worldNormal, mat3 tbn, vec3 w,
                                 bool useTriplanar, float tiling, float farTiling, float farBlend)
{
    if (!useTriplanar)
    {
        return oloTerrainSampleScaled(u_TerrainNormalArray, layer, c.P.Top, c.TopDx, c.TopDy, tiling, farTiling,
                                      farBlend).rgb * 2.0 - 1.0;
    }

    vec3 tangentTop = vec3(0.0, 0.0, 1.0);
    vec3 tangentSideX = vec3(0.0, 0.0, 1.0);
    vec3 tangentSideZ = vec3(0.0, 0.0, 1.0);
    if (w.y > 0.0)
    {
        tangentTop = oloTerrainSampleScaled(u_TerrainNormalArray, layer, c.P.Top, c.TopDx, c.TopDy, tiling,
                                            farTiling, farBlend).rgb * 2.0 - 1.0;
    }
    if (w.x > 0.0)
    {
        tangentSideX = oloTerrainSampleScaled(u_TerrainNormalArray, layer, c.P.SideX, c.SideXDx, c.SideXDy, tiling,
                                              farTiling, farBlend).rgb * 2.0 - 1.0;
    }
    if (w.z > 0.0)
    {
        tangentSideZ = oloTerrainSampleScaled(u_TerrainNormalArray, layer, c.P.SideZ, c.SideZDx, c.SideZDy, tiling,
                                              farTiling, farBlend).rgb * 2.0 - 1.0;
    }
    return oloTerrainTriplanarNormal(worldNormal, tbn, tangentTop, tangentSideX, tangentSideZ, w);
}

// The whole splat blend at one pixel: weights from the splatmaps, the height
// blend, then the layers. `normalMap` comes back in the tangent frame of
// oloTerrainTangentFrame(worldNormal), normalised.
//
// `viewDistance` is the distance from the camera to the pixel; it only selects
// between the two sampling scales, and only when u_TerrainLayerScale enables
// them (oloTerrainFarScaleBlend).
void oloTerrainBlendLayers(OloTerrainLayerCoords c, vec3 worldNormal, float viewDistance, int layerCount,
                           float triplanarSharpness, out vec3 albedo, out float ao, out vec3 normalMap,
                           out float roughness, out float metallic)
{
    // The splatmaps have a single mip level, so the explicit LOD reads exactly
    // what an implicit one would — without a derivative inside the caller's
    // per-pixel branch.
    float weights[8];
    vec4 splat0 = textureLod(u_TerrainSplatmap0, c.P.Top, 0.0);
    weights[0] = splat0.r;
    weights[1] = splat0.g;
    weights[2] = splat0.b;
    weights[3] = splat0.a;

    if (layerCount > 4)
    {
        vec4 splat1 = textureLod(u_TerrainSplatmap1, c.P.Top, 0.0);
        weights[4] = splat1.r;
        weights[5] = splat1.g;
        weights[6] = splat1.b;
        weights[7] = splat1.a;
    }
    else
    {
        weights[4] = 0.0; weights[5] = 0.0; weights[6] = 0.0; weights[7] = 0.0;
    }

    // Shared with the VT tile bake, which must agree about BOTH the
    // weights and the slope threshold: a tile baked planar and shaded
    // as if triplanar shows a hard band exactly where they disagree.
    vec3 projectionWeights = oloTerrainProjectionWeights(oloTerrainTriplanarWeights(worldNormal, triplanarSharpness));
    bool useTriplanar = oloTerrainUseTriplanar(worldNormal);
    mat3 tbn = oloTerrainTangentFrame(worldNormal);
    float farBlend = oloTerrainFarScaleBlend(viewDistance, u_TerrainLayerScale);

    // The ARM alpha is the layer's height for the height blend. Sampled only
    // for the layers the splatmap puts at this pixel, and kept: the blend loop
    // below needs the same texel.
    float heights[8];
    vec4 layerARMs[8];
    for (int i = 0; i < 8; ++i)
    {
        heights[i] = 0.0;
        layerARMs[i] = vec4(0.0);
    }
    int sampledMask = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (i >= layerCount)
            break;
        if (oloTerrainLayerIsDead(weights[i]))
            continue;
        float tiling = oloTerrainLayerTiling(i, u_LayerTilingScales0, u_LayerTilingScales1);
        layerARMs[i] = oloTerrainSampleLayer(u_TerrainARMArray, i, c, projectionWeights, useTriplanar, tiling,
                                             tiling * u_TerrainLayerScale.x, farBlend);
        heights[i] = layerARMs[i].a;
        sampledMask |= (1 << i);
    }

    oloTerrainHeightBlend(weights, heights, layerCount, u_LayerBlendSharpness0, u_LayerBlendSharpness1);

    albedo = vec3(0.0);
    metallic = 0.0;
    roughness = 0.0;
    ao = 0.0;
    normalMap = vec3(0.0);

    for (int i = 0; i < 8; ++i)
    {
        if (i >= layerCount)
            break;
        if (weights[i] < 0.001)
            continue;

        float tiling = oloTerrainLayerTiling(i, u_LayerTilingScales0, u_LayerTilingScales1);
        float farTiling = tiling * u_TerrainLayerScale.x;

        // A layer the splatmap left out can still come out of the height blend
        // with weight — where the splatmap holds no weight at all, every layer
        // does — and then its ARM was not fetched above.
        vec4 layerARM = layerARMs[i];
        if ((sampledMask & (1 << i)) == 0)
        {
            layerARM = oloTerrainSampleLayer(u_TerrainARMArray, i, c, projectionWeights, useTriplanar, tiling,
                                             farTiling, farBlend);
        }
        vec4 layerAlbedo = oloTerrainSampleLayer(u_TerrainAlbedoArray, i, c, projectionWeights, useTriplanar, tiling,
                                                 farTiling, farBlend);
        vec3 layerNormal = oloTerrainSampleLayerNormal(i, c, worldNormal, tbn, projectionWeights, useTriplanar,
                                                       tiling, farTiling, farBlend);

        albedo += layerAlbedo.rgb * weights[i];
        normalMap += layerNormal * weights[i];
        ao += layerARM.r * weights[i];
        roughness += layerARM.g * weights[i];
        metallic += layerARM.b * weights[i];
    }

    // normalize() of a zero vector is a NaN, and it would reach the lighting.
    normalMap = dot(normalMap, normalMap) > 1e-8 ? normalize(normalMap) : vec3(0.0, 0.0, 1.0);
}

// The macro map, applied to the surface the layer blend or the virtual-texture
// resolve produced. Does nothing — to the bit — when the terrain has none:
// u_TerrainMacroParams.x is a UBO value, so the branch is uniform over the draw.
void oloTerrainApplyMacroMap(OloTerrainLayerCoords c, inout vec3 albedo, inout float roughness)
{
    if (u_TerrainMacroParams.x > 0.5)
    {
        // The macro map covers the terrain's own [0,1] UV once.
        vec4 macro = textureGrad(u_TerrainMacroMap, c.P.Top, c.TopDx, c.TopDy);
        oloTerrainApplyMacro(macro, u_TerrainMacroParams, albedo, roughness);
    }
}

#endif // OLO_TERRAIN_LAYER_SAMPLING_GLSL
