// =============================================================================
// Foliage_Depth.glsl - Shadow depth pass for instanced foliage
// Matches Foliage_Instance.glsl vertex layout with alpha test support
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V8 foliage two-stream pull — stream 0 = the
// 20-byte card quad on the engine-wide binding 57, stream 1 = FoliageRenderer's
// 48-byte per-instance VB {PositionScale, RotationHeight, ColorAlpha} on the
// reserved stream-1 binding 63, indexed by gl_InstanceIndex. Pulled locals
// under the attribute names in main() keep the body shared (Foliage_Instance
// carries the canonical comment).
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloBonePull
{
    float v[];
} b_Instances;
#define OLO_PULLED_VERTEX 1
#else
// Per-vertex attributes (unit quad)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

// Per-instance attributes
layout(location = 2) in vec4 a_PositionScale;   // xyz = world pos, w = scale
layout(location = 3) in vec4 a_RotationHeight;  // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 4) in vec4 a_ColorAlpha;       // rgb = tint, a = alpha cutoff
#endif

// Camera UBO (binding 0) — contains light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

// Foliage UBO (binding 12)
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
};

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out float v_AlphaCutoff;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
    int instBase = gl_InstanceIndex * 12;
    vec4 a_PositionScale = vec4(b_Instances.v[instBase + 0], b_Instances.v[instBase + 1],
                                b_Instances.v[instBase + 2], b_Instances.v[instBase + 3]);
    vec4 a_RotationHeight = vec4(b_Instances.v[instBase + 4], b_Instances.v[instBase + 5],
                                 b_Instances.v[instBase + 6], b_Instances.v[instBase + 7]);
    vec4 a_ColorAlpha = vec4(b_Instances.v[instBase + 8], b_Instances.v[instBase + 9],
                             b_Instances.v[instBase + 10], b_Instances.v[instBase + 11]);
#endif
    OLO_INSTANCE_FORWARD();
    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float height = a_RotationHeight.y;

    vec3 localPos = a_Position;
    localPos.x *= scale;
    localPos.y *= height * scale;

    float cosR = cos(rotation);
    float sinR = sin(rotation);
    vec3 rotatedPos;
    rotatedPos.x = localPos.x * cosR - localPos.z * sinR;
    rotatedPos.y = localPos.y;
    rotatedPos.z = localPos.x * sinR + localPos.z * cosR;

    // Wind (must match main shader for consistent shadows)
    float windInfluence = a_Position.y;
    float windPhase = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_Time * u_WindSpeed;
    float wind = sin(windPhase) * cos(windPhase * 0.7 + 1.3) * u_WindStrength * windInfluence;
    rotatedPos.x += wind;
    rotatedPos.z += wind * 0.5;

    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos = (u_Model * vec4(instancePos + rotatedPos, 1.0)).xyz;

    v_TexCoord = a_TexCoord;
    v_AlphaCutoff = a_ColorAlpha.a;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in float v_AlphaCutoff;

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
#endif

// Foliage UBO (binding 12) — shared with vertex stage
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
};

void main()
{
    float alpha = texture(u_DiffuseTexture, v_TexCoord).a;
    if (alpha < v_AlphaCutoff)
        discard;
}
