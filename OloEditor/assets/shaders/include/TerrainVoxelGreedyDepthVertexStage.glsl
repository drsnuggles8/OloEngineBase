#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyVertex.glsl"
#endif
#include "VoxelQuadUnpack.glsl"

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): two-stream vertex pull. Stream 0
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
#include "InstanceBlock_Vertex.glsl"

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

    gl_Position = u_ViewProjection * instances[0].Transform * vec4(localPos, 1.0);
#ifdef OLO_VSM_FAMILY
    gl_Position = vsmFamilyPosition(gl_Position);
#endif
}
