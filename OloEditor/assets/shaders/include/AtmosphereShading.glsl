// =============================================================================
// AtmosphereShading.glsl — the shared MEDIA-OCCLUSION block (issues #633, #723)
//
// The weather director's global wetness signal, the cloudscape's ground
// shadow, and the two volumetric-shadow cascade transforms — shared by every
// surface-lighting consumer (PBR forward/deferred, terrain, and — because this
// include is deliberately self-contained, no PBRCommon dependency — the froxel
// fog scatter compute, the cloud raymarch, and the volumetric-shadow
// generator).
//
// CPU twin: UBOStructures::AtmosphereShadingUBO (ShaderBindingLayout.h),
// uploaded once per frame at UBO_ATMOSPHERE_SHADING (54); a zeroed upload
// disables every effect it gates. Cloud shadow map: TEX_CLOUD_SHADOW (62), R8,
// 1 = unshadowed transmittance (see CloudShadow_Generate.comp).
//
// #723 widened this block rather than taking the engine's LAST free UBO
// binding (83): both volumetric consumers already read it, and it was already
// carrying a dead 64-byte `u_CloudShadowViewProj` mat4 — declared "reserved",
// never written by the CPU, never read by any shader — which the two cascade
// transforms reclaimed. The volumetric SAMPLER and its helpers live in
// include/VolumetricShadowCommon.glsl, which includes this file; only the
// transforms are here, so a surface shader that wants wetness does not also
// acquire a sampler3D it never reads.
// =============================================================================

#ifndef ATMOSPHERE_SHADING_GLSL
#define ATMOSPHERE_SHADING_GLSL

// Cascade indices into the arrays below (and into the z-stacked volume at
// TEX_VOLUMETRIC_SHADOW). Mirrors AtmosphereShadingUBO::kVsmCascades == 2.
#define VSM_CASCADE_CLOUD 0
#define VSM_CASCADE_FOG   1
#define VSM_CASCADE_COUNT 2

layout(std140, binding = 54) uniform AtmosphereShadingData {
    // ⚠ TWO SPACES IN ONE BLOCK, ON PURPOSE, AND THEY ARE NOT INVERSES.
    // RelWorldToVsmTex takes RENDER-RELATIVE world (issue #429 — the space
    // every consumer's fragments already carry, same as u_AtmoParams1 below).
    // VsmTexToAbsWorld produces ABSOLUTE world because its only consumer is the
    // generator, which evaluates cloud/fog density fields defined in absolute
    // world space. Each maps the cascade's OWN unit cube; the z-stacking of the
    // cascades inside the shared volume is folded in by the sampler.
    mat4 u_RelWorldToVsmTex[VSM_CASCADE_COUNT]; // render-relative world -> cascade [0,1]^3
    mat4 u_VsmTexToAbsWorld[VSM_CASCADE_COUNT]; // cascade [0,1]^3 -> ABSOLUTE world (generator only)
    vec4 u_AtmoParams0;                         // x = wetness [0,1], y = cloud shadow strength, z = cloud shadow enabled, w = unused
    vec4 u_AtmoParams1;                         // xy = shadow map center (world xz), z = world size, w = 1/world size
    vec4 u_VsmParams[VSM_CASCADE_COUNT];        // x = enabled, y = strength, z = march step length (m), w = unused
    vec4 u_VsmVolume;                           // x = slices per cascade, y = cascade count, z = 1/(slices*cascades), w = XY texels
};

layout(binding = 62) uniform sampler2D u_CloudShadowMap;

// Cloud shadow factor for a world position: 1 = fully lit. Applies to the
// DIRECTIONAL light only (the body the clouds occlude), independent of the
// CSM gate — clouds shadow the ground even when shadow mapping is off.
//
// SPACE CONTRACT: worldPos is RENDER-RELATIVE (the space mesh shaders carry
// in v_WorldPos under camera-relative rendering, issue #429). The CPU upload
// shifts the shadow-map center by the render origin so Params1.xy is also
// render-relative — callers with absolute positions must subtract the origin.
float atmosphereCloudShadow(vec3 worldPos)
{
    if (u_AtmoParams0.z < 0.5)
        return 1.0;
    vec2 uv = (worldPos.xz - u_AtmoParams1.xy) * u_AtmoParams1.w + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0; // outside the moving window: unshadowed
    float transmittance = texture(u_CloudShadowMap, uv).r;
    // Fade the effect near the window edge so the boundary never pops.
    vec2 edge = min(uv, 1.0 - uv);
    float edgeFade = smoothstep(0.0, 0.05, min(edge.x, edge.y));
    return mix(1.0, transmittance, clamp(u_AtmoParams0.y, 0.0, 1.0) * edgeFade);
}

// Surface wetness response: darker, glossier up-facing surfaces while the
// weather director's wetness signal is high (rain-soaked look). Vertical
// surfaces shed water — the effect scales with how up-facing the normal is.
void atmosphereApplyWetness(inout vec3 albedo, inout float roughness, vec3 worldNormal)
{
    float wetness = clamp(u_AtmoParams0.x, 0.0, 1.0);
    if (wetness <= 1.0e-3)
        return;
    float upFacing = clamp(worldNormal.y, 0.0, 1.0);
    float w = wetness * mix(0.25, 1.0, upFacing);
    albedo *= mix(1.0, 0.72, w);
    roughness = mix(roughness, 0.08, w * 0.85);
}

#endif // ATMOSPHERE_SHADING_GLSL
