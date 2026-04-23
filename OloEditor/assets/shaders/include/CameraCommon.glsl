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
};

#endif // CAMERA_COMMON_GLSL
