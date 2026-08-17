#ifndef OLO_VIRTUAL_SHADOW_COMMON_GLSL
#define OLO_VIRTUAL_SHADOW_COMMON_GLSL

// =============================================================================
// VirtualShadowCommon.glsl — the Virtual Shadow Map contract (issue #702)
//
// ONE file owns the page-state encoding, the toroidal addressing, the clip-level
// heuristic and the sampling entry point. Every VSM kernel, the depth raster and
// every lit shader include it, so the producer and the consumer of a page cannot
// disagree about what a page entry means.
//
// C++ twin: OloEngine/src/OloEngine/Renderer/Shadow/VirtualShadowMap.h. The
// constants below are mirrored there and pinned by VirtualShadowMapTest —
// changing one without the other silently reinterprets every page entry.
//
// Adapted from Timberdoodle's virtual_shadow_maps (Apache-2.0,
// https://github.com/Sunset-Flock/Timberdoodle) — specifically the page-state
// bit layout, the wrapped-coordinate scheme and the clip-level heuristic in
// src/shader_lib/vsm_util.glsl. Reworked for GL 4.6 core: the meta table is 32-
// bit (no int64 image atomics), the page tables are SSBOs rather than R32UI
// images, and the physical pool is written by hardware raster + imageAtomicMin
// instead of a software rasterizer.
// =============================================================================

// --- Geometry of the virtual texture -----------------------------------------

// One clip level is VSM_VIRTUAL_RESOLUTION² virtual texels, addressed through a
// VSM_PAGE_TABLE_RESOLUTION² grid of VSM_PAGE_SIZE² pages.
#define VSM_VIRTUAL_RESOLUTION      4096
#define VSM_PAGE_SIZE               64
#define VSM_PAGE_SIZE_LOG2          6
#define VSM_PAGE_TABLE_RESOLUTION   (VSM_VIRTUAL_RESOLUTION / VSM_PAGE_SIZE)
// Spelled out rather than derived from VSM_PAGE_SIZE_LOG2: the two are equal only
// because 4096/64 happens to be 64, and code that shifts by "the table's log2"
// must not silently follow the page size if either constant is ever retuned.
#define VSM_PAGE_TABLE_RES_LOG2     6
#define VSM_PAGE_TABLE_MASK         (VSM_PAGE_TABLE_RESOLUTION - 1)
#define VSM_PAGES_PER_CLIP_LEVEL    (VSM_PAGE_TABLE_RESOLUTION * VSM_PAGE_TABLE_RESOLUTION)

// Concentric clip levels, each covering 2x the world extent of the previous.
#define VSM_CLIP_LEVELS             16
#define VSM_TOTAL_VIRTUAL_PAGES     (VSM_PAGES_PER_CLIP_LEVEL * VSM_CLIP_LEVELS)

// Allocation-request ring capacity. A frame cannot usefully request more pages
// than the physical pool can hold, so this is generous by a wide margin;
// overflow is counted (Statistics::PagesFailed) rather than silently wrapping.
#define VSM_MAX_REQUESTS            16384

// Cull-stage buffer capacities, mirrored from VSM::kMaxBatches /
// VSM::kMaxDrawInstances (pinned by VirtualShadowMapTest's mirror test). The
// cull validates every CPU-supplied batch record against these before touching
// b_DrawCommands / b_DrawInstances — a malformed record must count as an
// overflow, never scribble.
#define VSM_MAX_BATCHES             1024
#define VSM_MAX_DRAW_INSTANCES      131072

// Hierarchical Page Buffer: a HiZ-shaped pyramid over the DIRTY flag (not
// depth), one pyramid per clip level. 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1.
#define VSM_HPB_MIP_COUNT           7
// Sum over mips of (res >> mip)² for res = 64: 4096+1024+256+64+16+4+1.
#define VSM_HPB_ENTRIES_PER_LEVEL   5461
#define VSM_HPB_TOTAL_ENTRIES       (VSM_HPB_ENTRIES_PER_LEVEL * VSM_CLIP_LEVELS)

// --- Page-table entry encoding (32 bits) --------------------------------------
//
// BIT 31 -> ALLOCATED          — backed by a physical page
// BIT 30 -> REQUESTS_ALLOCATION— marked this frame, not yet backed
// BIT 29 -> DIRTY              — must be (re)drawn this frame
// BIT 28 -> VISITED            — referenced this frame (keeps it off the LRU list)
// BIT 27 -> ALLOCATION_FAILED  — requested but the physical pool was exhausted
// BIT  0..7  -> physical page X
// BIT  8..15 -> physical page Y

#define VSM_PAGE_ALLOCATED_BIT      (1u << 31)
#define VSM_PAGE_REQUESTS_ALLOC_BIT (1u << 30)
#define VSM_PAGE_DIRTY_BIT          (1u << 29)
#define VSM_PAGE_VISITED_BIT        (1u << 28)
#define VSM_PAGE_ALLOC_FAILED_BIT   (1u << 27)
#define VSM_PAGE_PHYSICAL_COORD_MASK 0x0000FFFFu

bool vsmPageIsAllocated(uint entry)      { return (entry & VSM_PAGE_ALLOCATED_BIT) != 0u; }
bool vsmPageRequestsAlloc(uint entry)    { return (entry & VSM_PAGE_REQUESTS_ALLOC_BIT) != 0u; }
bool vsmPageIsDirty(uint entry)          { return (entry & VSM_PAGE_DIRTY_BIT) != 0u; }
bool vsmPageIsVisited(uint entry)        { return (entry & VSM_PAGE_VISITED_BIT) != 0u; }
bool vsmPageAllocFailed(uint entry)      { return (entry & VSM_PAGE_ALLOC_FAILED_BIT) != 0u; }

ivec2 vsmUnpackPhysicalPage(uint entry)
{
    return ivec2(int(entry & 0xFFu), int((entry >> 8) & 0xFFu));
}

uint vsmPackPhysicalPage(ivec2 coords)
{
    return (uint(coords.y & 0xFF) << 8) | uint(coords.x & 0xFF);
}

// --- Meta (physical -> virtual owner) entry encoding (32 bits) -----------------
//
// BIT 31 -> ALLOCATED (this physical page backs a virtual page)
// BIT 30 -> VISITED   (its owner was referenced this frame — not evictable)
// BIT  0..7  -> owner virtual page X
// BIT  8..15 -> owner virtual page Y
// BIT 16..19 -> owner clip level
//
// 32 bits is deliberate. Timberdoodle packs the same information into an
// R64_UINT image because it also serves point/spot lights (12 bits of array
// layer); the directional-only table here fits in a plain uint, which keeps the
// whole thing on core GL 4.6 SSBO atomics instead of the int64 image atomics
// that would otherwise be required.

#define VSM_META_ALLOCATED_BIT      (1u << 31)
#define VSM_META_VISITED_BIT        (1u << 30)

uint vsmPackMetaOwner(ivec2 virtualPage, int clipLevel)
{
    return (uint(clipLevel & 0xF) << 16) | (uint(virtualPage.y & 0xFF) << 8) | uint(virtualPage.x & 0xFF);
}

ivec2 vsmUnpackMetaOwnerPage(uint entry)
{
    return ivec2(int(entry & 0xFFu), int((entry >> 8) & 0xFFu));
}

int vsmUnpackMetaOwnerLevel(uint entry)
{
    return int((entry >> 16) & 0xFu);
}

bool vsmMetaIsAllocated(uint entry) { return (entry & VSM_META_ALLOCATED_BIT) != 0u; }
bool vsmMetaIsVisited(uint entry)   { return (entry & VSM_META_VISITED_BIT) != 0u; }

// --- Flat indexing ------------------------------------------------------------

int vsmPageIndex(int clipLevel, ivec2 wrappedPage)
{
    return clipLevel * VSM_PAGES_PER_CLIP_LEVEL + wrappedPage.y * VSM_PAGE_TABLE_RESOLUTION + wrappedPage.x;
}

// Byte-free offset of an HPB mip inside one clip level's pyramid. Closed form of
// sum_{i<mip} (RES >> i)², which for RES = 64 is (4^7 - 4^(7-mip)) / 3 scaled —
// spelled as a loop-free table because a mip count of 7 makes the table cheaper
// (and provably right) than the arithmetic.
int vsmHPBMipOffset(int mip)
{
    // 0, 4096, 5120, 5376, 5440, 5456, 5460
    const int offsets[VSM_HPB_MIP_COUNT] = int[VSM_HPB_MIP_COUNT](0, 4096, 5120, 5376, 5440, 5456, 5460);
    return offsets[mip];
}

int vsmHPBIndex(int clipLevel, int mip, ivec2 coord)
{
    int mipRes = VSM_PAGE_TABLE_RESOLUTION >> mip;
    return clipLevel * VSM_HPB_ENTRIES_PER_LEVEL + vsmHPBMipOffset(mip) + coord.y * mipRes + coord.x;
}

// --- Toroidal (wraparound) addressing -----------------------------------------
//
// A clip level's frustum origin is snapped to whole pages, so its world position
// in page units is an integer. `PageOffset` carries that origin mod the page-
// table resolution, which makes the wrapped slot of a world page depend only on
// the WORLD page index — so a page that stays put keeps its slot (and its cached
// texels) while the frustum scrolls underneath it, and a page entering the
// frustum lands in the slot of the page that just left.

ivec2 vsmWrapPage(ivec2 virtualPage, ivec2 pageOffset)
{
    return (virtualPage + pageOffset) & ivec2(VSM_PAGE_TABLE_MASK);
}

// --- Clip-level selection -----------------------------------------------------
//
// Distance-based, matching Timberdoodle's shipped heuristic: clip level L covers
// a world half-extent of clip0HalfExtent * 2^L, so the level whose texels are
// about the size of a screen pixel at distance d is ceil(log2(d / base)).
//
// `bias` (VSMGlobals.ClipSelectionBias) scales the distance before the log, so
// >1 pushes work to coarser (cheaper) levels and <1 to finer (sharper) ones.
//
// EVERY consumer must call this — the page-marking kernel and the lit pass above
// all. Marking a page at one level and sampling at another is the archetypal VSM
// bug: the sample lands on an unallocated page and the surface reads unshadowed.
int vsmClipLevelForDistance(float distanceToCamera, float clip0HalfExtent, float bias)
{
    float scaled = max(distanceToCamera * bias, 1e-4) / max(clip0HalfExtent, 1e-4);
    return clamp(int(ceil(log2(max(scaled, 1e-4)))), 0, VSM_CLIP_LEVELS - 1);
}

// --- Depth encoding -----------------------------------------------------------
//
// The physical pool is R32UI so the raster can resolve visibility with
// imageAtomicMin. IEEE-754 non-negative floats order identically to their bit
// patterns as unsigned ints, and the clip-level projections are orthographic
// with a [0,1] depth range, so the raw bits ARE the comparison key. Nothing here
// is a reinterpret-cast trick that only works for the common case: a negative
// depth cannot reach this path because the raster clips z < 0.
#define VSM_DEPTH_FAR_BITS 0xFFFFFFFFu

float vsmDecodeDepth(uint bits)
{
    return (bits == VSM_DEPTH_FAR_BITS) ? 1.0 : uintBitsToFloat(bits);
}

#endif // OLO_VIRTUAL_SHADOW_COMMON_GLSL
