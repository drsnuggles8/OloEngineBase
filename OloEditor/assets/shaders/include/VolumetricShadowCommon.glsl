// =============================================================================
// VolumetricShadowCommon.glsl — sampling the shared volumetric shadow map
// (issue #723)
//
// One home for how a scattering march asks "how much medium is between me and
// the light?", so the cloud raymarch (PostProcess_Cloudscape.glsl), the froxel
// fog scatter (compute/FroxelFogScatter.comp) and the generator itself
// (compute/VolumetricShadow_Generate.comp) all agree — the
// light-path-photometric-parity discipline applied to media.
//
// WHAT THE VOLUME HOLDS. TEX_VOLUMETRIC_SHADOW (66) is an R32F sampler3D whose
// texel is the OPTICAL DEPTH accumulated FROM THE LIGHT down to that point, not
// a transmittance. Optical depth is what the cloud consumer needs: it still has
// to run Beer-powder and a reduced-extinction multi-scatter octave on the
// value, and neither survives being handed exp(-od). Transmittance is one
// exp() away (`volumetricShadowTransmittance`).
//
// TWO CASCADES, STACKED IN Z. Cascade c owns slices
// [c * slices, (c+1) * slices). The two media are four orders of magnitude
// apart in scale — a kilometres-thick cloud deck and a hundred-metre fog bank —
// so one shared box would be useless for both. `vsmCascadeW` clamps the
// per-cascade w to the cascade's own half-texel interior, which is why a
// trilinear tap can never blend cloud optical depth into a fog sample.
//
// Requires: AtmosphereShading.glsl for the cascade transforms (UBO 54).
// =============================================================================

#ifndef VOLUMETRIC_SHADOW_COMMON_GLSL
#define VOLUMETRIC_SHADOW_COMMON_GLSL

#include "AtmosphereShading.glsl"

layout(binding = 66) uniform sampler3D u_VolumetricShadowVolume;

// Map a cascade-local w in [0,1] onto the shared volume's w, clamped to that
// cascade's half-texel interior. The clamp does two jobs at once:
//   * no trilinear tap ever crosses a cascade boundary;
//   * a point past the far face reads the column TOTAL (the last slice), which
//     is the physically right extrapolation — it is fully behind the medium.
float vsmCascadeW(float cascadeW, int cascade)
{
    float slices = max(u_VsmVolume.x, 1.0);
    float w = clamp(cascadeW, 0.5 / slices, 1.0 - 0.5 / slices);
    return (float(cascade) * slices + w * slices) * u_VsmVolume.z;
}

// Optical depth from the light to `relWorldPos`, along the light direction the
// cascade was fitted to.
//
// SPACE CONTRACT: relWorldPos is RENDER-RELATIVE — the same space
// atmosphereCloudShadow() takes, and the space mesh shaders carry in
// v_WorldPos (issue #429). A caller holding absolute-world positions (the cloud
// raymarch does) must subtract the render origin.
//
// Returns 0 (fully lit) outside the cascade's footprint or when the cascade is
// disabled, with a smooth edge fade so the footprint boundary never pops —
// mirroring atmosphereCloudShadow's moving-window treatment.
float volumetricShadowOpticalDepth(vec3 relWorldPos, int cascade)
{
    if (u_VsmParams[cascade].x < 0.5)
        return 0.0;

    vec3 tex = (u_RelWorldToVsmTex[cascade] * vec4(relWorldPos, 1.0)).xyz;
    if (any(lessThan(tex.xy, vec2(0.0))) || any(greaterThan(tex.xy, vec2(1.0))))
        return 0.0; // outside the fitted window: unshadowed

    float opticalDepth = texture(u_VolumetricShadowVolume, vec3(tex.xy, vsmCascadeW(tex.z, cascade))).r;

    vec2 edge = min(tex.xy, 1.0 - tex.xy);
    float edgeFade = smoothstep(0.0, 0.04, min(edge.x, edge.y));
    return max(opticalDepth, 0.0) * u_VsmParams[cascade].y * edgeFade;
}

// Beer-Lambert transmittance from the light to `relWorldPos`: 1 = fully lit.
float volumetricShadowTransmittance(vec3 relWorldPos, int cascade)
{
    return exp(-volumetricShadowOpticalDepth(relWorldPos, cascade));
}

#endif // VOLUMETRIC_SHADOW_COMMON_GLSL
