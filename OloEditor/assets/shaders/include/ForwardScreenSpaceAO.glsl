// =============================================================================
// ForwardScreenSpaceAO.glsl — the screen-space AO buffer, read by a FORWARD
// shader for its ambient term (issue #1452).
//
// Screen-space AO is visibility for the AMBIENT term and nothing else
// (docs/agent-rules/lighting-signal-contract.md, rule 3). On the forward paths
// the AO buffer is built from the DEPTH PREPASS — which writes the same
// normal-mapped view normals the colour pass would — and the AO passes run
// between that prepass and forward colour. So by the time a forward shader
// shades a pixel the buffer already exists, and the shader multiplies its
// ambient term by it exactly as DeferredLighting multiplies the ambient split.
// PostProcess_SSAOApply no longer multiplies the composed colour on any path.
//
// ONE UPSAMPLE, THE SAME NUMBER. The value comes from oloSampleScreenSpaceAO
// (ScreenSpaceAOSampling.glsl), the bilateral upsample DeferredLighting runs,
// with the same depth linearisation, so a forward pixel and a deferred pixel of
// the same surface are multiplied by the same visibility.
//
// REQUIREMENTS
//   * The CameraMatrices block (include/CameraCommon.glsl) is declared before
//     this file: it supplies u_ScreenSpaceAOParams (x = live, y = strength) and
//     u_ProjectionForReconstruction (the depth linearisation pair).
//   * TEX_SSAO (20) is the AO buffer and TEX_POSTPROCESS_DEPTH (19) the
//     full-resolution scene depth COPY the prepass exported — a separate
//     texture, so no pass samples the depth attachment it is testing against.
//     CommandDispatch publishes both for every forward draw; with AO off they
//     are a white placeholder and x is 0, and this returns 1 without sampling.
// =============================================================================

#ifndef FORWARD_SCREEN_SPACE_AO_GLSL
#define FORWARD_SCREEN_SPACE_AO_GLSL

#include "BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_ForwardScreenSpaceAO OLO_HEAP_TEX_2D(20) // TEX_SSAO
#define u_ForwardSceneDepth OLO_HEAP_TEX_2D(19)    // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 20) uniform sampler2D u_ForwardScreenSpaceAO; // TEX_SSAO
layout(binding = 19) uniform sampler2D u_ForwardSceneDepth;    // TEX_POSTPROCESS_DEPTH
#endif

#define OLO_SSAO_TAP_DEPTH(uv) texture(u_ForwardSceneDepth, (uv)).r
#include "ScreenSpaceAOSampling.glsl"

// The screen-space ambient visibility of the pixel at `fragCoord`
// (gl_FragCoord.xy), strength applied. 1.0 when no AO technique is live.
float oloForwardScreenSpaceAO(vec2 fragCoord)
{
    if (u_ScreenSpaceAOParams.x < 0.5)
        return 1.0;
    vec2 uv = fragCoord / vec2(textureSize(u_ForwardSceneDepth, 0));
    return oloScreenSpaceAOVisibility(
        oloSampleScreenSpaceAO(u_ForwardScreenSpaceAO, uv, u_ProjectionForReconstruction[2][2],
                               u_ProjectionForReconstruction[3][2]),
        u_ScreenSpaceAOParams.y);
}

#endif // FORWARD_SCREEN_SPACE_AO_GLSL
