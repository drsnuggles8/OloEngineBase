#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// Output: RGB = accumulated inscatter light, A = transmittance
// Compositing happens in the upsample/composite pass
layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 19) uniform sampler2D u_DepthTexture;  // Full-res scene depth

// Integrated froxel fog volume (issue #435): rgb = accumulated in-scatter,
// a = transmittance, built by the VolumetricFogPass compute chain. The
// volumetric branch below fetches it with one trilinear tap — this replaced
// the old per-pixel screen-space raymarch (temporal accumulation now lives in
// the 3D scatter volume, sun shadowing + local-light scattering happen in
// FroxelFogScatter.comp).
layout(binding = 53) uniform sampler3D u_FroxelFogVolume;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): the fog reconstructs an ABSOLUTE world
    // position (world inverse-VP), so it only needs the render origin to lift the
    // relative u_CameraPosition to absolute world. The full tail (incl. an unused
    // u_PrevViewProjection) keeps this block a prefix of the shared CameraMatrices
    // so the cross-shader layout check stays happy; the reprojection prev-VP is
    // read from MotionBlurUBO (u_MB) below, not from here.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// (The old per-pixel CSM light-shaft lookup — the ShadowData block + CSM
// sampler — moved into FroxelFogScatter.comp with the raymarch, issue #435.)

// Motion blur UBO (binding 8) — inverse VP for depth reconstruction
// Instance-named so its members don't sit at global scope — otherwise its
// u_PrevViewProjection collides with the same-named member the CameraMatrices
// block above now declares for std140 layout parity (issue #429).
layout(std140, binding = 8) uniform MotionBlurUBO
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
} u_MB;

// Froxel fog parameters (binding 46) — mirrors UBOStructures::FroxelFogUBO.
// The fragment only consumes the depth-slicing params + the enabled flag in
// u_FroxelRenderOrigin.w (0 = froxel chain did not run, fall back to
// analytic); the matrices drive the compute chain.
layout(std140, binding = 46) uniform FroxelFogData
{
    mat4 u_FroxelInverseView;
    mat4 u_FroxelInverseProjection;
    mat4 u_FroxelPrevViewProjection;
    vec4 u_FroxelDims;        // xyz = volume dims
    vec4 u_FroxelDepthParams; // x = near, y = far, z = log2(far/near)
    vec4 u_FroxelRenderOrigin; // xyz = render origin, w = enabled
};

// Include fog UBO, noise functions, phase functions
#include "include/FogCommon.glsl"

// Include local fog volume evaluation
#include "include/FogVolumeCommon.glsl"

// ---------------------------------------------------------------------------
// Reconstruct world position from depth
// ---------------------------------------------------------------------------
vec3 worldPosFromDepth(vec2 uv, float depth)
{
    vec4 ndcPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_MB.u_InverseViewProjection * ndcPos;
    return worldPos4.xyz / worldPos4.w;
}

// ---------------------------------------------------------------------------
// Main — analytic fog, or one trilinear fetch of the integrated froxel fog
// volume (issue #435; replaces the old per-pixel screen-space raymarch)
// ---------------------------------------------------------------------------
void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Early out for disabled fog
    if (u_FogFlags.x < 0.5)
    {
        o_Color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Camera-relative (issue #429): worldPosFromDepth inverts the WORLD
    // view-projection, so worldPos is already ABSOLUTE world (see the identity in
    // RenderPipeline::PrepareFrame). u_CameraPosition comes from the render-
    // relative camera UBO, so lift it to absolute world with the render origin —
    // then all of the fog math (distances, height plane, fog volumes) is
    // consistently in absolute world space.
    vec3 worldPos = worldPosFromDepth(v_TexCoord, depth);
    vec3 cameraPos = u_CameraPosition + u_RenderOrigin;

    // Unpack fog parameters
    float baseDensity    = u_FogColorAndDensity.a;
    float heightFalloff  = u_FogDistanceParams.z;
    float heightOffset   = u_FogDistanceParams.w;
    float maxOpacity     = u_FogRayleighColorAndMaxOpacity.a;
    vec3  fogColor       = u_FogColorAndDensity.rgb;
    float absorptionCoeff = u_FogVolumetricParams.y;

    bool volumetric  = u_FogFlags.w > 0.5;
    bool froxelReady = u_FroxelRenderOrigin.w > 0.5;

    // Analytical mode — also the fallback when the froxel chain did not run
    // this frame (its disabled UBO zeroes the enabled flag).
    if (!volumetric || !froxelReady)
    {
        // Analytical fog — simple per-pixel closed form
        vec3 analyticFog = applyFog(vec3(0.0), worldPos, cameraPos);
        // Compute fog factor for transmittance
        int mode = int(u_FogFlags.y + 0.5);
        float dist = distance(worldPos, cameraPos);
        float distFog = computeDistanceFog(dist, mode, baseDensity, u_FogDistanceParams.x, u_FogDistanceParams.y);
        float heightFog = computeHeightFog(worldPos, cameraPos, baseDensity, heightFalloff, heightOffset);
        float fogFactor = clamp(max(distFog, heightFog), 0.0, maxOpacity);

        // Add local fog volume contribution at the fragment position
        FogVolumeResult volResult = evaluateFogVolumesAtPoint(worldPos);
        if (volResult.density > 0.001)
        {
            float volFogFactor = clamp(volResult.density, 0.0, 1.0);
            fogFactor = clamp(fogFactor + volFogFactor, 0.0, maxOpacity);
            analyticFog = mix(analyticFog, volResult.color * volFogFactor, volFogFactor / max(fogFactor, 0.001));
        }

        o_Color = vec4(analyticFog, 1.0 - fogFactor);
        return;
    }

    // ===== FROXEL VOLUMETRIC FOG (issue #435) =====
    // The VolumetricFogPass compute chain already injected media density,
    // scattered the sun (CSM-shadowed god rays) and every clustered local
    // light, accumulated temporally, and integrated front-to-back. One
    // trilinear tap fetches the in-scatter + transmittance between the camera
    // and this fragment's depth.

    float froxelNear = u_FroxelDepthParams.x;
    float froxelFar = u_FroxelDepthParams.y;
    float froxelLogFN = max(u_FroxelDepthParams.z, 1e-4);

    // Positive view-space depth of the fragment (u_View is render-relative)
    vec3 relPos = worldPos - u_RenderOrigin;
    float viewDepth = -(u_View * vec4(relPos, 1.0)).z;

    // Exponential-slice W coordinate — the inverse of the compute chain's
    // slice placement, so the fetch lands between the camera and the fragment.
    float clampedDepth = max(viewDepth, froxelNear);
    float w = clamp(log2(clampedDepth / froxelNear) / froxelLogFN, 0.0, 1.0);

    vec4 vol = texture(u_FroxelFogVolume, vec3(v_TexCoord, w));
    vec3 accumulatedLight = vol.rgb;
    float transmittance = vol.a;

    // Analytic tail beyond the froxel volume's far plane: the volume covers
    // [near, froxelFar]; geometry farther than that gets the remaining path
    // fogged with the closed-form height-fog density at the fragment.
    float rayDistFull = distance(worldPos, cameraPos);
    float secant = rayDistFull / max(viewDepth, 1e-4);
    float maxRayDist = max(u_FogDistanceParams.y, 500.0);
    float rayLen = (depth > 0.9999) ? maxRayDist : min(rayDistFull, maxRayDist);
    float volumeFarLen = froxelFar * secant;
    if (rayLen > volumeFarLen)
    {
        float tailLen = rayLen - volumeFarLen;
        float tailDensity = fogDensityAtPoint(worldPos, baseDensity, heightFalloff, heightOffset);
        float tailExtinction = tailDensity + absorptionCoeff;
        float tailTransmittance = exp(-tailExtinction * tailLen);
        accumulatedLight += transmittance * fogColor * tailDensity *
                            (1.0 - tailTransmittance) / max(tailExtinction, 1e-5);
        transmittance *= tailTransmittance;
    }

    // Clamp by max opacity + firefly clamp (matches the old raymarch)
    transmittance = max(transmittance, 1.0 - maxOpacity);
    accumulatedLight = min(accumulatedLight, vec3(10.0));

    o_Color = vec4(accumulatedLight, transmittance);
}
