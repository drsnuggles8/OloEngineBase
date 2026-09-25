#ifndef SNOW_LAYER_GLSL
#define SNOW_LAYER_GLSL

// =============================================================================
// SnowLayer.glsl — snow as a MATERIAL LAYER, one definition for every path
// (issue #1451).
//
// A snow weight w in [0, 1] says how much of a surface is covered. Every lit
// path treats that fraction the same way, through the functions below, so a
// snowy scene is the same picture on Forward, Forward+ and Deferred:
//
//   1. THE MATERIAL IS BLENDED TOWARD SNOW before any light is evaluated —
//      albedo, roughness, metallic (to 0), material AO (toward 1: snow fills
//      the crevices) and emission (covered by (1 - w)). The ordinary closure
//      then lights the blended surface, so snow is shadowed, lit by every
//      light the path has (loop, clustered tiles, ReSTIR DI on Deferred), and
//      takes its ambient from the shared ladder with the screen-space AO —
//      nothing about snow is a second estimate of a term another owner has.
//   2. THE NORMAL IS FILLED, THEN ROUGHENED. The FILLED normal
//      (oloSnowLayerFilledNormal: the surface normal pulled toward world up by
//      0.6 w) is what the depth prepass and the G-Buffer store, so screen-space
//      AO sees snow-filled geometry on every path. The SHADING normal adds the
//      crystalline micro-perturbation on top, and is derived from the filled
//      normal alone — which is what lets the deferred lighting pass rebuild it
//      from the G-Buffer exactly.
//   3. SPARKLE is a specular glint lobe, evaluated for DIRECTIONAL lights
//      inside each path's light loop and gated by that light's visibility.
//      Directional lights are the ones every path evaluates in its loop
//      (Forward+ and Deferred hand the punctual lights to tiles or to ReSTIR
//      DI), so this is the one light set that sparkles identically everywhere.
//   4. SUBSURFACE is the screen-space blur of the DIFFUSE half, handed over in
//      scene attachment 4 as (diffuse, -w) — include/SnowDiffusionCommon.glsl.
//
// On the deferred path the weight travels in G-Buffer RT3.a — the "material
// profile" channel of #1256, the continuous position along a material's
// profile axis, which is exactly what a snow blend is. See GBuffer.h.
//
// Self-contained, so the G-Buffer writers (which include no PBR helpers) use
// exactly the functions the lit passes use. A shader that only READS the weight
// (the deferred lighting pass) defines OLO_SNOW_LAYER_NO_COVERAGE before
// including this, which leaves out the coverage computation and the wind block
// it needs.
// =============================================================================

#ifdef OLO_SNOW_LAYER_NO_COVERAGE
#define OLO_SNOW_COMMON_NO_WIND 1
#endif
#include "SnowCommon.glsl"
#include "SnowDiffusionCommon.glsl"

// Snow UBO (binding 13) — SnowUBOData in Renderer/PostProcessSettings.h.
layout(std140, binding = 13) uniform SnowParams {
    vec4 u_SnowCoverageParams;       // (heightStart, heightFull, slopeStart, slopeFull)
    vec4 u_SnowAlbedoAndRoughness;   // (albedo.rgb, roughness)
    vec4 u_SnowSSSColorAndIntensity; // (sssColor.rgb, sssIntensity) — read by the blur pass
    vec4 u_SnowSparkleParams;        // (sparkleIntensity, sparkleDensity, sparkleScale, normalPerturbStrength)
    vec4 u_SnowFlags;                // (enabled, windDriftFactor, pad, pad)
};

bool oloSnowLayerEnabled()
{
    return u_SnowFlags.x > 0.5;
}

bool oloSnowLayerActive(float weight)
{
    return weight > OLO_SNOW_MIN_WEIGHT;
}

#ifndef OLO_SNOW_LAYER_NO_COVERAGE
// Procedural coverage from ABSOLUTE world height and the VERTEX normal's slope,
// with the wind-drift bias. Absolute, not render-relative: the snow line is a
// world altitude and must not move when the render origin re-centres (#429).
// The vertex normal, not the shading one: a normal map must not decide where
// snow lies, or every bump would carry its own speck of snow.
float oloSnowLayerCoverage(vec3 absWorldPos, vec3 vertexNormal)
{
    if (!oloSnowLayerEnabled())
        return 0.0;
    return computeSnowWeight(absWorldPos.y, normalize(vertexNormal), u_SnowCoverageParams.x,
                             u_SnowCoverageParams.y, u_SnowCoverageParams.z, u_SnowCoverageParams.w,
                             u_SnowFlags.y);
}
#endif

// Blend the surface's material toward snow. `emissive` is covered: snow over a
// lamp hides the lamp, in proportion to how much of it is covered.
void oloSnowLayerBlendMaterial(float weight, inout vec3 albedo, inout float metallic, inout float roughness,
                               inout float ao, inout vec3 emissive)
{
    if (!oloSnowLayerActive(weight))
        return;
    albedo = mix(albedo, u_SnowAlbedoAndRoughness.rgb, weight);
    roughness = mix(roughness, u_SnowAlbedoAndRoughness.w, weight);
    metallic = mix(metallic, 0.0, weight);
    // Snow fills crevices, so the material's own occlusion fades under it.
    ao = mix(ao, 1.0, weight * 0.6);
    emissive *= (1.0 - weight);
}

// The snow-FILLED normal: what the prepass, scene attachment 2 and G-Buffer RT1
// store, and what screen-space AO therefore sees. Deliberately free of the
// crystalline perturbation — fed to AO, that micro-detail reads as grey
// occlusion speckle across every snowfield.
vec3 oloSnowLayerFilledNormal(vec3 N, float weight)
{
    if (!oloSnowLayerActive(weight))
        return N;
    return normalize(mix(N, vec3(0.0, 1.0, 0.0), weight * 0.6));
}

// The SHADING normal, from the filled normal alone so the deferred lighting
// pass can rebuild it from what the G-Buffer stored.
vec3 oloSnowLayerShadingNormal(vec3 filledN, vec3 absWorldPos, float weight)
{
    if (!oloSnowLayerActive(weight))
        return filledN;
    vec3 snowN = perturbSnowNormal(filledN, absWorldPos, u_SnowSparkleParams.w);
    return normalize(mix(filledN, snowN, weight));
}

// The glint lobe for ONE directional light: radiance in, cosine and Fresnel
// applied here, visibility applied by the caller with the light's own shadow.
vec3 oloSnowLayerSparkle(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 absWorldPos, float weight)
{
    if (!oloSnowLayerActive(weight))
        return vec3(0.0);
    float sparkle = snowSparkle(V, N, L, absWorldPos, u_SnowSparkleParams.x, u_SnowSparkleParams.y,
                                u_SnowSparkleParams.z);
    if (sparkle <= 0.0)
        return vec3(0.0);
    // Schlick's Fresnel for ice (F0 = 0.04), written out rather than taken
    // from PBRCommon so this file stands alone.
    float cosTheta = clamp(dot(normalize(V + L), V), 0.0, 1.0);
    float F = 0.04 + 0.96 * pow(1.0 - cosTheta, 5.0);
    return vec3(weight * sparkle * F) * radiance * max(dot(N, L), 0.0);
}

#endif // SNOW_LAYER_GLSL
