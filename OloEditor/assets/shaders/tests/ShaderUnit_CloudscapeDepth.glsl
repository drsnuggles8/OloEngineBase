#version 460 core

// =============================================================================
// ShaderUnit_CloudscapeDepth.glsl
//
// L2 probe for the production half-resolution depth reduction and exact sky
// sentinel in CloudscapeDepth.glsl. Each compute invocation corresponds to one
// half-resolution cloud pixel and writes {conservative depth, has geometry}.
// =============================================================================

layout(local_size_x = 1, local_size_y = 1) in;

layout(binding = 0) uniform sampler2D u_DepthTexture;

#define OLO_CLOUDSCAPE_DEPTH_TEXTURE u_DepthTexture
#include "../include/CloudscapeDepth.glsl"
#undef OLO_CLOUDSCAPE_DEPTH_TEXTURE

layout(std430, binding = 1) writeonly buffer Outputs
{
    vec2 u_Outputs[];
};

void main()
{
    ivec2 halfSize = (textureSize(u_DepthTexture, 0) + ivec2(1)) / 2;
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(coord, halfSize)))
        return;

    float depth = cloudConservativeDepth(coord);
    uint index = uint(coord.y * halfSize.x + coord.x);
    u_Outputs[index] = vec2(depth, cloudDepthContainsGeometry(depth) ? 1.0 : 0.0);
}
