// =============================================================================
// OcclusionProxy.glsl - Minimal shader for occlusion query proxy boxes
// Transforms vertices using camera VP and a model matrix push constant.
// No fragment output — used with color writes disabled for GL_ANY_SAMPLES_PASSED.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform Camera {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

uniform mat4 u_Model;

void main()
{
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // No color output — this shader is only used for depth testing
    // during occlusion query passes with color writes disabled.
}
