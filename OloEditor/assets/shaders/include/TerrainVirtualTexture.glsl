// =============================================================================
// Terrain virtual texturing — the shading side (issue #715).
//
// One indirection lookup plus one cache sample per pixel replaces the per-pixel
// splat blend, so terrain shading cost stops scaling with layer count. The
// blend itself now happens once per cache tile in
// compute/TerrainVTTileBake.comp.
//
// Adaptive images: the terrain is cut into SectorsWide^2 sectors, each
// owning a variable-size square image inside the shared virtual ATLAS. All the
// address math below operates in atlas space and is unchanged from the fixed
// grid — adaptivity enters in exactly three per-sector scalars (UV origin/size
// and a derivative scale) plus a per-sector mip clamp and ready gate. The
// sector table itself rides the TerrainParams UBO, which this include
// deliberately does not declare: callers fetch the two vec4s and hand them to
// oloVTDecodeSector, so the same helpers serve the forward and deferred paths
// unchanged.
//
// C++ twin for every packing here:
// OloEngine/Terrain/VirtualTexture/TerrainVirtualTextureTypes.h. The two are
// pinned against each other by TerrainVirtualTextureTest — a mismatch would
// otherwise only show as terrain sampling the wrong part of its own cache,
// which reads as "a texture bug" rather than "a packing bug".
//
// Includers must declare nothing: the samplers and the feedback buffer are
// declared HERE and nowhere else (grep before adding a second declaration —
// see docs/agent-rules/glsl-shaders.md §5b on the include-shadowing trap).
// =============================================================================

#ifndef OLO_TERRAIN_VIRTUAL_TEXTURE_GLSL
#define OLO_TERRAIN_VIRTUAL_TEXTURE_GLSL

#include "BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_TerrainVTIndirection OLO_HEAP_TEX_2D(67) // TEX_TERRAIN_VT_INDIRECTION
#define u_TerrainVTCache OLO_HEAP_TEX_2D_ARRAY(68) // TEX_TERRAIN_VT_CACHE
#else
layout(binding = 67) uniform sampler2D u_TerrainVTIndirection;
layout(binding = 68) uniform sampler2DArray u_TerrainVTCache;
#endif

// One uint per feedback texel, written by the terrain fragment stage and read
// back a few frames later. NOT `writeonly`: a `readonly`/`writeonly` qualifier
// here would have to agree across every includer, and the buffer is genuinely
// write-only from the shader's side only by convention.
layout(std430, binding = 79) buffer TerrainVTFeedback
{
    uint b_TerrainVTFeedback[];
};

// The VT half of the TerrainParams UBO, unpacked. Passed as four vec4s rather
// than read from the block directly so this file carries no binding dependency
// and the same helpers serve the forward and deferred paths unchanged.
struct OloVTParams
{
    float PagesWide;      // ATLAS pages across at mip 0
    float PageTexels;     // unique texels per page edge
    float BorderTexels;
    float TileTexels;     // PageTexels + 2 * BorderTexels
    float CacheTexels;    // physical cache edge, in texels
    float MaxMip;         // the atlas's 1x1 level
    vec2  FeedbackDims;
    float Enabled;        // 0 = shade from the splat path instead
    float FeedbackFrame;  // which screen-hash slot writes feedback this frame
    float Downscale;      // feedback target is the viewport / this
    float DownscaleLog2;
    float SectorsWide;    // terrain cut into this^2 sectors (1 = fixed grid)
    float Trilinear;      // 1 = blend two virtual mips per sample
};

OloVTParams oloVTUnpackParams(vec4 p0, vec4 p1, vec4 p2, vec4 p3)
{
    OloVTParams p;
    p.PagesWide     = p0.x;
    p.PageTexels    = p0.y;
    p.BorderTexels  = p0.z;
    p.TileTexels    = p0.w;
    p.CacheTexels   = p1.x;
    p.MaxMip        = p1.y;
    p.FeedbackDims  = p1.zw;
    p.Enabled       = p2.x;
    p.FeedbackFrame = p2.y;
    p.Downscale     = p2.z;
    p.DownscaleLog2 = p2.w;
    p.SectorsWide   = p3.x;
    p.Trilinear     = p3.y;
    return p;
}

// One sector's image, decoded from its two TerrainParams UBO vec4s
// (u_TerrainVTSectors[2i] and [2i+1]). C++ producer:
// TerrainVirtualTexture::FillShaderSectorTable.
struct OloVTSector
{
    vec2  UVPos;      // image origin in atlas UV
    float UVSize;     // image extent in atlas UV
    float DerivScale; // texels across the image content = sizePages * pageTexels
    float MaxMip;     // the image's own 1x1 level (an atlas mip index)
    float Ready;      // 1 = coarsest page resident AND published
};

OloVTSector oloVTDecodeSector(vec4 a, vec4 b)
{
    OloVTSector s;
    s.UVPos      = a.xy;
    s.UVSize     = a.z;
    s.DerivScale = a.w;
    s.MaxMip     = b.x;
    s.Ready      = b.y;
    return s;
}

// Which sector a terrain UV lands in — the flat index into the sector table.
int oloVTSectorIndex(OloVTParams p, vec2 terrainUV)
{
    int wide = int(p.SectorsWide);
    ivec2 coord = clamp(ivec2(terrainUV * p.SectorsWide), ivec2(0), ivec2(wide - 1));
    return coord.y * wide + coord.x;
}

// Terrain UV -> atlas UV, through the sector's image rect. `scaledUV` is
// terrainUV * SectorsWide — pass the CONTINUOUS value, computed once before
// any per-sector work, so derivatives taken on it don't blow up on the fract
// at a sector boundary (only the VALUE wraps, never the derivative source).
vec2 oloVTSectorVirtualUV(OloVTSector s, vec2 scaledUV)
{
    vec2 local = scaledUV - floor(scaledUV);
    return s.UVPos + local * s.UVSize;
}

// The mip a pixel wants, in the sector image's own mip-0 texels per pixel.
// May go BELOW zero: a negative mip means the camera resolves finer than the
// image currently provides, which is the adaptive grow signal — do not clamp
// it away before the feedback write. C++ twin: VTComputeMip().
//
// The derivative is taken on scaledUV ALONE and the per-sector scale enters as
// + log2(DerivScale) afterwards. Multiplying before dFdx would fold the scale
// into the derivative source, and DerivScale is a step function across a
// boundary between different-sized images — the derivative would pick up a
// scaledUV * (scaleA - scaleB) jump term and explode the mip along every such
// sector border (a persistent coarsest-mip seam the mean-difference evidence
// bound cannot see).
float oloVTSectorMip(OloVTSector s, vec2 scaledUV)
{
    vec2 dx = dFdx(scaledUV);
    vec2 dy = dFdy(scaledUV);
    float lenSq = max(dot(dx, dx), dot(dy, dy));
    return (lenSq > 0.0) ? 0.5 * log2(lenSq) + log2(s.DerivScale) : 0.0;
}

// Pack one feedback word. C++ twin: VTPackFeedback(). The mip field stores
// wantedMip + 1, clamped to [0, 15]: value 0 means "wanted finer than mip 0
// exists" — the grow signal the analyzer decodes as -1.
uint oloVTPackFeedback(uvec2 mip0Page, int biasedMip)
{
    return 0x80000000u | ((uint(biasedMip) & 0xFu) << 24u) | ((mip0Page.y & 0xFFFu) << 12u) | (mip0Page.x & 0xFFFu);
}

// Record what this pixel wanted, for the readback loop.
//
// Only ONE pixel of each Downscale x Downscale screen block writes per frame,
// and which one cycles with FeedbackFrame — the Call of Duty scheme. That keeps
// the feedback target small (a full-resolution one would be a 4 MB readback)
// while still covering every screen pixel over Downscale^2 frames, which is
// what makes the request list converge rather than lock onto a fixed lattice.
void oloVTWriteFeedback(OloVTParams p, vec2 screenPos, vec2 virtualUV, float mip, float sectorMaxMip)
{
    uint mask = uint(p.Downscale) - 1u;
    uint shift = uint(p.DownscaleLog2);
    uvec2 pixel = uvec2(screenPos);
    uint hash = (pixel.x & mask) + ((pixel.y & mask) << shift);
    if (hash != uint(p.FeedbackFrame))
        return;

    uvec2 coord = pixel >> shift;
    uvec2 dims = uvec2(p.FeedbackDims);
    if (coord.x >= dims.x || coord.y >= dims.y)
        return;

    int biasedMip = int(clamp(floor(mip), -1.0, sectorMaxMip)) + 1;
    vec2 clampedUV = clamp(virtualUV, vec2(0.0), vec2(0.9999999));
    uvec2 mip0Page = uvec2(clampedUV * p.PagesWide);

    b_TerrainVTFeedback[coord.y * dims.x + coord.x] = oloVTPackFeedback(mip0Page, biasedMip);
}

// One indirection texel, unpacked. C++ twin: VTUnpackIndirection().
struct OloVTIndirection
{
    vec2  Tile;   // physical tile coordinate
    float Mip;    // the mip of the page ACTUALLY resident there
};

// Resolve a virtual UV at a requested mip, clamped to the owning sector's
// coarsest level — atlas texels above it belong to OTHER images, so reading
// one would hand back a neighbouring sector's tile.
//
// texelFetch, not texture(): this is a point lookup of an exact texel at an
// exact level, and a filtered read would blend two different pages' physical
// addresses into an address that points at neither. It also means the lookup
// does not depend on the indirection texture's sampler state at all.
OloVTIndirection oloVTLookup(OloVTParams p, vec2 virtualUV, int mip, int sectorMaxMip)
{
    int clampedMip = clamp(mip, 0, min(sectorMaxMip, int(p.MaxMip)));
    int sizeAtMip = int(p.PagesWide) >> clampedMip;
    ivec2 coord = clamp(ivec2(virtualUV * float(sizeAtMip)), ivec2(0), ivec2(sizeAtMip - 1));

    vec4 raw = texelFetch(u_TerrainVTIndirection, coord, clampedMip);

    OloVTIndirection o;
    o.Tile = floor(raw.xy * 255.0 + 0.5);
    o.Mip  = floor(raw.z * 255.0 + 0.5);
    return o;
}

// Virtual UV -> physical cache UV. C++ twin: VTVirtualToPhysicalUV().
//
// The page-local coordinate is evaluated at the RESIDENT page's mip, not the
// mip that was asked for — that single substitution IS the coarse-mip fallback:
// a lookup that wanted mip 2 but found a mip 5 page reads the correct quarter-
// of-a-quarter of that coarser page rather than garbage.
vec2 oloVTVirtualToPhysicalUV(OloVTParams p, vec2 virtualUV, OloVTIndirection ind)
{
    float pagesAtMip = p.PagesWide / exp2(ind.Mip);
    vec2 scaled = virtualUV * pagesAtMip;
    vec2 local = scaled - floor(scaled);

    vec2 tileOrigin = ind.Tile * p.TileTexels;
    vec2 inTile = vec2(p.BorderTexels) + local * p.PageTexels;
    return (tileOrigin + inTile) / p.CacheTexels;
}

// @brief The whole surfacing lookup. One indirection fetch and two cache
// samples — doubled when Trilinear is on, so a page-mip transition is a
// cross-fade rather than a density step (the request list already carries
// both levels: the analyzer asks for each page's parent).
//
// `outNormal` is TANGENT space, matching what the splat path's per-layer normal
// blend produces, so the caller's existing TBN transform applies unchanged.
void oloVTSampleSurface(OloVTParams p, OloVTSector s, vec2 virtualUV, float mip,
                        out vec3 outAlbedo, out float outAO,
                        out vec3 outNormal, out float outRoughness, out float outMetallic)
{
    int sectorMaxMip = int(s.MaxMip);
    int m0 = int(clamp(floor(mip), 0.0, s.MaxMip));

    OloVTIndirection ind0 = oloVTLookup(p, virtualUV, m0, sectorMaxMip);
    vec2 physUV0 = oloVTVirtualToPhysicalUV(p, virtualUV, ind0);
    vec4 c0 = texture(u_TerrainVTCache, vec3(physUV0, 0.0));
    vec4 c1 = texture(u_TerrainVTCache, vec3(physUV0, 1.0));

    if (p.Trilinear > 0.5)
    {
        int m1 = min(m0 + 1, sectorMaxMip);
        // frac computed BEFORE the fetches: a negative mip clamps to 0 (the
        // finest page cannot cross-fade toward anything finer), and that is
        // the steady state for the nearest pixels once an image has hit its
        // size ceiling — paying three texture fetches for a mix weight of
        // exactly zero there would be permanent waste on the highest-cost
        // fragments. The branch is screen-coherent.
        float frac = clamp(mip - float(m0), 0.0, 1.0);
        if (m1 > m0 && frac > 0.0)
        {
            OloVTIndirection ind1 = oloVTLookup(p, virtualUV, m1, sectorMaxMip);
            vec2 physUV1 = oloVTVirtualToPhysicalUV(p, virtualUV, ind1);
            c0 = mix(c0, texture(u_TerrainVTCache, vec3(physUV1, 0.0)), frac);
            c1 = mix(c1, texture(u_TerrainVTCache, vec3(physUV1, 1.0)), frac);
        }
    }

    outAlbedo = c0.rgb;
    outAO = c0.a;
    outRoughness = c1.b;
    outMetallic = c1.a;

    // z is reconstructed rather than stored: the blended tangent-space normal
    // is always in the upper hemisphere, so the sign is known and the byte is
    // better spent on roughness.
    vec2 nxy = c1.rg * 2.0 - 1.0;
    outNormal = normalize(vec3(nxy, sqrt(max(1.0 - dot(nxy, nxy), 0.0))));
}

// @brief The whole per-pixel adaptive resolve, shared by the forward and
// deferred paths — sector selection, mip, feedback write, and the ready-gated
// sample. ONE definition on purpose: a fix that landed in only one of the two
// fragment shaders would make them shade the same terrain differently, which
// is exactly the drift this include exists to prevent. The caller passes the
// sector's two UBO vec4s so this file stays binding-free.
//
// Returns false when the sector is not ready — the caller then runs its splat
// path for this pixel. Feedback is written either way; that is what converges
// an unready sector.
//
// Call ONLY under a uniform condition (Enabled is a UBO value): the mip
// helper takes screen-space derivatives.
bool oloVTResolveSurface(OloVTParams p, vec4 sectorA, vec4 sectorB, vec2 terrainUV, vec2 fragCoord,
                         out vec3 outAlbedo, out float outAO,
                         out vec3 outNormal, out float outRoughness, out float outMetallic)
{
    OloVTSector s = oloVTDecodeSector(sectorA, sectorB);
    // Derivatives come from the CONTINUOUS scaled UV; the clamp below only
    // affects the VALUE. The 0.9999999 ceiling restores the pre-adaptive edge
    // semantics: a terrain UV of exactly 1.0 (the outer rim's vertex UV)
    // would otherwise wrap `fract` to 0 and sample the last sector image's
    // ORIGIN instead of its far edge.
    vec2 scaledUV = terrainUV * p.SectorsWide;
    float mip = oloVTSectorMip(s, scaledUV);
    vec2 clampedScaledUV = clamp(terrainUV, vec2(0.0), vec2(0.9999999)) * p.SectorsWide;
    vec2 virtualUV = oloVTSectorVirtualUV(s, clampedScaledUV);
    // Record what this pixel wanted BEFORE clamping to what is resident: the
    // request list has to describe the camera, not the cache. A mip below
    // zero survives into the word — it is the grow signal.
    oloVTWriteFeedback(p, fragCoord, virtualUV, mip, s.MaxMip);
    if (s.Ready < 0.5)
    {
        outAlbedo = vec3(0.0);
        outAO = 1.0;
        outNormal = vec3(0.0, 0.0, 1.0);
        outRoughness = 1.0;
        outMetallic = 0.0;
        return false;
    }
    oloVTSampleSurface(p, s, virtualUV, mip, outAlbedo, outAO, outNormal, outRoughness, outMetallic);
    return true;
}

#endif // OLO_TERRAIN_VIRTUAL_TEXTURE_GLSL
