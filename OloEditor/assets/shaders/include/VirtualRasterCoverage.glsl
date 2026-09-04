#ifndef VIRTUAL_RASTER_COVERAGE_GLSL
#define VIRTUAL_RASTER_COVERAGE_GLSL

// =============================================================================
// VirtualRasterCoverage.glsl — the ONE spelling of the virtualized-geometry
// raster's sub-sample-miss test (issue #712).
//
// Rasterization decides coverage at PIXEL CENTRES — (x + 0.5, y + 0.5) in
// window space. A micro-triangle can therefore have a perfectly good
// bounding box and still cover nothing at all, which is the common case at
// micropoly density: the compute software rasterizer used to walk every pixel
// of floor(bbMin)..ceil(bbMax) running three edge functions each, and a
// triangle that fell between sample points paid for the whole box.
//
// A header rather than inline code because the rule has to agree EXACTLY across
// three places — a half-pixel disagreement drops real geometry with nothing to
// show for it:
//   * compute/VirtualClusterRaster.comp — reject + tight inner-loop bounds
//   * tests/ShaderUnit_VirtualSampleBounds.glsl — the L2 probe that runs THIS file
//   * OloEngine/tests/Rendering/VirtualRasterCoverageMirror.h — the CPU mirror,
//     pinned against a brute-force scan by VirtualRasterCoverageTest.
// The hardware raster has no per-triangle reject yet; when it gets one it wants
// these same two functions rather than a fourth spelling of them.
// =============================================================================

// Inclusive range of pixels whose CENTRES can fall inside the window-space box
// [bbMin, bbMax], clamped to a `viewport`-sized render target. Returns false
// when that range is empty in either axis — the box lies entirely between
// sample points, or entirely off-screen — in which case the outputs are
// unspecified and the caller must not rasterize.
//
//     x + 0.5 >= bbMin.x   <=>   x >= ceil (bbMin.x - 0.5)
//     x + 0.5 <= bbMax.x   <=>   x <= floor(bbMax.x - 0.5)
//
// Both bounds are inclusive: the raster's edge test accepts a zero edge
// function, so a centre lying exactly on the box boundary is a covered sample
// and must stay in range.
//
// The clamps run in FLOAT, before the int conversion, so window coordinates
// that left the int range (an inf out of a grazing projection) cannot produce
// an out-of-range float-to-int conversion. Their limits sit one past the
// viewport on each side — `viewport` on the min, -1 on the max — so both stay
// legal ints while still losing the emptiness comparison, letting a fully
// off-screen box take the early out instead of scanning one clamped edge pixel.
bool OloVirtualSampleRange(vec2 bbMin, vec2 bbMax, vec2 viewport,
                           out ivec2 sampleMin, out ivec2 sampleMax)
{
    sampleMin = ivec2(clamp(ceil(bbMin - 0.5), vec2(0.0), viewport));
    sampleMax = ivec2(clamp(floor(bbMax - 0.5), vec2(-1.0), viewport - 1.0));
    return sampleMin.x <= sampleMax.x && sampleMin.y <= sampleMax.y;
}

// Same rule for a triangle given by its three window-space positions.
bool OloVirtualTriangleSampleRange(vec2 s0, vec2 s1, vec2 s2, vec2 viewport,
                                   out ivec2 sampleMin, out ivec2 sampleMax)
{
    return OloVirtualSampleRange(min(s0, min(s1, s2)), max(s0, max(s1, s2)),
                                 viewport, sampleMin, sampleMax);
}

// Window-space signed area of a triangle, doubled — the shared edge-function
// determinant. Positive is counter-clockwise, which is the front face on both
// raster paths (the hardware pipelines are configured CCW-front / cull-back).
// A NaN vertex propagates to NaN here, so `!(area > 0.0)`-shaped guards catch
// degenerates and NaNs together.
float OloVirtualSignedArea2(vec2 s0, vec2 s1, vec2 s2)
{
    return (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
}

#endif // VIRTUAL_RASTER_COVERAGE_GLSL
