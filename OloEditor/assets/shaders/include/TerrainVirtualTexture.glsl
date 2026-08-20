// =============================================================================
// Terrain virtual texturing — the shading side (issue #715, slice 1).
//
// One indirection lookup plus one cache sample per pixel replaces the per-pixel
// splat blend, so terrain shading cost stops scaling with layer count. The
// blend itself now happens once per cache tile in
// compute/TerrainVTTileBake.comp.
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

// The VT half of the TerrainParams UBO, unpacked. Passed as three vec4s rather
// than read from the block directly so this file carries no binding dependency
// and the same helpers serve the forward and deferred paths unchanged.
struct OloVTParams
{
    float PagesWide;      // virtual pages across at mip 0
    float PageTexels;     // unique texels per page edge
    float BorderTexels;
    float TileTexels;     // PageTexels + 2 * BorderTexels
    float CacheTexels;    // physical cache edge, in texels
    float MaxMip;         // the 1x1 level
    vec2  FeedbackDims;
    float Enabled;        // 0 = shade from the splat path instead
    float FeedbackFrame;  // which screen-hash slot writes feedback this frame
    float Downscale;      // feedback target is the viewport / this
    float DownscaleLog2;
};

OloVTParams oloVTUnpackParams(vec4 p0, vec4 p1, vec4 p2)
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
    return p;
}

// The mip a pixel wants, in mip-0 virtual texels per pixel. Isotropic log2 of
// the longer derivative — van Waveren's anisotropic refinement biases the
// REQUEST, not the lookup, so it belongs with the adaptive slice rather than
// here. C++ twin: VTComputeMip().
float oloVTComputeMip(OloVTParams p, vec2 virtualUV)
{
    vec2 texelUV = virtualUV * p.PagesWide * p.PageTexels;
    vec2 dx = dFdx(texelUV);
    vec2 dy = dFdy(texelUV);
    float lenSq = max(dot(dx, dx), dot(dy, dy));
    return (lenSq > 0.0) ? 0.5 * log2(lenSq) : 0.0;
}

// Pack one feedback word. C++ twin: VTPackFeedback().
uint oloVTPackFeedback(uvec2 mip0Page, uint mip)
{
    return 0x80000000u | ((mip & 0xFu) << 24u) | ((mip0Page.y & 0xFFFu) << 12u) | (mip0Page.x & 0xFFFu);
}

// Record what this pixel wanted, for the readback loop.
//
// Only ONE pixel of each Downscale x Downscale screen block writes per frame,
// and which one cycles with FeedbackFrame — the Call of Duty scheme. That keeps
// the feedback target small (a full-resolution one would be a 4 MB readback)
// while still covering every screen pixel over Downscale^2 frames, which is
// what makes the request list converge rather than lock onto a fixed lattice.
void oloVTWriteFeedback(OloVTParams p, vec2 screenPos, vec2 virtualUV, float mip)
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

    uint clampedMip = uint(clamp(mip, 0.0, p.MaxMip));
    vec2 clampedUV = clamp(virtualUV, vec2(0.0), vec2(0.9999999));
    uvec2 mip0Page = uvec2(clampedUV * p.PagesWide);

    b_TerrainVTFeedback[coord.y * dims.x + coord.x] = oloVTPackFeedback(mip0Page, clampedMip);
}

// One indirection texel, unpacked. C++ twin: VTUnpackIndirection().
struct OloVTIndirection
{
    vec2  Tile;   // physical tile coordinate
    float Mip;    // the mip of the page ACTUALLY resident there
};

// Resolve a virtual UV at a requested mip.
//
// texelFetch, not texture(): this is a point lookup of an exact texel at an
// exact level, and a filtered read would blend two different pages' physical
// addresses into an address that points at neither. It also means the lookup
// does not depend on the indirection texture's sampler state at all.
OloVTIndirection oloVTLookup(OloVTParams p, vec2 virtualUV, int mip)
{
    int clampedMip = clamp(mip, 0, int(p.MaxMip));
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

// @brief The whole surfacing lookup: one indirection fetch, two cache samples.
//
// `outNormal` is TANGENT space, matching what the splat path's per-layer normal
// blend produces, so the caller's existing TBN transform applies unchanged.
void oloVTSampleSurface(OloVTParams p, vec2 virtualUV, int mip,
                        out vec3 outAlbedo, out float outAO,
                        out vec3 outNormal, out float outRoughness, out float outMetallic)
{
    OloVTIndirection ind = oloVTLookup(p, virtualUV, mip);
    vec2 physUV = oloVTVirtualToPhysicalUV(p, virtualUV, ind);

    vec4 c0 = texture(u_TerrainVTCache, vec3(physUV, 0.0));
    vec4 c1 = texture(u_TerrainVTCache, vec3(physUV, 1.0));

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

#endif // OLO_TERRAIN_VIRTUAL_TEXTURE_GLSL
