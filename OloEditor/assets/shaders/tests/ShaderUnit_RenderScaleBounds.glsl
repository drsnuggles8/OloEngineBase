#version 460 core

// =============================================================================
// ShaderUnit_RenderScaleBounds.glsl
//
// L2 probe for include/RenderScaleBounds.glsl (issue #1537): the mapping every
// screen-space pass uses between TEXTURE uv (what it samples) and FRAME uv
// (what the camera matrices speak) under a dynamic render scale.
//
// The bounds come from the inputs, not the DRS UBO: the probe defines
// OLO_RENDER_SCALE_BOUNDS to a global it sets per invocation, which is the
// override a shader whose binding 33 is taken uses.
//
// Per case i, inputs:  [2i]     = (bounds.xy, textureUV.xy)
//                      [2i + 1] = (frameUV.xy, 0, 0)
//             outputs: [2i]     = (oloFrameUV(textureUV), oloTextureUV(frameUV))
//                      [2i + 1] = (oloRenderScaleBounds(), in-frame flag, 0)
// =============================================================================

layout(local_size_x = 1, local_size_y = 1) in;

layout(std430, binding = 2) readonly buffer Inputs
{
    vec4 u_Inputs[];
};

layout(std430, binding = 1) writeonly buffer Outputs
{
    vec4 u_Outputs[];
};

vec2 g_ProbeBounds = vec2(1.0);
#define OLO_RENDER_SCALE_BOUNDS g_ProbeBounds
#include "../include/RenderScaleBounds.glsl"

void main()
{
    uint i = gl_GlobalInvocationID.x;
    vec4 a = u_Inputs[2u * i];
    vec4 b = u_Inputs[2u * i + 1u];
    g_ProbeBounds = a.xy;
    u_Outputs[2u * i] = vec4(oloFrameUV(a.zw), oloTextureUV(b.xy));
    u_Outputs[2u * i + 1u] = vec4(oloRenderScaleBounds(), oloInRenderedFrame(a.zw) ? 1.0 : 0.0, 0.0);
}
