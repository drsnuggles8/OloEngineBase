#ifndef VIRTUAL_RASTER_DISPATCH_ARGS_GLSL
#define VIRTUAL_RASTER_DISPATCH_ARGS_GLSL

// =============================================================================
// VirtualRasterDispatchArgs.glsl — the ONE spelling of the virtual-geometry
// software rasterizer's record-count -> workgroup-grid flattening (issue #1048).
//
// The raster consumes a work list whose length is written on the GPU by
// VirtualClusterCull.comp and never read back, so the grid it runs on is
// computed on the GPU too, by VirtualRasterArgs.comp, and read by
// DispatchComputeIndirect.
//
// A header rather than inline code for the same reason
// VirtualRasterCoverage.glsl is one: the rule has to agree EXACTLY across
// three places, and here a disagreement is silent — a grid that is too small
// drops clusters with no error anywhere, and an indirect dispatch has no CPU
// to notice:
//   * compute/VirtualRasterArgs.comp — writes the arguments
//   * tests/ShaderUnit_VirtualRasterArgs.comp — the L2 probe that runs THIS file
//   * OloEngine/tests/Rendering/VirtualRasterDispatchArgsMirror.h — the CPU
//     mirror, pinned against the raster's own index recovery by
//     VirtualRasterDispatchArgsTest.
//
// The INVERSE of this function lives in VirtualClusterRaster.comp's main():
//
//     swRecordIndex = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x
//
// so a grid of (gx, gy) covers record indices [0, gx*gy) exactly once. Every
// record must therefore land inside the grid — gx * gy >= count — and the
// surplus (at most gx - 1 groups) early-outs on the raster's own bound.
// =============================================================================

// The x-axis split. Two constraints meet here:
//   * a compute grid axis is capped at 65535 by GL and by every Vulkan
//     maxComputeWorkGroupCount we target, so a work list longer than that
//     cannot be a 1D dispatch at all;
//   * the surplus a 2D split wastes is at most (gx - 1) groups, so a SMALLER
//     gx wastes less at the tail.
// 4096 keeps both comfortably: it is the value the CPU-side bound used before
// this was on the GPU, so the grid shape did not change when the count did.
const uint kOloVirtualRasterMaxGroupsX = 4096u;

// Workgroup grid for `count` software-raster records, in the layout
// VirtualClusterRaster.comp flattens back to a record index.
//
// count == 0 yields (0, 0, 1): a zero-group dispatch is legal and does nothing,
// which is exactly what a frame that routed nothing to software should cost.
// The divisor is guarded rather than the whole branch, so that case costs no
// divergence.
uvec3 OloVirtualRasterDispatchArgs(uint count)
{
    uint groupsX = min(count, kOloVirtualRasterMaxGroupsX);
    uint groupsY = (count + max(groupsX, 1u) - 1u) / max(groupsX, 1u);
    return uvec3(groupsX, groupsY, 1u);
}

#endif // VIRTUAL_RASTER_DISPATCH_ARGS_GLSL
