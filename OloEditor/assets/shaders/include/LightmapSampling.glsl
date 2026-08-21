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

// Returns baked indirect irradiance E in .rgb and COVERAGE in .a: a > 0.5
// means this fragment has a valid baked texel — possibly legitimately pure
// black (an enclosed surface no indirect light reaches within MaxBounces) —
// and the caller must use the lightmap INSTEAD of the probe/IBL diffuse
// ladder. a == 0.0 means no bake covers this fragment (scene kill switch off,
// no region on this draw, or a never-baked texel) and the caller falls
// through. Coverage must be the branch signal, never dot(rgb, rgb): branching
// on the colour makes baked darkness indistinguishable from "no bake" and the
// enclosed room glows with sky IBL — exactly the leak the bake exists to kill.
vec4 sampleLightmapIrradiance(vec2 lightmapUV, vec4 scaleOffset)
{
    if (u_LightmapEnabled == 0 || scaleOffset.x <= 0.0)
        return vec4(0.0);

    vec2 atlasUV = lightmapUV * scaleOffset.xy + scaleOffset.zw;
    vec4 texel = texture(u_LightmapAtlas, atlasUV);
    // Alpha marks baked/dilated texels (1.0) vs never-written ones (0.0). A
    // bilinear tap straddling the coverage edge blends both; below one-half
    // the tap is dominated by unwritten texels — fall through. Above it,
    // un-premultiply by the sampled alpha so the empty neighbours' black
    // doesn't darken the chart edge.
    if (texel.a <= 0.5)
        return vec4(0.0);

    return vec4(texel.rgb * (u_LightmapIntensity / texel.a), 1.0);
}

#endif // LIGHTMAP_SAMPLING_GLSL
