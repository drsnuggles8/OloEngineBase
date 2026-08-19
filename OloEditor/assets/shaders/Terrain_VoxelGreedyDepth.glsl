// =============================================================================
// Terrain_VoxelGreedyDepth.glsl - Depth-only shadow pass for the packed-quad
// voxel path (issue #727).
//
// Instanced sibling of Terrain_VoxelDepth.glsl. Must reconstruct the quad
// EXACTLY the way Terrain_VoxelGreedy.glsl does, or the shadow silhouette
// drifts from the lit one — a divergence that renders as peter-panning or a
// shadow of geometry that is not there.
// =============================================================================

#type vertex
#version 460 core

#include "include/VoxelQuadUnpack.glsl"

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5, amendment (76)): two-stream vertex pull. Stream 0
// (binding 57) = shared unit quad {vec2 corner}; stream 1 (binding 63) = the
// per-chunk instance VB {uint geometry, uint material}. The material is unused
// by a depth stage but the stride must still match the C++ layout.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloInstancePull
{
    uint v[];
} b_Instances;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec2 a_Corner;
layout(location = 1) in int  a_QuadGeometry;
layout(location = 2) in int  a_QuadMaterial; // Unused, matches the instance layout
#endif

// Camera UBO (binding 0) — holds light VP during the shadow pass
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_CameraPosition;
};

// This stage's consumer never reads v_InstanceIndex.
#define OLO_INSTANCE_SINGLE 1
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

void main()
{
#ifdef OLO_PULLED_VERTEX
    int cornerBase = gl_VertexIndex * 2;
    vec2 a_Corner = vec2(b_Vertices.v[cornerBase + 0], b_Vertices.v[cornerBase + 1]);
    int instanceBase = gl_InstanceIndex * 2;
    uint geometryWord = b_Instances.v[instanceBase + 0];
#else
    uint geometryWord = uint(a_QuadGeometry);
#endif
    OLO_INSTANCE_FORWARD();

    OloVoxelQuad quad = oloUnpackVoxelQuad(geometryWord, 0u);
    vec3 localPos = oloVoxelQuadCorner(quad, a_Corner);

    gl_Position = u_ViewProjection * u_Model * vec4(localPos, 1.0);
}

#type fragment
#version 460 core

void main()
{
    // Depth-only pass — no color output needed
}
