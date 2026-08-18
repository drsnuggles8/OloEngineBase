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

// --- Geometry of a LOCAL light's virtual texture (issue #703) -----------------
//
// A point light owns SIX consecutive layers (its cube faces, +X,-X,+Y,-Y,+Z,-Z —
// the same order as the atlas path's atlasCubeFace) and a spot light owns one.
// Layers come from ONE flat pool rather than separate point/spot budgets, so a
// scene spends its layers on whatever mix of lights it actually has: 42 point
// lights, or 256 spots, or anything between.
//
// Unlike a clip level, a layer is MIPPED. Mip m addresses the same face at half
// the resolution of mip m-1, and the mip a fragment uses is chosen per texel by
// vsmLocalMipForDistances below — which is what makes a light's shadow detail
// follow its screen footprint instead of an authored priority rank. A distant
// light resolves to mip 5 and costs ONE page for a whole face.
#define VSM_LOCAL_VIRTUAL_RESOLUTION    2048
#define VSM_LOCAL_PAGE_TABLE_RESOLUTION (VSM_LOCAL_VIRTUAL_RESOLUTION / VSM_PAGE_SIZE) // 32
// Spelled out for the same reason VSM_PAGE_TABLE_RES_LOG2 is: 2048/64 = 32 is a
// coincidence of the two constants, not a relationship code may lean on.
#define VSM_LOCAL_PAGE_TABLE_RES_LOG2   5
#define VSM_LOCAL_PAGE_TABLE_MASK       (VSM_LOCAL_PAGE_TABLE_RESOLUTION - 1)
// 32 -> 16 -> 8 -> 4 -> 2 -> 1.
#define VSM_LOCAL_MIP_COUNT             6
// Sum over mips of (32 >> m)²: 1024+256+64+16+4+1.
#define VSM_LOCAL_PAGES_PER_LAYER       1365
// 6 * 32 point-light faces + 64 spots, as a FLAT pool (see above).
#define VSM_MAX_LOCAL_LAYERS            256
#define VSM_TOTAL_LOCAL_PAGES           (VSM_LOCAL_PAGES_PER_LAYER * VSM_MAX_LOCAL_LAYERS)
// The page table holds the directional pages first, then the local ones, in ONE
// buffer sharing ONE physical pool, ONE allocator and ONE eviction policy. That
// sharing is the point of the issue: a frame spends its pages where the pixels
// are, instead of on a fixed per-light tile.
#define VSM_TOTAL_PAGE_TABLE_ENTRIES    (VSM_TOTAL_VIRTUAL_PAGES + VSM_TOTAL_LOCAL_PAGES)

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

// The same pyramid, per LOCAL LAYER, appended to the same buffer. Its mip 0 is
// NOT a straight copy of a dirty bit the way the directional one is: a local
// layer's pages live at six page-table mips at once, so mip 0 here is a GATHER —
// cell c is dirty if ANY page covering c is dirty, at any page-table mip. That
// flattening is what lets one HPB serve a mipped page table, and it is why the
// cull can reject a caster against a layer with a single lookup.
#define VSM_LOCAL_HPB_MIP_COUNT     VSM_LOCAL_MIP_COUNT
#define VSM_LOCAL_HPB_ENTRIES_PER_LAYER VSM_LOCAL_PAGES_PER_LAYER
#define VSM_LOCAL_HPB_TOTAL_ENTRIES (VSM_LOCAL_HPB_ENTRIES_PER_LAYER * VSM_MAX_LOCAL_LAYERS)
#define VSM_TOTAL_HPB_ENTRIES       (VSM_HPB_TOTAL_ENTRIES + VSM_LOCAL_HPB_TOTAL_ENTRIES)

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

// An allocation request's `.w` word. Directional requests leave it zero (which
// is what the pre-#703 encoding wrote), so the allocator's fork is "is bit 31
// set" rather than a value the old marker could ever have produced.
//   BIT 31 -> the request names a LOCAL layer, and .x is a layer, not a level
//   BIT 0..2 -> the local page-table mip
#define VSM_REQUEST_LOCAL_BIT       (1u << 31)
#define VSM_REQUEST_MIP_MASK        0x7u

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
// BIT 29 -> LOCAL     (the owner is a local-light layer, not a clip level)
//
// DIRECTIONAL owner (bit 29 clear):
//   BIT  0..7  -> owner virtual page X
//   BIT  8..15 -> owner virtual page Y
//   BIT 16..19 -> owner clip level
//
// LOCAL owner (bit 29 set):
//   BIT  0..5  -> owner page X within the layer's mip     (0..31)
//   BIT  6..11 -> owner page Y within the layer's mip     (0..31)
//   BIT 12..14 -> owner page-table mip                    (0..5)
//   BIT 15..22 -> owner layer                             (0..255)
//
// STILL 32 bits, and that is the whole reason issue #703 could reuse this table
// rather than widen it. Timberdoodle packs the same information into an R64_UINT
// image and therefore needs int64 IMAGE atomics, which are an extension in GL
// 4.6; a local owner fits in 23 bits here because the layer pool is 256 deep and
// a layer's finest page grid is 32², so the whole allocator stays on core SSBO
// atomics with both light classes sharing one physical pool.

#define VSM_META_ALLOCATED_BIT      (1u << 31)
#define VSM_META_VISITED_BIT        (1u << 30)
#define VSM_META_LOCAL_BIT          (1u << 29)

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
bool vsmMetaIsLocal(uint entry)     { return (entry & VSM_META_LOCAL_BIT) != 0u; }

uint vsmPackMetaLocalOwner(ivec2 page, int mip, int layer)
{
    return VSM_META_LOCAL_BIT | (uint(layer & 0xFF) << 15) | (uint(mip & 0x7) << 12) |
           (uint(page.y & 0x3F) << 6) | uint(page.x & 0x3F);
}

ivec2 vsmUnpackMetaLocalPage(uint entry) { return ivec2(int(entry & 0x3Fu), int((entry >> 6) & 0x3Fu)); }
int vsmUnpackMetaLocalMip(uint entry)    { return int((entry >> 12) & 0x7u); }
int vsmUnpackMetaLocalLayer(uint entry)  { return int((entry >> 15) & 0xFFu); }

// --- Flat indexing ------------------------------------------------------------

int vsmPageIndex(int clipLevel, ivec2 wrappedPage)
{
    return clipLevel * VSM_PAGES_PER_CLIP_LEVEL + wrappedPage.y * VSM_PAGE_TABLE_RESOLUTION + wrappedPage.x;
}

// Offset of a local mip inside one layer's 1365-entry pyramid. Same loop-free
// table treatment as vsmHPBMipOffset, and used for BOTH the page table and the
// local HPB — the two pyramids are the same shape on purpose, so the cull can
// walk one with the other's coordinates.
int vsmLocalMipOffset(int mip)
{
    // 0, 1024, 1280, 1344, 1360, 1364
    const int offsets[VSM_LOCAL_MIP_COUNT] = int[VSM_LOCAL_MIP_COUNT](0, 1024, 1280, 1344, 1360, 1364);
    return offsets[mip];
}

int vsmLocalPageIndex(int layer, int mip, ivec2 page)
{
    int mipRes = VSM_LOCAL_PAGE_TABLE_RESOLUTION >> mip;
    return VSM_TOTAL_VIRTUAL_PAGES + layer * VSM_LOCAL_PAGES_PER_LAYER + vsmLocalMipOffset(mip) +
           page.y * mipRes + page.x;
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

int vsmLocalHPBIndex(int layer, int mip, ivec2 coord)
{
    int mipRes = VSM_LOCAL_PAGE_TABLE_RESOLUTION >> mip;
    return VSM_HPB_TOTAL_ENTRIES + layer * VSM_LOCAL_HPB_ENTRIES_PER_LAYER + vsmLocalMipOffset(mip) +
           coord.y * mipRes + coord.x;
}

// The ONE place a meta entry is turned back into a page-table index. Both the
// eviction path (VSM_AllocatePages) and the dirty clear (VSM_ClearDirtyPages)
// go through it, so the directional/local fork cannot be applied in one and
// forgotten in the other — which would release, or clear, an unrelated page.
int vsmMetaOwnerPageIndex(uint meta)
{
    if (vsmMetaIsLocal(meta))
    {
        return vsmLocalPageIndex(vsmUnpackMetaLocalLayer(meta), vsmUnpackMetaLocalMip(meta),
                                 vsmUnpackMetaLocalPage(meta));
    }
    return vsmPageIndex(vsmUnpackMetaOwnerLevel(meta), vsmUnpackMetaOwnerPage(meta));
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

// --- Local-light mip selection ------------------------------------------------
//
// The local half of the same contract, and it carries the same lethal property:
// the page MARKER and the SAMPLER must pick the same mip, or the sample lands on
// a page nobody asked to have backed and the surface reads fully LIT. Both call
// this; neither reimplements it. (agent-rules/virtual-shadow-map-page-cache.md §1
// is about exactly this failure for clip levels — it applies verbatim here.)
//
// The derivation, because the formula looks like it came from nowhere:
//
//   * A local face is a 90° perspective, so at distance d from the light one
//     mip-m texel covers 2*d / (LOCAL_RES >> m) world units.
//   * The DIRECTIONAL heuristic already fixes what "one screen pixel" means to
//     this system: it picks the clip level whose extent is about the distance to
//     the camera, which makes a directional texel 2*distToCamera / VIRTUAL_RES.
//     Reusing that number rather than inventing a second one is what keeps the
//     two techniques' shadow sharpness comparable in the same frame.
//   * Equating the two and solving for m:
//         m = log2( LOCAL_RES * distToCamera / (VIRTUAL_RES * distToLight) )
//
// So it needs no screen resolution and no field of view — which matters, because
// the marker (a compute pass) and the sampler (a fragment pass) would otherwise
// have to be handed the same viewport, and a mismatch there is invisible.
//
// `bias` > 1 pushes toward coarser (cheaper, blurrier) mips, < 1 toward finer —
// the SAME direction as vsmClipLevelForDistance's bias, which is why it
// MULTIPLIES the numerator. Dividing by it reads just as naturally and inverts
// the knob: the slider then sharpens where it says it blurs, at a cost nobody
// would look for. `VirtualShadowMapLocal.LocalMipIsMonotonicInBothDistances`
// caught exactly that, and it is the only reason this comment is worth writing.
//
// ceil, so the rounding is toward the CHEAPER side, matching the clip-level twin.
int vsmLocalMipForDistances(float distanceToCamera, float distanceToLight, float bias)
{
    float ratio = (float(VSM_LOCAL_VIRTUAL_RESOLUTION) * max(distanceToCamera, 1e-4) * max(bias, 1e-4)) /
                  (float(VSM_VIRTUAL_RESOLUTION) * max(distanceToLight, 1e-4));
    return clamp(int(ceil(log2(max(ratio, 1e-4)))), 0, VSM_LOCAL_MIP_COUNT - 1);
}

// World size of one texel of a local face at `distanceToLight`, at `mip`. Used
// for the sampler's normal offset, which has to follow the mip for the same
// reason the directional one follows the clip level: a mip-5 texel is 32x a
// mip-0 one, and a constant offset is either useless or a peter-pan.
float vsmLocalTexelWorldSize(float distanceToLight, int mip)
{
    return (2.0 * distanceToLight) / float(VSM_LOCAL_VIRTUAL_RESOLUTION >> mip);
}

// Dominant-axis cube face, in the layer order a point light's six layers are
// built in: +X,-X,+Y,-Y,+Z,-Z. Identical to PBRCommon's atlasCubeFace — spelled
// here too because the VSM kernels must not depend on the atlas path's include,
// and pinned against it by VirtualShadowMapLocalTest.
int vsmCubeFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z)
        return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
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
