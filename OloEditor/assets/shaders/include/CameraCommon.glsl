#ifndef CAMERA_COMMON_GLSL
#define CAMERA_COMMON_GLSL

// Shared camera uniform block — matches CameraUBO in ShaderBindingLayout.h
// Include this file instead of redeclaring the block in every shader stage.
//
// u_PrevViewProjection is the previous frame's view-projection matrix. It
// pairs with the instance SSBO's PrevTransform (when applicable) so forward-path
// shaders can emit screen-space motion vectors to scene FB RT3 for TAA.
// Static geometry produces zero velocity because PrevTransform == Transform and
// world position is identical between frames.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    // Camera-relative render origin (issue #429). Geometry is drawn with world
    // positions shifted by this, so an interpolated worldPos is RELATIVE to it.
    // Lighting/fog differences (cameraPos - worldPos, lightPos - worldPos) are
    // invariant, but shaders that sample an ABSOLUTE-world *pattern* (triplanar
    // tiling, procedural noise, world-anchored wave phase, world-grid/clipmap)
    // must add it back: absWorldPos = worldPos + u_RenderOrigin. Zero within the
    // first grid cell (near origin), so the add-back is a no-op there.
    vec3 u_RenderOrigin;
    // The selected lighting tap (issue #1526, OLO_LIGHTING_TAP_* in
    // include/PBRCommon.glsl), as a float in what was this block's padding
    // lane. Set on the main view's colour passes only; 0 everywhere else.
    float u_LightingTap;
    // The SHADER-RECONSTRUCTION flavour of u_Projection (#691).
    // u_Projection carries the rasterizer flavour (Vulkan: y flip + z remap
    // into [0,1]) and is ONLY for gl_Position. Any math that re-applies the
    // GL depth remap itself — `(clip.z/clip.w) * 0.5 + 0.5`, near/far
    // extraction from rows 2/3, `inverse(...)` unprojection at ndc z = ±1 —
    // must read THIS member instead, or the remap is applied twice on Vulkan
    // (depths squeeze into [0.5, 1] and every reconstruction lands wrong).
    // Identical to u_Projection on GL. Note [1][1] is NEGATIVE on Vulkan in
    // both flavours — wrap in abs() where a magnitude (projection scale) is
    // wanted.
    mat4 u_ProjectionForReconstruction;
    // Screen-space AO for the FORWARD shaders' ambient term (issue #1452):
    // x = live, y = strength. Zero on every view but the main view's forward
    // colour passes. Read through include/ForwardScreenSpaceAO.glsl.
    vec4 u_ScreenSpaceAOParams;
    // The TAA jitter's screen offset (#1552): xy this frame's, zw the previous
    // frame's, in velocity units (NDC times 0.5, the uploaded projection's
    // convention). Zero on every unjittered view. Read through
    // oloVelocityFromNdc below.
    vec4 u_JitterUV;
};

// oloVelocityFromNdc, the one velocity every writer emits (#1552).
#include "ScreenVelocity.glsl"

#endif // CAMERA_COMMON_GLSL
