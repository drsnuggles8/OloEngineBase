// =============================================================================
// CloudscapeDepth.glsl — half-resolution cloud/scene-depth classification.
//
// The includer must define OLO_CLOUDSCAPE_DEPTH_TEXTURE to its full-resolution
// scene-depth sampler before including this file.
// =============================================================================

#ifndef CLOUDSCAPE_DEPTH_GLSL
#define CLOUDSCAPE_DEPTH_GLSL

#include "SkyDepth.glsl"

#ifndef OLO_CLOUDSCAPE_DEPTH_TEXTURE
#error "Define OLO_CLOUDSCAPE_DEPTH_TEXTURE before including CloudscapeDepth.glsl"
#endif

// One half-resolution cloud invocation represents a 2x2 footprint in the
// full-resolution depth buffer. Conventional depth stores nearer geometry at
// the smaller value, so the minimum is the conservative occluder. Clamping
// duplicates the last row/column for odd-sized viewports.
float cloudConservativeDepth(ivec2 halfResolutionPixel)
{
    ivec2 depthSize = textureSize(OLO_CLOUDSCAPE_DEPTH_TEXTURE, 0);
    ivec2 footprintOrigin = halfResolutionPixel * 2;
    ivec2 maxCoord = depthSize - ivec2(1);
    float depth = 1.0;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            ivec2 coord = min(footprintOrigin + ivec2(x, y), maxCoord);
            depth = min(depth, texelFetch(OLO_CLOUDSCAPE_DEPTH_TEXTURE, coord, 0).r);
        }
    }
    return depth;
}

// Sky is "within an ULP of 1.0", not "exactly 1.0": on AMD the cleared depth
// reads 0.99999994 (issue #1008), and `depth < 1.0` clamped every horizon ray
// at the far plane. SkyDepth.glsl owns the epsilon and the reasoning.
bool cloudDepthContainsGeometry(float depth)
{
    return !oloDepthIsSky(depth);
}

#endif // CLOUDSCAPE_DEPTH_GLSL
