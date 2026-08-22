// =============================================================================
// Terrain_VoxelDepth.glsl - Voxel Override Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System
// Simple VS → FS pipeline (no tessellation)
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is the marching-cubes chunk VBO
// (Terrain/Voxel/MarchingCubes.cpp VoxelVertex — 24 B: vec3 Position @0,
// vec3 Normal @12), so the stride is 6 floats, NOT the 8-float engine
// Vertex; the normal is unused by this depth stage. The GL attribute branch
// below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal; // Unused but matches VoxelVertex layout
#endif

// Camera UBO (binding 0) — holds light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_CameraPosition;
};

// Model UBO (binding 3)
// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 6;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    OLO_INSTANCE_FORWARD();
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // Depth-only pass — no color output needed
}
