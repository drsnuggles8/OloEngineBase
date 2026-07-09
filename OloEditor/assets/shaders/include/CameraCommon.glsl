#ifndef CAMERA_COMMON_GLSL
#define CAMERA_COMMON_GLSL

// Shared camera uniform block — matches CameraUBO in ShaderBindingLayout.h
// Include this file instead of redeclaring the block in every shader stage.
//
// u_PrevViewProjection is the previous frame's view-projection matrix. It
// pairs with ModelMatrices::u_PrevModel (when applicable) so forward-path
// shaders can emit screen-space motion vectors to scene FB RT3 for TAA.
// Static geometry produces zero velocity because u_PrevModel == u_Model and
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
    float _padding1;
};

#endif // CAMERA_COMMON_GLSL
