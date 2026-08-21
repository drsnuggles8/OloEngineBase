// =============================================================================
// ThreadGroupSwizzle.glsl — remap gl_WorkGroupID for L2 locality (issue #720)
//
// A GPU dispatches a compute pass's workgroups in roughly raster order:
// increasing X first, then Y. For a fullscreen pass whose inputs/outputs are
// spatially addressed 2D (or 2D-tiled-3D) resources, the workgroups resident
// on the GPU AT THE SAME TIME therefore span the FULL WIDTH of the dispatch —
// their texture/image fetches land on widely separated cache lines and thrash
// L2. Remapping the ID so a contiguous run of dispatch order covers a compact
// screen-space TILE instead of a full row keeps concurrently-scheduled
// workgroups' memory traffic local.
//
// PURELY A COORDINATE REMAP. OloSwizzleWorkGroupID2D is a bijection over
// [0, numWorkGroups.x) x [0, numWorkGroups.y) for any tileWidth >= 1 (proven
// exhaustively in ThreadGroupSwizzleTest.cpp, which mirrors this exact
// algorithm in C++). Every workgroup in the dispatch still gets remapped to
// exactly one (unique) coordinate and every coordinate is still visited by
// exactly one workgroup — only WHEN a given output element is computed
// relative to its neighbours changes, never WHAT is computed.
//
// Technique: NVIDIA, "Optimizing Compute Shaders for L2 Locality using
// Thread-Group ID Swizzling" (developer blog). This is an independent
// derivation of the published idea, not a copy of any reference source.
//
// MEASURED, NOT ASSUMED (issue #720). Adopted in GTAO_Denoise.comp,
// FroxelFogScatter.comp, FroxelFogIntegrate.comp, LightCulling.comp and
// AutoExposureHistogram.comp, where per-pass GPU-ms (GPUPassTimerPool,
// median of 40-50 samples) showed no measurable regression at either 1080p
// or 4K on the dev GPU (RTX 4090, 72 MiB L2 — large enough that these
// passes' dispatch grids apparently never thrash it in the first place).
// Deliberately NOT adopted in GTAO.comp or HZB.comp: GTAO.comp regressed at
// every tested tile width (worst at tileWidth=4: 13.55ms -> 46.17ms; even
// tileWidth=32, nearly unswizzled, stayed ~3% slower than baseline at 4K),
// and HZB.comp showed a small but consistent regression (~6%) at the
// default tileWidth=8. Both are left computing gl_WorkGroupID directly. See
// the PR for the full per-pass numbers — this is why two of the issue's
// seven candidate shaders are absent from the adopters above.
// =============================================================================

#ifndef THREAD_GROUP_SWIZZLE_GLSL
#define THREAD_GROUP_SWIZZLE_GLSL

// Default tile width, in WORKGROUPS (not pixels/texels/voxels) — a tile spans
// this many workgroup columns and the full dispatch height. Per-pass adoption
// sites `#define OLO_SWIZZLE_TILE_WIDTH n` ahead of this include when a
// pass's measurement justifies a different value; see the per-pass comments
// at each adoption site and the PR description for the measured numbers.
#ifndef OLO_SWIZZLE_TILE_WIDTH
#define OLO_SWIZZLE_TILE_WIDTH 8
#endif

// Remaps a row-major 2D workgroup ID into tile-major dispatch order.
//   workGroupId   — gl_WorkGroupID.xy (or gl_WorkGroupID.xy of a 3D dispatch;
//                   the third axis is untouched by this function).
//   numWorkGroups — gl_NumWorkGroups.xy for this dispatch.
//   tileWidth     — tile width in workgroups (clamped to >= 1 here).
//
// The dispatch width need not be a multiple of tileWidth: the final tile
// column is simply narrower (tileColumns below), and the mapping stays a
// bijection regardless — verified for many non-multiple grid/tile
// combinations in ThreadGroupSwizzleTest.cpp.
ivec2 OloSwizzleWorkGroupID2D(ivec2 workGroupId, ivec2 numWorkGroups, int tileWidth)
{
    tileWidth = max(tileWidth, 1);

    // The workgroup's position in raster dispatch order (row-major: X varies
    // fastest), i.e. "the Nth workgroup the GPU would schedule unswizzled".
    int flatIndex = workGroupId.y * numWorkGroups.x + workGroupId.x;

    // One full-width tile covers this many workgroups. Integer division below
    // self-consistently gives every full tile exactly this many indices and
    // hands whatever remains to the last (possibly narrower) tile.
    int groupsPerTile = tileWidth * numWorkGroups.y;

    int tileIndex = flatIndex / groupsPerTile;
    int indexInTile = flatIndex - tileIndex * groupsPerTile;

    // Columns actually available in THIS tile — full tileWidth for every tile
    // except a possible narrower last one.
    int tileColumns = max(min(tileWidth, numWorkGroups.x - tileIndex * tileWidth), 1);

    int localY = indexInTile / tileColumns;
    int localX = indexInTile - localY * tileColumns;

    return ivec2(tileIndex * tileWidth + localX, localY);
}

// Convenience overload using the file-scope default tile width.
ivec2 OloSwizzleWorkGroupID2D(ivec2 workGroupId, ivec2 numWorkGroups)
{
    return OloSwizzleWorkGroupID2D(workGroupId, numWorkGroups, OLO_SWIZZLE_TILE_WIDTH);
}

// Convenience for the common 2D fullscreen-pass case: swizzles gl_WorkGroupID
// and reconstructs the full global invocation ID from it in one call, so an
// adopting shader's `main()` needs only one line instead of hand-expanding
// `swizzledGroupID * gl_WorkGroupSize.xy + gl_LocalInvocationID.xy` itself.
// Reads the compute-shader builtins directly — call only from `main()` (or a
// function `main()` calls), same as any other builtin-dependent GLSL helper.
// Not used by LightCulling.comp (one workgroup IS one output element, no
// per-invocation offset to reconstruct) or FroxelFogScatter.comp (3D, with a
// Z passthrough this 2D-only helper doesn't express).
ivec2 OloSwizzledGlobalInvocationID2D(int tileWidth)
{
    ivec2 swizzledGroupID = OloSwizzleWorkGroupID2D(ivec2(gl_WorkGroupID.xy), ivec2(gl_NumWorkGroups.xy), tileWidth);
    return swizzledGroupID * ivec2(gl_WorkGroupSize.xy) + ivec2(gl_LocalInvocationID.xy);
}

ivec2 OloSwizzledGlobalInvocationID2D()
{
    return OloSwizzledGlobalInvocationID2D(OLO_SWIZZLE_TILE_WIDTH);
}

#endif // THREAD_GROUP_SWIZZLE_GLSL
