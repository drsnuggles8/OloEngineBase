// =============================================================================
// IrradianceConvolution.glsl - Irradiance Convolution for IBL
// Part of OloEngine PBR System
// Generates irradiance map from environment cubemap
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
#define u_EnvironmentMap OLO_HEAP_TEX_CUBE(9)  // TEX_ENVIRONMENT
#else
layout(binding = 9) uniform samplerCube u_EnvironmentMap;
#endif

// Branchless OrthonormalBasis + PI.
#include "include/MathCommon.glsl"

void main()
{
    vec3 N = normalize(v_LocalPos);

    vec3 irradiance = vec3(0.0);

    // Tangent space. Branchless basis (Duff et al. 2017) — also removes the NaN
    // the old fixed up=(0,1,0) + normalize(cross(up,N)) produced when N pointed
    // straight up/down (cross collapsed to zero). The hemisphere convolution is
    // rotation-invariant about N, so the irradiance result is unchanged.
    // The brute-force sampleDelta grid is kept deliberately: this is the simple
    // fallback for the importance-sampled IrradianceConvolutionAdvanced path and
    // only runs once at bake, so its sample count is not on any frame budget.
    vec3 right, up;
    OrthonormalBasis(N, right, up);

    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // Spherical to cartesian (in tangent space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            // Tangent space to world
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            // textureLod(..., 0.0) for the same reason as IBLPrefilter.glsl: the
            // sky cubemap is mipmapped now (#943) and this convolution must keep
            // reading the base level, not whatever an implicit LOD would pick.
            irradiance += textureLod(u_EnvironmentMap, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }

    irradiance = PI * irradiance * (1.0 / float(nrSamples));

    o_Color = vec4(irradiance, 1.0);
}
