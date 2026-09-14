// =============================================================================
// DDGIRelocateParams.glsl — the batched relocation dispatch's capture set
// (binding 7, issue #846)
//
// A BLOCK OF ITS OWN RATHER THAN THREE MORE FIELDS IN DDGIPassData, and the
// reason is upload volume, not tidiness. DDGIPassData is re-uploaded once per
// CASTER per cube face per captured probe by DDGIProbeUpdatePass::CaptureProbe;
// this array is 1 KB and is read by exactly one dispatch per frame. Carrying it
// inside that block would have multiplied every one of those capture uploads by
// ~3.5x, and on the Vulkan backend each upload is a fresh frame-arena push —
// which at the Ultra budget is how a DDGI frame runs the arena out and starts
// dropping draws with nothing in the log (issue #1185's shape).
//
// It costs no binding. UBO_USER_0 is a PASS-LOCAL slot that its occupant
// refills immediately before its own work: DDGIPassData, VRCS ShadingRateParams
// and this block already share it at three different sizes, and the relocation
// dispatch binds this buffer directly before dispatching.
//
// C++ twin: UBOStructures::DDGIRelocateParamsUBO in ShaderBindingLayout.h.
// =============================================================================

#ifndef DDGI_RELOCATE_PARAMS_GLSL
#define DDGI_RELOCATE_PARAMS_GLSL

// Capture-set array length. C++ mirror:
// UBOStructures::DDGIRelocateParamsUBO::MaxRelocationBatch, pinned against it
// by BindlessShaderPipeline.DDGIShaderConstantsMatchTheCppMirrors.
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

layout(std140, binding = 7) uniform DDGIRelocateParams
{
    // One entry per work group of the batched relocation dispatch:
    // x = global probe index, y = DDGI_RELOCATE_ENTRY_* flags. Entries at or
    // past the dispatch's group count are never written and never read.
    ivec4 u_DDGICaptureSet[DDGI_RELOCATE_BATCH]; // 0
};                                              // 1024 bytes total

#endif // DDGI_RELOCATE_PARAMS_GLSL
