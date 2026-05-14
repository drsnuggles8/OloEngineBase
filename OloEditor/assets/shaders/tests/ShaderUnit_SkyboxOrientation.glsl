#version 460 core

// =============================================================================
// ShaderUnit_SkyboxOrientation.glsl
//
// GPU probe for the visible skybox sampling convention. Samples the production
// GetSkyboxSampleDirection helper at representative screen/world directions so
// renderer tests can catch vertical cubemap inversions without relying on a
// full golden-image scene.
// =============================================================================

layout(local_size_x = 1) in;

layout(binding = 9) uniform samplerCube u_Skybox;
layout(std430, binding = 0) writeonly buffer Outputs { vec4 u_Outputs[]; };

#include "../include/SkyboxSampling.glsl"

vec3 GetProbeDirection(uint index)
{
    if (index == 0u)
    {
        // Top-center screen ray for an identity skybox camera looking down -Z.
        return vec3(0.0, 0.65, -1.0);
    }
    if (index == 1u)
    {
        // Bottom-center screen ray for the same camera.
        return vec3(0.0, -0.65, -1.0);
    }
    if (index == 2u)
    {
        // Mostly-up world ray; slight -Z avoids sampling exactly on an edge.
        return vec3(0.0, 1.0, -0.15);
    }

    // Mostly-down world ray; slight -Z avoids sampling exactly on an edge.
    return vec3(0.0, -1.0, -0.15);
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= 4u)
        return;

    vec3 direction = normalize(GetProbeDirection(index));
    u_Outputs[index] = texture(u_Skybox, GetSkyboxSampleDirection(direction));
}
