// =============================================================================
// Terrain_VoxelDepth.glsl - Voxel Override Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System (Phase 6)
// Simple VS → FS pipeline (no tessellation)
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal; // Unused but matches VoxelVertex layout

// Camera UBO (binding 0) — holds light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_CameraPosition;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

void main()
{
    OLO_INSTANCE_FORWARD();
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // Depth-only pass — no color output needed
}
