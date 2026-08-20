// =============================================================================
// LightmapSampling.glsl — baked lightmap irradiance sampling (issue #439)
//
// The scene lightmap atlas stores INDIRECT irradiance E in the reference path
// tracer's physical units — the same convention as the DDGI irradiance atlas
// (see docs/agent-rules/reference-path-tracer.md §4), so a lightmap texel and a
// probe sample are interchangeable inputs to the ambient ladder's
// calculateLightProbeAmbient / calculateCombinedAmbientPrefiltered helpers.
// Punctual direct lighting is NOT in the atlas (the bake's estimator can never
// hit a delta light) and stays realtime.
//
// Per-draw atlas regions travel in InstanceData::LightmapScaleOffset (all-zero
// = this draw has no lightmap). u_LightmapEnabled is the scene-level kill
// switch: the runtime uploads 0 whenever the bake is stale or unresolved, so a
// stale bake is never sampled.
//
// The sampler is deliberately SLOT-BASED with no OLO_BINDLESS branch: this is a
// shared include, and an OLO_BINDLESS token here would drag every includer onto
// the raw-GLSL route (glsl-shaders.md §5e, first exclusion row). The C++ side
// binds it through HeapBinding::PublishTextureOffsetAndBind, which stages a
// heap offset AND issues a real bind, so converted and slot-based includers
// both read the atlas — the exact mechanism the DDGI atlases use.
// =============================================================================

#ifndef LIGHTMAP_SAMPLING_GLSL
#define LIGHTMAP_SAMPLING_GLSL

// Lightmap parameters UBO (binding 1)
layout(std140, binding = 1) uniform LightmapData {
    int   u_LightmapEnabled;   // 1 = atlas bound and bake key matches the live scene
    float u_LightmapIntensity; // global baked-GI intensity multiplier
    float u_LightmapTexelSize; // 1.0 / atlas dimension (square atlas)
    int   _lightmapPad0;
};

layout(binding = 16) uniform sampler2D u_LightmapAtlas; // TEX_LIGHTMAP

// Returns the baked indirect irradiance E for this fragment, or vec3(0.0) when
// the scene has no valid bake or this draw carries no lightmap region. Callers
// route a non-zero result through the same ambient helpers as probe irradiance.
vec3 sampleLightmapIrradiance(vec2 lightmapUV, vec4 scaleOffset)
{
    if (u_LightmapEnabled == 0 || scaleOffset.x <= 0.0)
        return vec3(0.0);

    vec2 atlasUV = lightmapUV * scaleOffset.xy + scaleOffset.zw;
    vec4 texel = texture(u_LightmapAtlas, atlasUV);
    // Alpha 0 = never-baked texel (outside every chart, beyond dilation reach):
    // fall through to probes/IBL rather than shading from the clear colour.
    if (texel.a <= 0.0)
        return vec3(0.0);

    return texel.rgb * u_LightmapIntensity;
}

#endif // LIGHTMAP_SAMPLING_GLSL
