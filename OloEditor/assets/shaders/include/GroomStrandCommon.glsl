// GroomStrandCommon.glsl — the strand coverage arithmetic, issue #1246.
//
// EVERYTHING IN THIS FILE HAS A C++ TWIN in OloEngine/Groom/GroomCoverage.h,
// and the two must agree bit for bit. That is not a style preference: the
// criterion-1 comparison, and the coverage error every capture is judged
// against, are computed by the C++ side. A shader that widened strands
// differently, or hashed differently, would make every measured number a
// measurement of something that is not on screen.
//
// GroomStrandGpuParityTest renders tests/GroomStrandHashParityProbe.glsl —
// which includes THIS file — over a 64x64 integer grid and compares every
// texel against GroomCoverage::StochasticHash and the widened alpha. It asks
// for EXACT equality, because none of this is a floating-point estimator: the
// hash is integer arithmetic whose wraparound is defined identically in C++
// and GLSL, and its result is a 24-bit integer scaled by an exact power of
// two. If you change either side, that test is what tells you.

#ifndef GROOM_STRAND_COMMON_GLSL
#define GROOM_STRAND_COMMON_GLSL

// A strand narrower than one pixel is rasterised at one pixel and pays for the
// widening in alpha. Half-widths throughout, so this is half a pixel.
//
// WHY: a quarter-pixel strand rasterised honestly produces a fragment only
// where it happens to cross a pixel centre, so a coat of them sparkles as the
// camera moves and disappears entirely at distance. Widened and weighted, it
// is a continuous line of the correct total energy. This constant is the whole
// mechanism.
#define OLO_GROOM_MIN_HALF_WIDTH_PX 0.5

// The alpha a fragment carries, given the strand's TRUE projected half width in
// pixels: the ratio of the real width to the width it was drawn at.
float oloGroomWidenedAlpha(float trueHalfWidthPixels)
{
    return clamp(trueHalfWidthPixels / OLO_GROOM_MIN_HALF_WIDTH_PX, 0.0, 1.0);
}

// The half width a strand is actually rasterised at.
float oloGroomRasterHalfWidth(float trueHalfWidthPixels)
{
    return max(trueHalfWidthPixels, OLO_GROOM_MIN_HALF_WIDTH_PX);
}

// The stochastic alpha test's hash, in [0, 1).
//
// Twin of GroomCoverage::StochasticHash. Every operation is a 32-bit unsigned
// one whose wraparound is defined identically in C++ and GLSL, and the final
// shift-and-scale uses 24 bits so the result lands on the same 2^-24 grid on
// both sides and can never reach exactly 1.0.
float oloGroomStochasticHash(uint pixelX, uint pixelY, uint frameIndex, uint segmentId, uint seed)
{
    uint h = pixelX * 73856093u;
    h ^= pixelY * 19349663u;
    h ^= segmentId * 83492791u;
    h ^= frameIndex * 2654435761u;
    h ^= seed * 40503u;

    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;

    return float(h >> 8) * (1.0 / 16777216.0);
}

// Pixels per world unit at clip w == 1, from the projection matrix and the
// viewport height. Exact for a perspective projection (where element [1][1] is
// 1/tan(fovY/2)) and for an orthographic one (where clip w is 1 and [1][1] is
// 2/orthoHeight), so no branch on projection type is needed — the same
// derivation GroomCoverage::ProjectGroom uses.
//
// THE abs() IS LOAD-BEARING, AND ITS ABSENCE IS INVISIBLE ON OPENGL.
// Vulkan's clip space has +Y downwards, so the engine uploads a projection
// whose [1][1] is NEGATIVE there. Without the abs() this returns a negative
// pixels-per-unit, every strand gets a negative half width, oloGroomWidenedAlpha
// clamps that to 0, and the alpha test discards EVERY fragment: the pass runs,
// the draw is recorded, the vertex pull and the UBO are both correct, and not
// one pixel changes — with no error and no validation message anywhere. The
// sign is a clip-space convention; the MAGNITUDE is the thing being asked for.
// The engine guards the same element the same way — see Renderer3D.h's
// "|cull projection[1][1]|" and RenderPipeline.cpp's std::abs on it.
float oloGroomPixelsPerUnitAtUnitW(mat4 projection, float viewportHeight)
{
    return abs(projection[1][1]) * viewportHeight * 0.5;
}

// The per-strand coat tint (issue #1251), unpacked from the strand vertex's
// spare float lane. TWIN OF UnpackGroomCoatTint in OloEngine/Groom/GroomCoat.h,
// and the mask is the load-bearing half: the C++ side forces the top byte to
// 0x3F so every possible payload is a NORMAL float rather than a denormal a
// vertex pipeline may flush to zero. Masking it off here is what turns that
// exponent back into nothing. Dropping the mask would multiply every coat by a
// tint whose blue channel is 0x3F.
vec3 oloGroomUnpackTint(float packed)
{
	uint bits = floatBitsToUint(packed) & 0x00FFFFFFu;
	return vec3(float(bits & 0xFFu), float((bits >> 8) & 0xFFu), float((bits >> 16) & 0xFFu)) * (1.0 / 255.0);
}

// Composition modes. Twin of GroomCompositionMode in
// OloEngine/Groom/GroomVisibility.h; only the two modes the strand pass
// IMPLEMENTS appear here. AlphaToCoverage and WeightedBlendedOIT are enum
// values the CPU seam can reason about and refuse — see that header and
// docs/analysis/groom-strand-visibility-1246.md — and adding a branch for them
// here without the pass state to back it would be a mode that reports active
// and renders as the baseline.
#define OLO_GROOM_MODE_OPAQUE_RIBBON 0
#define OLO_GROOM_MODE_STOCHASTIC_ALPHA 1

#endif // GROOM_STRAND_COMMON_GLSL
