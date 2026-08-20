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
// fragment paths use implicit derivatives while the bake computes an explicit
// LOD from the page's texel density (it has no derivatives, and the explicit
// LOD is what makes a baked tile alias-free). Sharing the sampling would have
// meant one of the three lying about its LOD.
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

// Triplanar projection weights for a world normal. Same expression the two
// fragment paths inline today.
vec3 oloTerrainTriplanarWeights(vec3 worldNormal, float sharpness)
{
    vec3 w = pow(abs(worldNormal), vec3(max(sharpness, 1.0)));
    return w / (w.x + w.y + w.z + 0.0001);
}

// The slope threshold above which the terrain switches to triplanar sampling.
// A named constant because the bake and the fragment paths must agree: a tile
// baked planar and shaded as if it were triplanar would show a hard band
// wherever the two disagreed.
#define OLO_TERRAIN_TRIPLANAR_SLOPE 0.4

bool oloTerrainUseTriplanar(vec3 worldNormal)
{
    return (1.0 - worldNormal.y) > OLO_TERRAIN_TRIPLANAR_SLOPE;
}

#endif // OLO_TERRAIN_LAYER_BLEND_GLSL
