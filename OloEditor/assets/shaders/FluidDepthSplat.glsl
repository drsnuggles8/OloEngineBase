// FluidDepthSplat.glsl — screen-space fluid depth splat (issue #630 pillar B).
//
// Instanced camera-facing quads (unit quad + SSBO-indexed instancing, one
// instance per particle) raster sphere impostors into an R32F target owned by
// FluidIntermediatesPass: each fragment computes the analytic sphere surface,
// writes gl_FragDepth for nearest-splat z-testing against the pass's own
// depth buffer, discards behind the opaque scene depth, and outputs the
// POSITIVE view-space depth in metres. Target is cleared to 0 = "no fluid".
//
// Camera-relative rendering (issue #429): SSBO positions are ABSOLUTE world;
// the shared camera UBO (binding 0) is render-relative, so the vertex stage
// subtracts u_RenderOrigin before the view transform.

#type vertex
#version 460 core

#include "include/CameraCommon.glsl"
#include "include/FluidSplatCommon.glsl"

layout(location = 0) in vec2 a_QuadPos; // unit quad corner in [-0.5, 0.5]

layout(location = 0) out vec2 v_UV;         // [-1, 1] across the splat
layout(location = 1) out vec3 v_ViewCenter; // particle centre, view space

void main()
{
    // Vulkan GLSL builtin (shaderc pipeline); equals gl_InstanceID here since
    // the pass draws with a zero base instance.
    uint instanceIndex = uint(gl_InstanceIndex);
    if (!FluidSplatAlive(instanceIndex))
    {
        v_UV = vec2(0.0);
        v_ViewCenter = vec3(0.0);
        gl_Position = FluidSplatDegenerate();
        return;
    }

    float radius = u_FluidRender.TintRadius.w;

    // Absolute world -> render-relative -> view space. Building the corner in
    // view space makes the quad camera-facing by construction.
    vec3 relPos = positions[instanceIndex].xyz - u_RenderOrigin;
    vec3 viewCenter = (u_View * vec4(relPos, 1.0)).xyz;

    v_UV = a_QuadPos * 2.0;
    v_ViewCenter = viewCenter;

    vec3 viewCorner = viewCenter + vec3(v_UV * radius, 0.0);
    gl_Position = u_Projection * vec4(viewCorner, 1.0);
}

#type fragment
#version 460 core

#include "include/CameraCommon.glsl"
#include "include/FluidSplatCommon.glsl"

layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec3 v_ViewCenter;

// Scene depth for behind-geometry rejection. Water-identical slot + uniform
// name (binding 39, u_SceneDepth) so IsKnownTextureBinding passes.
layout(binding = 39) uniform sampler2D u_SceneDepth;

layout(location = 0) out float o_ViewDepth; // positive metres in front of the camera

void main()
{
    float r2 = dot(v_UV, v_UV);
    if (r2 > 1.0)
    {
        discard; // outside the sphere silhouette
    }

    float radius = u_FluidRender.TintRadius.w;
    float nz = sqrt(1.0 - r2);

    // Sphere impostor: view space looks down -Z, so +nz bulges the surface
    // toward the camera.
    vec3 viewSurface = v_ViewCenter + radius * vec3(v_UV, nz);

    vec4 clipPos = u_Projection * vec4(viewSurface, 1.0);
    float windowDepth = clamp((clipPos.z / clipPos.w) * 0.5 + 0.5, 0.0, 1.0);

    // Reject fluid behind opaque geometry (+ small bias against shimmer at
    // fluid/geometry contact).
    vec2 screenUV = gl_FragCoord.xy * u_FluidRender.ScreenParams.zw;
    float sceneDepth = texture(u_SceneDepth, screenUV).r;
    if (windowDepth > sceneDepth + 0.0005)
    {
        discard;
    }

    gl_FragDepth = windowDepth;
    o_ViewDepth = -viewSurface.z;
}
