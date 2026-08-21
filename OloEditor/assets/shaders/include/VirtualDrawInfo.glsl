#ifndef VIRTUAL_DRAW_INFO_GLSL
#define VIRTUAL_DRAW_INFO_GLSL

// =============================================================================
// VirtualDrawInfo.glsl — the ONE GLSL spelling of the per-draw info UBO shared
// by every virtual-geometry raster / resolve / shadow pipeline
// (binding 49 == ShaderBindingLayout::UBO_VIRTUAL_DRAW), mirroring
// VirtualDrawInfoGpu in Renderer/VirtualGeometry/VirtualMeshGpuData.h.
//
// Why this is an include and not four private copies: this block is the exact
// shape ShaderUBOSizeConsistency.CrossShaderUBOMemberOffsetsAgree exists to
// police. Before #813 the G-Buffer stage declared offsets 8/12 as `_vdPad0` /
// `_vdPad1` while the resolve and shadow stages declared those same bytes as
// the visbuffer viewport size — two different structs on one binding, with the
// pad names the only reason the disagreement stayed invisible. The mesh path
// needed two more per-draw values; claiming the pad slots for them is what made
// the split visible. Giving every field its own offset in one shared block is
// what makes it impossible: identical text in every consumer cannot disagree.
//
// Every pass uploads all 32 bytes; a pass with no use for a field writes 0.
// Adding a field means adding it HERE and growing the C++ mirror with it.
layout(std140, binding = 49) uniform VirtualDrawInfo {
    // 0 — instance every draw/workgroup of this call belongs to.
    uint u_VirtualInstanceIndex;
    // 4 — the instance's global visible-slot base for the current phase region
    //     (what the cull wrote into each MDI command's BaseInstance).
    uint u_VirtualCommandBase;
    // 8/12 — visbuffer dimensions, read by the software-raster resolve and the
    //        shadow depth stage; the G-Buffer stages write them but never read.
    uint u_VirtualViewportWidth;
    uint u_VirtualViewportHeight;
    // 16 — the instance's VirtualDrawArgs slot for the current phase, read by
    //      the TASK stage as its launch count (#813). MDI stages never read it.
    uint u_VirtualArgsSlot;
    // 20 — the instance's ClusterCount: the task stage's launch clamp, mirroring
    //      the MDI arm's maxDrawCount bound against a corrupt GPU-written count.
    uint u_VirtualMaxClusters;
    // 24/28 — std140 rounds the block to a 16-byte multiple; the pads are
    //         explicit so the C++ mirror can be a plain struct with the same size.
    uint u_VirtualDrawInfoPad0;
    uint u_VirtualDrawInfoPad1;
};

#endif // VIRTUAL_DRAW_INFO_GLSL
