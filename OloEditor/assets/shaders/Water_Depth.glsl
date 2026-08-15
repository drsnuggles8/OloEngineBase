// =============================================================================
// Water_Depth.glsl — depth-only variant of Water.glsl for the surface-depth
// capture (underwater fog). Shares the exact VS/TCS/TES displacement chain via
// the include stage files so the captured depth can never drift from the drawn
// surface; the fragment stage keeps ONLY the waterline-side discard and writes
// no color outputs, so the capture framebuffer needs no scene-MRT mirroring
// (#691 Phase 8 — this retires the RGBA16F/RED_INTEGER/RG16F padding
// attachments the capture target used to carry for pipeline-interface parity).
// CommandDispatch swaps this program in while WaterDepthCaptureActive, the same
// shape as the depth-prepass shader swap.
// =============================================================================

#type vertex
#version 460 core
#include "include/WaterVertexStage.glsl"

#type tess_control
#version 460 core
#include "include/WaterTessControlStage.glsl"

#type tess_evaluation
#version 460 core
#include "include/WaterTessEvalStage.glsl"

#type fragment
#version 460 core

// Full varying interface declared (matching the shared TES outputs) even
// though only v_WorldPos is read — a declared input consumes its location,
// so the pipeline interface stays warning-free under Vulkan validation.
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_ViewDir;
layout(location = 4) in vec3 v_Tangent;
layout(location = 5) in vec3 v_Bitangent;
layout(location = 6) in float v_WaveHeight;
layout(location = 7) in vec3 v_PrevWorldPos;

// No color outputs — the capture target is depth-only.

// Camera UBO (binding 0) — must match the shared stages' declaration exactly
// (GL links all stages into one program and rejects mismatched blocks).
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
    mat4 u_ProjectionForReconstruction;
};

// Water UBO (binding 23) — same exact-match rule as CameraMatrices.
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;
    vec4 u_WaterDeepColor;
    vec4 u_VisualParams;
    vec4 u_NormalMapScroll;
    vec4 u_NormalMapSpeed;
    vec4 u_LightDirection;
    vec4 u_ScreenParams;
    vec4 u_DepthRefractionParams;
    vec4 u_RefractionColor;
    vec4 u_FoamParams;
    vec4 u_FoamParams2;
    vec4 u_SSSColor;
    vec4 u_SSRParams;
    vec4 u_TessParams;
    vec4 u_FFTParams;
};

void main()
{
    // Waterline-side discard — identical to Water.glsl's color FS so the
    // captured depth covers exactly the fragments the color pass shades.
    // When render-from-below is enabled (u_NormalMapSpeed.w > 0.5) the water
    // draws double-sided; keep only the face whose side the camera is
    // actually on relative to THIS fragment.
    if (u_NormalMapSpeed.w > 0.5)
    {
        bool cameraBelowFragment = v_WorldPos.y > u_CameraPosition.y;
        if (cameraBelowFragment == gl_FrontFacing)
            discard;
    }
    // Depth writes happen via the fixed-function pipeline; nothing to emit.
}
