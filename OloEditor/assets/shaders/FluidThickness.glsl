// FluidThickness.glsl — additive fluid thickness accumulation (issue #630
// pillar B). Same instanced sphere-impostor geometry as FluidDepthSplat.glsl,
// but every overlapping splat ADDS its chord length (ONE/ONE blending set by
// FluidIntermediatesPass, depth test off, no gl_FragDepth) into an RG16F
// target: r = geometric thickness (metres), g = speed-weighted thickness —
// the composite derives foam coverage as g / r.
//
// Camera-relative rendering (issue #429): see FluidDepthSplat.glsl.

#type vertex
#version 460 core

#include "include/CameraCommon.glsl"
#include "include/FluidSplatCommon.glsl"

layout(location = 0) in vec2 a_QuadPos; // unit quad corner in [-0.5, 0.5]

layout(location = 0) out vec2 v_UV;         // [-1, 1] across the splat
layout(location = 1) out vec3 v_ViewCenter; // particle centre, view space
layout(location = 2) out float v_Speed;     // |velocity| of this particle (m/s)

void main()
{
    // Vulkan GLSL builtin (shaderc pipeline); equals gl_InstanceID here since
    // the pass draws with a zero base instance.
    uint instanceIndex = uint(gl_InstanceIndex);
    if (!FluidSplatAlive(instanceIndex))
    {
        v_UV = vec2(0.0);
        v_ViewCenter = vec3(0.0);
        v_Speed = 0.0;
        gl_Position = FluidSplatDegenerate();
        return;
    }

    float radius = u_FluidRender.TintRadius.w;

    vec3 relPos = positions[instanceIndex].xyz - u_RenderOrigin;
    vec3 viewCenter = (u_View * vec4(relPos, 1.0)).xyz;

    v_UV = a_QuadPos * 2.0;
    v_ViewCenter = viewCenter;
    v_Speed = length(velocities[instanceIndex].xyz);

    vec3 viewCorner = viewCenter + vec3(v_UV * radius, 0.0);
    gl_Position = u_Projection * vec4(viewCorner, 1.0);
}

#type fragment
#version 460 core

#include "include/CameraCommon.glsl"
#include "include/FluidSplatCommon.glsl"

layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec3 v_ViewCenter;
layout(location = 2) in float v_Speed;

// Scene depth for behind-geometry rejection (binding 39, water-identical name).
layout(binding = 39) uniform sampler2D u_SceneDepth;

layout(location = 0) out vec2 o_Thickness; // r = thickness, g = speed-weighted thickness

void main()
{
    float r2 = dot(v_UV, v_UV);
    if (r2 > 1.0)
    {
        discard;
    }

    float radius = u_FluidRender.TintRadius.w;
    float nz = sqrt(1.0 - r2);

    // Same analytic sphere surface as the depth splat, used only for the
    // scene-depth rejection here — depth test is off and no depth is written.
    vec3 viewSurface = v_ViewCenter + radius * vec3(v_UV, nz);
    vec4 clipPos = u_Projection * vec4(viewSurface, 1.0);
    float windowDepth = clamp((clipPos.z / clipPos.w) * 0.5 + 0.5, 0.0, 1.0);

    vec2 screenUV = gl_FragCoord.xy * u_FluidRender.ScreenParams.zw;
    float sceneDepth = texture(u_SceneDepth, screenUV).r;
    if (windowDepth > sceneDepth + 0.0005)
    {
        discard;
    }

    // Chord length through the sphere at this fragment.
    float chord = 2.0 * radius * nz;

    // Foam numerator: full weight from 1.5x the speed threshold, none below
    // 0.5x — the composite normalises by total thickness.
    float speedThreshold = u_FluidRender.FoamParams.x;
    float foamWeight = smoothstep(0.5 * speedThreshold, 1.5 * speedThreshold, v_Speed);

    o_Thickness = vec2(chord, chord * foamWeight);
}
