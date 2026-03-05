#ifndef CAMERA_COMMON_GLSL
#define CAMERA_COMMON_GLSL

// Shared camera uniform block — matches CameraUBO in ShaderBindingLayout.h
// Include this file instead of redeclaring the block in every shader stage.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

#endif // CAMERA_COMMON_GLSL
