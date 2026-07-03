// =============================================================================
// DepthPrepass.glsl - Depth-only prepass shader for opaque static meshes
//
// Bound by CommandDispatch in place of the full PBR forward / G-Buffer shader
// while the scene depth prepass is active, so covered fragments never run the
// expensive lighting shader — the prepass exists to ELIMINATE overdraw, not
// multiply the per-pixel cost by it.
//
// The position math and `invariant gl_Position` MUST stay bit-identical to
// the vertex stages of PBR_MultiLight.glsl / PBR_GBuffer.glsl: the color pass
// re-draws the same geometry with glDepthFunc(GL_LEQUAL) and depth writes
// off, so any rounding difference fails the depth test and punches holes.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Instance transforms SSBO (binding 15)
#include "include/InstanceBlock_Vertex.glsl"

invariant gl_Position;

void main()
{
    OLO_INSTANCE_FORWARD();
    // Same association as PBR_MultiLight / PBR_GBuffer: world position first,
    // then view-projection — required for invariant depth between the passes.
    vec3 worldPos = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // Depth is written by the rasterizer; color writes are masked off.
}
