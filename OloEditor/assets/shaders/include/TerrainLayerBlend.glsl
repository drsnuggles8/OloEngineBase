// =============================================================================
// Terrain splat-layer blending — the math ONLY, with every input passed in.
//
// Extracted (issue #715) because there are now THREE evaluators of this blend
// and they must not drift: the forward path (Terrain_PBR.glsl), the deferred
// path (Terrain_GBuffer.glsl), and the virtual-texture tile bake
// (compute/TerrainVTTileBake.comp), which composites the same blend once per
// cache tile so the fragment stages can stop doing it per pixel.
//
// Only the pure functions live here. SAMPLING deliberately does not: the two
// fragment paths take screen-space derivatives while the bake computes an
// explicit footprint from the page's texel density (it has no derivatives, and
// the explicit footprint is what makes a baked tile alias-free). Sharing the
// sampling would have meant one of the three lying about its LOD. The two
// fragment paths do share theirs, in include/TerrainLayerSampling.glsl.
//
// The per-layer scalars arrive as the same two vec4 pairs the TerrainParams UBO
// stores them in, rather than being read from the UBO here, so this file has no
// binding dependency and the compute kernel can pass its own copy.
// =============================================================================

#ifndef OLO_TERRAIN_LAYER_BLEND_GLSL
#define OLO_TERRAIN_LAYER_BLEND_GLSL

// UV tiling factor for a layer, from the packed pair (layers 0-3, layers 4-7).
float oloTerrainLayerTiling(int layer, vec4 tiling0, vec4 tiling1)
{
    if (layer < 4)
        return tiling0[layer];
    else
        return tiling1[layer - 4];
}

// Height-blend sharpness for a layer, from the same packed pair.
float oloTerrainLayerSharpness(int layer, vec4 sharp0, vec4 sharp1)
{
    if (layer < 4)
        return sharp0[layer];
    else
        return sharp1[layer - 4];
}

// Height-based blending: sharpen the splat transition using each layer's height
// (the ARM alpha channel), so a gravel layer's pebbles poke through the grass
// edge instead of the two cross-fading.
//
// `weights` are the raw splatmap weights in, the normalized blend weights out.
void oloTerrainHeightBlend(inout float weights[8], float heights[8], int layerCount,
                           vec4 sharp0, vec4 sharp1)
{
    float maxHeight = -1e10;
    for (int i = 0; i < layerCount; ++i)
    {
        float h = heights[i] + weights[i];
        maxHeight = max(maxHeight, h);
    }

    float sum = 0.0;
    for (int i = 0; i < layerCount; ++i)
    {
        float h = heights[i] + weights[i];
        float sharpness = oloTerrainLayerSharpness(i, sharp0, sharp1);
        weights[i] = max(h - maxHeight + (1.0 / max(sharpness, 0.01)), 0.0);
        sum += weights[i];
    }

    if (sum > 0.0)
    {
        for (int i = 0; i < layerCount; ++i)
            weights[i] /= sum;
    }
}

// A layer whose SPLAT weight is below this is dead at the pixel (issue #1721):
// nothing of it is sampled, and its blend height is 0. Before, the height loop
// sampled the ARM array for every layer of the material at every pixel, so an
// eight-layer terrain paid eight (or, triplanar, twenty-four) fetches for the
// one or two layers actually painted there.
#define OLO_TERRAIN_DEAD_LAYER_WEIGHT 0.001

bool oloTerrainLayerIsDead(float splatWeight)
{
    return splatWeight < OLO_TERRAIN_DEAD_LAYER_WEIGHT;
}

// The slope threshold above which the terrain switches to triplanar sampling.
// A named constant because the bake and the fragment paths must agree: a tile
// baked planar and shaded as if it were triplanar would show a hard band
// wherever the two disagreed.
#define OLO_TERRAIN_TRIPLANAR_SLOPE 0.4
// The slope range over which the side projections fade in above that
// threshold (issue #1721). AT the threshold the triplanar weights are exactly
// (0, 1, 0) — the planar sample — so a pixel crossing it does not jump; the
// full triplanar weights are reached OLO_TERRAIN_TRIPLANAR_FADE further up.
// Without the fade the weights at the threshold were already two-thirds side
// projection, and a textured slope showed a line there.
#define OLO_TERRAIN_TRIPLANAR_FADE 0.15

bool oloTerrainUseTriplanar(vec3 worldNormal)
{
    return (1.0 - worldNormal.y) > OLO_TERRAIN_TRIPLANAR_SLOPE;
}

// Triplanar projection weights for a world normal: x = the X-facing side
// projection, y = the top projection, z = the Z-facing side projection.
vec3 oloTerrainTriplanarWeights(vec3 worldNormal, float sharpness)
{
    vec3 w = pow(abs(worldNormal), vec3(max(sharpness, 1.0)));
    w /= (w.x + w.y + w.z + 0.0001);
    float sideFade = smoothstep(OLO_TERRAIN_TRIPLANAR_SLOPE, OLO_TERRAIN_TRIPLANAR_SLOPE + OLO_TERRAIN_TRIPLANAR_FADE,
                                1.0 - worldNormal.y);
    return mix(vec3(0.0, 1.0, 0.0), w, sideFade);
}

// A projection whose weight is below this is not sampled, and the weights of
// the ones that are get renormalised (oloTerrainProjectionWeights).
#define OLO_TERRAIN_PROJECTION_MIN_WEIGHT 0.002

// The weights actually applied: sub-threshold projections dropped, the rest
// rescaled to sum to exactly one, so a surface of one colour keeps that colour
// whatever its slope.
vec3 oloTerrainProjectionWeights(vec3 triWeights)
{
    vec3 w = vec3(triWeights.x > OLO_TERRAIN_PROJECTION_MIN_WEIGHT ? triWeights.x : 0.0,
                  triWeights.y > OLO_TERRAIN_PROJECTION_MIN_WEIGHT ? triWeights.y : 0.0,
                  triWeights.z > OLO_TERRAIN_PROJECTION_MIN_WEIGHT ? triWeights.z : 0.0);
    float sum = w.x + w.y + w.z;
    return sum > 0.0 ? w / sum : vec3(0.0, 1.0, 0.0);
}

// =============================================================================
// WHERE A LAYER TEXTURE IS SAMPLED (issue #1721)
//
// TilingScale means ONE thing: repeats across the terrain tile. All three
// projections are therefore in tile units — position divided by the tile's
// world size — before the tiling is applied. The old triplanar helpers
// multiplied a world position in METRES by the same number, so a layer that
// repeated 40 times across a 200 m tile on flat ground repeated 8000 times on
// the slope next to it.
//
//   Top    (x, z):  the terrain UV itself — the planar path's own coordinate,
//                   so the top projection IS the planar sample.
//   SideX  (z, -y): X-facing faces. Texture U runs along world Z, texture V
//                   DOWN world Y.
//   SideZ  (x, -y): Z-facing faces. Texture U runs along world X, texture V
//                   down world Y.
//
// Texture V runs down, so image row 0 is the highest: a picture is the right
// way up on a cliff, bedding planes stay horizontal, and the coordinate carries
// on in the same direction over a cliff's top edge as the top projection it
// meets there. The horizontal coordinate is the terrain UV (terrain-local
// position over the tile size), not the absolute world position, for the same
// reason the top projection is: a terrain that does not sit at the world origin
// would otherwise shift its side projections against its planar ones.
//
// `worldY` is the ABSOLUTE world height (the fragment paths add u_RenderOrigin
// back to their camera-relative position, issue #429; the bake uses its
// absolute model matrix). `worldSize` is u_WorldSizeAndHeightScale.xy.
// =============================================================================
struct OloTerrainProjection
{
    vec2 Top;
    vec2 SideX;
    vec2 SideZ;
};

OloTerrainProjection oloTerrainProjection(vec2 terrainUV, float worldY, vec2 worldSize)
{
    OloTerrainProjection p;
    p.Top = terrainUV;
    p.SideX = vec2(terrainUV.y, -worldY / max(worldSize.y, 0.0001));
    p.SideZ = vec2(terrainUV.x, -worldY / max(worldSize.x, 0.0001));
    return p;
}

// The frame a layer's tangent-space normal is expressed in on the top
// projection: T along +X (texture U), B along -Z (image-up, the green channel
// of an OpenGL-convention normal map), N the surface normal. Terrain-specific —
// the tangent frame of a heightfield is analytic.
mat3 oloTerrainTangentFrame(vec3 worldNormal)
{
    vec3 T = normalize(cross(worldNormal, vec3(0.0, 0.0, 1.0)));
    if (length(T) < 0.001)
        T = normalize(cross(worldNormal, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(worldNormal, T);
    return mat3(T, B, worldNormal);
}

// Blend three projections' tangent-space normals into ONE normal, returned in
// the top projection's tangent frame (`tbn`, from oloTerrainTangentFrame) so
// the callers' layer blend and final TBN multiply are unchanged.
//
// Each projection's normal is first turned into a world-space one in its OWN
// frame. The old helpers averaged the three tangent-space vectors as if they
// shared a frame, which they do not: the side projections' X and Y are
// different world axes from the top one's, so a bump on a cliff was lit as if
// it lay on flat ground.
//
//   Top:   the planar frame itself, so with weights (0, 1, 0) the result is the
//          top sample, which is what keeps the planar/triplanar seam continuous.
//   Sides: the "whiteout" blend — the sample's XY added to the geometric normal
//          along the projection's two texture axes, its Z scaling the normal's
//          own component on the projection axis. U is +Z (SideX) or +X (SideZ),
//          image-up is +Y, matching oloTerrainProjection.
//
// `w` are the applied weights (oloTerrainProjectionWeights). A projection with
// zero weight may be passed any vector.
vec3 oloTerrainTriplanarNormal(vec3 worldNormal, mat3 tbn, vec3 tangentTop, vec3 tangentSideX, vec3 tangentSideZ,
                               vec3 w)
{
    vec3 worldTop = tbn * tangentTop;
    vec3 worldSideX = normalize(vec3(abs(tangentSideX.z) * worldNormal.x, tangentSideX.y + worldNormal.y,
                                     tangentSideX.x + worldNormal.z));
    vec3 worldSideZ = normalize(vec3(tangentSideZ.x + worldNormal.x, tangentSideZ.y + worldNormal.y,
                                     abs(tangentSideZ.z) * worldNormal.z));
    vec3 blended = worldTop * w.y;
    if (w.x > 0.0)
        blended += worldSideX * w.x;
    if (w.z > 0.0)
        blended += worldSideZ * w.z;
    // transpose(): the frame is orthonormal, so its transpose is its inverse.
    return transpose(tbn) * blended;
}

// =============================================================================
// TWO-SCALE SAMPLING (issue #1721)
//
// 0 = sample the layer at its own tiling only, 1 = at the far tiling only.
// `layerScale` is u_TerrainLayerScale: x = far tiling ratio (0 = off), y = near
// distance, z = far distance. The constants live in one place,
// Terrain/TerrainSurfaceShading.h, whose FarScaleBlend is the CPU twin.
//
// The virtual-texture tile bake does not call this: a baked tile has no view
// distance. That is also why u_TerrainLayerScale is zero for a terrain that
// shades through the virtual texture — the per-pixel fallback an unready sector
// takes has to keep matching the tiles around it.
// =============================================================================
float oloTerrainFarScaleBlend(float viewDistance, vec4 layerScale)
{
    if (layerScale.x <= 0.0)
        return 0.0;
    return smoothstep(layerScale.y, layerScale.z, viewDistance);
}

// =============================================================================
// THE MACRO MAP (issue #1721)
//
// `macro` is the map's texel: rgb a tint with 128/255 neutral, a = dryness.
// `macroParams` is u_TerrainMacroParams: y = tint scale (255/128), z = wet
// albedo factor, w = wet roughness. CPU twin: TerrainSurfaceShading::ApplyMacro.
// Applied to the blended surface after the layer blend (or the virtual-texture
// resolve) and before snow and weather.
// =============================================================================
void oloTerrainApplyMacro(vec4 macro, vec4 macroParams, inout vec3 albedo, inout float roughness)
{
    float wet = 1.0 - macro.a;
    albedo *= macro.rgb * macroParams.y;
    albedo *= mix(1.0, macroParams.z, wet);
    roughness = mix(roughness, macroParams.w, wet);
}

#endif // OLO_TERRAIN_LAYER_BLEND_GLSL
