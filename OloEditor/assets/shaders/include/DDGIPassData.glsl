// =============================================================================
// DDGIPassData.glsl — the DDGI pass-local per-dispatch block (binding 7)
//
// Declared ONCE here rather than copied into each DDGI shader (issues #632 /
// #707): the block grew from 160 to 400 bytes when the compute stages arrived,
// and four hand-copied declarations that must agree byte-for-byte with the C++
// twin is precisely the two-mirrors-drift shape this repo keeps postmortems
// about. C++ twin: DDGIPassDataUBO in DDGIProbeUpdatePass.cpp.
//
// UBO_USER_0 (binding 7) is a PASS-LOCAL slot — the DDGI pass owns it for the
// duration of its own dispatches and the post-process chain refills it later in
// the frame. No new binding is consumed.
// =============================================================================

#ifndef DDGI_PASS_DATA_GLSL
#define DDGI_PASS_DATA_GLSL

// Bit flags in u_DDGIComputeParams.w.
#define DDGI_PASS_FLAG_CASCADE_SHIFTED 1
#define DDGI_PASS_FLAG_DEPTH_VALID 2

// Capture-set array length (issue #846). C++ mirror:
// UBOStructures::DDGIPassDataUBO::MaxRelocationBatch, pinned against it by
// BindlessShaderPipeline.DDGIShaderConstantsMatchTheCppMirrors.
#define DDGI_RELOCATE_BATCH 64

// Per-entry flag in u_DDGICaptureSet[i].y: this capture is a periodic REFRESH
// of an already-placed probe, not a first placement. See DDGI_Relocate.comp
// for why that must not re-run the relocation spring.
//
// It is PER PROBE, not per dispatch. It was DDGI_PASS_FLAG_REFRESH_CAPTURE in
// u_DDGIComputeParams.w while relocation ran one dispatch per probe; batching
// the dispatch made a whole-dispatch flag wrong, because a single capture set
// mixes settled probes with probes still converging.
#define DDGI_RELOCATE_ENTRY_REFRESH 1

layout(std140, binding = 7) uniform DDGIPassData
{
    mat4 u_DDGIModel;               //   0 — capture: render-relative model matrix
    mat4 u_DDGINormalMatrix;        //  64 — capture: transpose(inverse(model))
    vec4 u_DDGIBaseColor;           // 128 — capture: material base color factor
    vec4 u_DDGIProbePosition;       // 144 — xyz = render-relative probe pos, w = GLOBAL probe index
    mat4 u_DDGIInvViewProjection;   // 160 — PREVIOUS frame's WORLD inverse view-projection
    vec4 u_DDGIRenderOrigin;        // 224 — xyz = render origin (world), w = camera seed radius (world units)
    vec4 u_DDGICameraPosRel;        // 240 — xyz = render-relative camera position
    ivec4 u_DDGIComputeParams;      // 256 — x = total probes, y = screen width, z = screen height, w = flags
    ivec4 u_DDGIPrevLattice[8];     // 272 — previous frame's per-cascade lattice min (xyz)
    // Relocation capture set, one entry per work group of the batched
    // relocation dispatch: x = global probe index, y = DDGI_RELOCATE_ENTRY_*
    // flags. Entries at or past the dispatch's group count are never read.
    ivec4 u_DDGICaptureSet[DDGI_RELOCATE_BATCH]; // 400
};                                  // 1424 bytes total

#endif // DDGI_PASS_DATA_GLSL
