#ifndef OLO_VRCS_GLSL
#define OLO_VRCS_GLSL

// =============================================================================
// VRCS.glsl — Variable Rate Compute Shading, consumer side (issue #683)
//
// The classification pass (compute/VRCSClassify.comp) rates every 8x8 screen
// tile into a SHADING FOOTPRINT — the side length, in pixels, of the square
// block that one invocation is allowed to compute and broadcast:
//
//     1  full rate (one invocation per pixel; the unchanged path)
//     2  a 2x2 quad computes once
//     4  a 4x4 quad computes once
//
// A consuming compute pass keeps its per-pixel dispatch. Per invocation it
// resolves which footprint it owns, computes once, and stores the result across
// that whole footprint. No shared memory and NO barrier() is involved: every
// invocation's decision is independent, so this composes with the early
// `return`s every compute kernel here already has.
//   (A shared-memory broadcast would need a barrier(), and a barrier only some
//   invocations reach is undefined behaviour — see
//   docs/agent-rules/gpu-scan-compaction.md §1.)
//
// -----------------------------------------------------------------------------
// MASKING LANES IS NOT RETIRING WAVES, AND ONLY THE SECOND ONE IS FASTER.
//
// The obvious mapping is "every invocation keeps its own pixel and returns
// unless OloVRCSIsLeader". It is correct, it is one line, and it is SLOWER than
// doing nothing: leaders sit at even x and even y, so a 32-lane wave over a
// 16x16 group still contains eight of them and still pays the full latency of
// whatever the leaders run. The masked lanes buy nothing, and the footprint
// fetch, the branch and the 4x stores are pure added cost. Measured on GTAO:
// 0.947 ms -> 1.354 ms, a 43% REGRESSION.
//
// A consumer that wants an actual saving must COMPACT its work items so the
// spare lanes are contiguous and whole waves fall off the end. GTAO.comp's
// ResolveVRCSWorkItem is the worked example: it hands its 256 invocations the
// concatenated leader lists of the four 8x8 tiles its group covers, so with all
// four at 2x2 only 64 work items exist and six of the eight waves retire on one
// comparison.
//
// OloVRCSIsLeader below is still the definition of the partition — it is what
// VRCSContractTest checks, and it is the right predicate for a consumer whose
// invocations must stay pinned to their own pixel for other reasons. Just do
// not expect it to make anything faster on its own.
//
// THE LEADER IS ALWAYS DISPATCHED AND ALWAYS IN BOUNDS. Footprints are aligned
// to the pixel-space origin (1, 2 and 4 all divide the 8-pixel tile), so the
// leader sits at the minimum corner of its block: if any pixel of a footprint
// is inside the viewport, the leader is too. A consumer must still bounds-check
// each texel it broadcasts to, because a footprint may hang off the right or
// bottom edge.
// =============================================================================

// Classification tile edge, in pixels. Mirrored by
// ShadingRateClassifier::kTileSize on the C++ side; VRCSClassifierTest pins the
// two together.
#define OLO_VRCS_TILE_SIZE 8

// Footprint encoding stored in the R8UI rate image, and its decode. 0 is the
// cleared / never-classified value and decodes to full rate, so a consumer that
// reads an unwritten tile degrades to correct-and-slow rather than to a coarse
// block of the wrong colour.
#define OLO_VRCS_RATE_1X1 1u
#define OLO_VRCS_RATE_2X2 2u
#define OLO_VRCS_RATE_4X4 4u

ivec2 OloVRCSTileCoord(ivec2 pixCoord)
{
    return pixCoord / OLO_VRCS_TILE_SIZE;
}

uint OloVRCSDecodeFootprint(uint stored)
{
    // Anything the classifier did not write, or wrote out of range, means full
    // rate. Never trust the stored value to be one of {1,2,4}: a coarse
    // footprint derived from garbage would broadcast one pixel's lighting over
    // a block, which is a visible artefact, while a wrongly-full-rate tile only
    // costs time.
    if (stored == OLO_VRCS_RATE_2X2)
        return OLO_VRCS_RATE_2X2;
    if (stored == OLO_VRCS_RATE_4X4)
        return OLO_VRCS_RATE_4X4;
    return OLO_VRCS_RATE_1X1;
}

// True for the one invocation in each footprint that does the expensive work.
// At full rate every invocation is its own leader, so a consumer needs no
// separate "VRCS off" code path — the enable gate only decides whether the
// footprint is read at all.
bool OloVRCSIsLeader(ivec2 pixCoord, uint footprint)
{
    ivec2 inBlock = pixCoord % ivec2(int(footprint));
    return inBlock.x == 0 && inBlock.y == 0;
}

// Debug heatmap value for a footprint: 1.0 full rate, 0.5 at 2x2, 0.25 at 4x4.
// Derived from the footprint the shader ACTUALLY used, never re-derived from
// the rate image, so the overlay cannot disagree with what ran.
float OloVRCSDebugShade(uint footprint)
{
    return 1.0 / float(footprint);
}

#endif // OLO_VRCS_GLSL
