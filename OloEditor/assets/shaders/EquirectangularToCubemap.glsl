// =============================================================================
// EquirectangularToCubemap.glsl - Converts Equirectangular HDR to Cubemap
// Part of OloEngine PBR System
// Converts equirectangular (panoramic) HDR images to cubemap format
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). This bake draws MeshPrimitives::CreateSkyboxCube(), whose
// stream is the engine `Vertex` (32 B: vec3 position @0, vec3 normal @12,
// vec2 uv @24), so the stride is 8 floats even though this stage only needs
// the position. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(location = 0) out vec3 v_LocalPos;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_EquirectangularMap OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_EquirectangularMap;
#endif

const vec2 invAtan = vec2(0.1591, 0.3183);

vec2 SampleSphericalMap(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

void main()
{
    vec2 uv = SampleSphericalMap(normalize(v_LocalPos));
    vec3 color = texture(u_EquirectangularMap, uv).rgb;

    o_Color = vec4(color, 1.0);
}
