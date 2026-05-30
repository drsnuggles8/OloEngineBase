// =============================================================================
// IrradianceConvolution.glsl - Irradiance Convolution for IBL
// Part of OloEngine PBR System
// Generates irradiance map from environment cubemap
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

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
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

layout(binding = 9) uniform samplerCube u_EnvironmentMap;

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

            irradiance += texture(u_EnvironmentMap, sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }

    irradiance = PI * irradiance * (1.0 / float(nrSamples));

    o_Color = vec4(irradiance, 1.0);
}
