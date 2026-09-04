// =============================================================================
// Water_Depth.glsl — depth-only variant of Water.glsl for the surface-depth
// capture (underwater fog). Shares the exact VS/TCS/TES displacement chain via
// the include stage files so the captured depth can never drift from the drawn
// surface; the fragment stage keeps ONLY the waterline-side discard and writes
// no color outputs, so the capture framebuffer needs no scene-MRT mirroring
// (#691 — this retires the RGBA16F/RED_INTEGER/RG16F padding
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
// The depth TES emits ONLY v_WorldPos (see the include: SPIR-V dead-strips
// unread FS inputs, so any other real output would be a
// written-but-unconsumed interface warning per depth pipeline).
#define OLO_WATER_DEPTH_ONLY 1
#include "include/WaterTessEvalStage.glsl"

#type fragment
#version 460 core

// The one varying the waterline discard needs — the depth TES emits
// nothing else (OLO_WATER_DEPTH_ONLY).
layout(location = 0) in vec3 v_WorldPos;

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
    vec4 u_FFTParams;              // x = cascade count (0 = Gerstner), y = 1/L0, z = heightScale, w = horizontalScale
    // FFT ocean cascades (issue #969). C++ twin:
    // UBOStructures::WaterUBO::FFTCascadeParams, packed by
    // Ocean::PackCascadeShaderParams. Declared in EVERY stage of the water
    // programs, identically, for the reason the #967/#968 fields below are.
    //
    // u_FFTParams.x carries the CASCADE COUNT rather than a 0/1 flag — 1 is the
    // single-cascade fallback and 3 the band-limited preset, so every existing
    // `u_FFTParams.x > 0.5` test still means "FFT is on" and did not change.
    vec4 u_FFTCascadeParams;       // x = 1/L1 (mid tile), y = 1/L2 (fine tile), z = cos(theta_mid), w = sin(theta_mid)
    // Boat / actor wake foam field (issue #967). C++ twin:
    // UBOStructures::WaterUBO::WakeFieldParams / WakeFieldParams2. Declared in
    // EVERY stage of the water programs, identically, because GL requires a
    // uniform block shared across a program's stages to be declared the same
    // way in each — appending to only the stage that reads it is a link error,
    // not a silent mismatch. Only the fragment stage actually reads them.
    vec4 u_WakeFieldParams;        // xy = field window centre (world XZ), z = 1/fieldExtent, w = intensity (<=0 disables)
    vec4 u_WakeFieldParams2;       // x = wake fade start (m), y = wake fade end (m), z = edge-fade start, w = unused

    // Boat / actor wake SHAPE (issue #968). C++ twin:
    // UBOStructures::WaterUBO::WakeShapeParams / WakeHulls; GLSL evaluator:
    // include/WaterWakeCommon.glsl. Declared in EVERY stage of the water
    // programs, identically, for the same reason the #967 fields above are: GL
    // requires a uniform block shared across a program's stages to be declared
    // the same way in each, so appending to only the stages that read it is a
    // LINK error rather than a silent mismatch. Read by the vertex and
    // tess-eval stages, which is where the surface is displaced.
    vec4 u_WakeShapeParams;        // x = live hull count, y = height scale (<=0 disables), z = hull flatten strength, w = reserved
    // Shore wave deformation (issue #1033). C++ twin:
    // UBOStructures::WaterUBO::ShoreParams / ShoreParams2; the encoding contract
    // and every relation driven by it live in Renderer/Water/WaterShoreDepth.h,
    // the GLSL evaluator in include/WaterShoreCommon.glsl. Declared in EVERY
    // stage of the water programs, identically, for the same reason the #967 /
    // #968 fields above are: GL requires a uniform block shared across a
    // program's stages to be declared the same way in each, so appending to only
    // the stages that read it is a LINK error rather than a silent mismatch.
    // Read by the vertex and tess-eval stages (which displace the surface) and
    // by the fragment stage (which fades the breaker foam).
    //
    // xy = seabed depth-field window centre (world XZ),
    // z  = 1 / window extent in metres (the UV scale),
    // w  = enable. w <= 0 IS the disabled state; there is no separate flag, so a
    //      frame whose bake did not run cannot leave a stale field showing.
    vec4 u_ShoreParams;
    // x = breaker index (the a/h limit the surf zone breaks at; 0.39 is the
    //     physical value — WaterShoreDepth.h :: kBreakerIndex),
    // y = breaking foam gain, z/w = reserved.
    vec4 u_ShoreParams2;
    // Rain-impact ripples (issue #1034, §7.3). C++ twin:
    // UBOStructures::WaterUBO::RainRippleParams / RainRippleParams2; the
    // contract and every constant the field is built from live in
    // Renderer/Water/WaterRainRipples.h, the evaluator in
    // include/WaterRainCommon.glsl. Declared in EVERY stage of the water
    // programs, identically, for the same reason the #967 / #968 / #1033 fields
    // above are: GL requires a uniform block shared across a program's stages to
    // be declared the same way in each, so appending to only the stage that
    // reads it is a LINK error rather than a silent mismatch. Only Water.glsl's
    // fragment stage reads them — the ripples are normal-only and never displace.
    //
    // x = strength (artist gain x live precipitation intensity), y = density,
    // z = cell size (m), w = unused. x <= 0 IS the disabled state.
    vec4 u_RainRippleParams;
    // x = ripple fade start (m), y = fade end (m), z/w unused.
    vec4 u_RainRippleParams2;
    // Advected foam field (issue #1034, §2.2). C++ twin:
    // UBOStructures::WaterUBO::FoamFieldParams; the contract is
    // Renderer/Water/WaterFoam.h, the sampler include/WaterFoamCommon.glsl.
    // Declared in EVERY stage of the water programs, identically, for the same
    // reason every block above is. Only Water.glsl's fragment stage reads it.
    //
    // The .g channel of the SAME disturbance texture u_WakeFieldParams
    // describes — same window, same lattice, same edge fade
    // (u_WakeFieldParams2.z), different channel. It carries its own copy of the
    // window because the two gate independently: open-ocean whitecaps advect in
    // a scene with no boat in it, and u_WakeFieldParams is all-zero there.
    //
    // xy = window centre (world XZ), z = 1 / field extent, w = intensity.
    // w <= 0 IS the disabled state.
    vec4 u_FoamFieldParams;
    // 80 = WaterWake::kHullVec4Count (4 hulls x 20 vec4). The layout is
    // WaterWake.h's, verbatim; WATER_WAKE_* in WaterWakeCommon.glsl mirrors the
    // offsets so nothing here indexes it by a bare literal.
    vec4 u_WakeHulls[80];
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
