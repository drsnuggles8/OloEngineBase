// FluidComposite.glsl — screen-space fluid shading + composite (issue #630
// pillar B). Fullscreen triangle over the scene framebuffer (SceneColor RMW
// chain, after WaterPass): every pixel covered by fluid (smoothed view-depth
// > 0) is shaded — normal from smoothed depth, refraction of the pre-fluid
// scene colour copy, Beer–Lambert absorption by accumulated thickness,
// Fresnel-weighted environment reflection, speed-derived foam — and written
// to all four scene MRT targets. Non-fluid pixels are discarded (depth test,
// depth writes and blending are all OFF; FluidCompositePass owns the state).

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

#include "include/CameraCommon.glsl"
#include "include/FluidRenderCommon.glsl"

layout(location = 0) in vec2 v_TexCoord;

// Fluid intermediates (FluidIntermediatesPass outputs)
layout(binding = 54) uniform sampler2D u_FluidDepth;     // R32F smoothed view depth (positive metres, 0 = no fluid)
layout(binding = 55) uniform sampler2D u_FluidThickness; // RG16F: r = thickness, g = speed-weighted thickness

// Scene inputs (water-identical slots + uniform names)
layout(binding = 39) uniform sampler2D u_SceneDepth;        // opaque scene depth
layout(binding = 40) uniform sampler2D u_RefractionTexture; // pre-fluid scene colour copy
layout(binding = 9) uniform samplerCube u_EnvironmentMap;   // global environment/skybox (Counts.z gates sampling)

// MRT outputs matching SceneRenderPass format
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
layout(location = 3) out vec2 o_Velocity;

// Screen-space refraction offset strength (UV units per unit normal.xy at
// full thickness saturation).
const float kRefractStrength = 0.03;

// Octahedral encode: unit normal -> RG16F [-1,1]^2.
// Copied from Water.glsl (octEncode, ~line 515) — keep identical.
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Reconstruct the view-space position from a positive view depth using the
// projection's focal terms: x_ndc = P00 * x_view / d  =>  x_view = x_ndc * d / P00.
vec3 ReconstructViewPos(vec2 uv, float viewDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * viewDepth / u_Projection[0][0],
                ndc.y * viewDepth / u_Projection[1][1],
                -viewDepth);
}

void main()
{
    float centerDepth = texture(u_FluidDepth, v_TexCoord).r;
    if (centerDepth <= 0.0)
    {
        discard; // no fluid at this pixel
    }

    vec2 texelSize = u_FluidRender.ScreenParams.zw;
    vec3 viewPos = ReconstructViewPos(v_TexCoord, centerDepth);

    // Guard against smoothing bleed pushing fluid behind opaque geometry:
    // compare the smoothed surface's window depth against the scene depth.
    vec4 clipPos = u_Projection * vec4(viewPos, 1.0);
    float windowDepth = clamp((clipPos.z / clipPos.w) * 0.5 + 0.5, 0.0, 1.0);
    float sceneDepth = texture(u_SceneDepth, v_TexCoord).r;
    if (windowDepth > sceneDepth + 0.001)
    {
        discard;
    }

    // --- Normal from smoothed depth ---------------------------------------
    // Central differences with the standard min-artifact trick: on each axis
    // take the neighbour whose depth is closer to the centre, so silhouette
    // edges (and no-fluid sentinels) don't produce skewed normals.
    float depthRight = texture(u_FluidDepth, v_TexCoord + vec2(texelSize.x, 0.0)).r;
    float depthLeft = texture(u_FluidDepth, v_TexCoord - vec2(texelSize.x, 0.0)).r;
    float depthUp = texture(u_FluidDepth, v_TexCoord + vec2(0.0, texelSize.y)).r;
    float depthDown = texture(u_FluidDepth, v_TexCoord - vec2(0.0, texelSize.y)).r;

    vec3 ddx = ReconstructViewPos(v_TexCoord + vec2(texelSize.x, 0.0), depthRight) - viewPos;
    if (depthLeft > 0.0 &&
        (depthRight <= 0.0 || abs(depthLeft - centerDepth) < abs(depthRight - centerDepth)))
    {
        ddx = viewPos - ReconstructViewPos(v_TexCoord - vec2(texelSize.x, 0.0), depthLeft);
    }

    vec3 ddy = ReconstructViewPos(v_TexCoord + vec2(0.0, texelSize.y), depthUp) - viewPos;
    if (depthDown > 0.0 &&
        (depthUp <= 0.0 || abs(depthDown - centerDepth) < abs(depthUp - centerDepth)))
    {
        ddy = viewPos - ReconstructViewPos(v_TexCoord - vec2(0.0, texelSize.y), depthDown);
    }

    vec3 viewNormal = normalize(cross(ddx, ddy));
    if (viewNormal.z < 0.0)
    {
        viewNormal = -viewNormal; // always face the camera (+Z in view space)
    }

    // --- Thickness + refraction --------------------------------------------
    vec2 thicknessSample = texture(u_FluidThickness, v_TexCoord).rg;
    float thickness = max(thicknessSample.r, 0.0);
    float foamThickness = max(thicknessSample.g, 0.0);

    vec2 refractUV = v_TexCoord + viewNormal.xy * kRefractStrength * clamp(thickness, 0.0, 1.0);
    refractUV = clamp(refractUV, texelSize, vec2(1.0) - texelSize);
    vec3 refracted = texture(u_RefractionTexture, refractUV).rgb;

    // Beer–Lambert absorption.
    // CPU mirror: OloEngine/src/OloEngine/Renderer/FluidShading.h
    // (FluidShading::Transmittance) — keep formula-identical. Single-scatter
    // approximation: what the absorption removes re-emerges as the tint.
    vec3 transmittance = exp(-u_FluidRender.AbsorptionParams.xyz * u_FluidRender.AbsorptionParams.w * thickness);
    vec3 refractedAbsorbed = refracted * transmittance + u_FluidRender.TintRadius.xyz * (1.0 - transmittance);

    // --- Fresnel + reflection ----------------------------------------------
    vec3 viewDir = normalize(-viewPos);
    float cosTheta = max(dot(viewNormal, viewDir), 0.0);
    const float kF0 = 0.02; // water-like IOR 1.33
    float fresnel = kF0 + (1.0 - kF0) * pow(1.0 - cosTheta, 5.0);

    vec3 reflection;
    if (u_FluidRender.Counts.z != 0u)
    {
        // View-space reflection lifted to world orientation: u_View is a
        // rigid transform, so transpose(mat3) is its inverse rotation.
        vec3 reflectView = reflect(-viewDir, viewNormal);
        vec3 reflectWorld = transpose(mat3(u_View)) * reflectView;
        reflection = texture(u_EnvironmentMap, reflectWorld).rgb;
    }
    else
    {
        // No environment map bound: fresnel-weighted sky-tint fallback.
        reflection = mix(vec3(0.35, 0.45, 0.60), u_FluidRender.TintRadius.xyz, 0.35);
    }

    vec3 color = mix(refractedAbsorbed, reflection, fresnel);

    // --- Foam ---------------------------------------------------------------
    // Coverage = intensity * (speed-weighted thickness / total thickness).
    float foam = clamp(u_FluidRender.FoamParams.y * foamThickness / max(thickness, 1.0e-3), 0.0, 1.0);
    color = mix(color, vec3(0.95), foam);

    // --- MRT ------------------------------------------------------------------
    o_Color = vec4(color, 1.0);
    o_EntityID = int(u_FluidRender.Counts.y);
    o_ViewNormal = octEncode(viewNormal);

    // Velocity: static-fluid approximation — reproject the reconstructed
    // surface point with the current and previous render-relative VPs, so
    // only CAMERA motion produces motion vectors (per-particle motion is not
    // carried through the depth intermediates). u_View is render-relative, so
    // its rigid inverse yields the render-relative world position both VPs expect.
    vec3 worldRel = transpose(mat3(u_View)) * (viewPos - u_View[3].xyz);
    vec4 clipCurr = u_ViewProjection * vec4(worldRel, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(worldRel, 1.0);
    vec2 ndcCurr = clipCurr.xy / max(clipCurr.w, 1.0e-6);
    vec2 ndcPrev = clipPrev.xy / max(clipPrev.w, 1.0e-6);
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
