// =============================================================================
// CloudscapeDepth.glsl — half-resolution cloud/scene-depth classification.
//
// The includer must define OLO_CLOUDSCAPE_DEPTH_TEXTURE to its full-resolution
// scene-depth sampler before including this file.
// =============================================================================

#ifndef CLOUDSCAPE_DEPTH_GLSL
#define CLOUDSCAPE_DEPTH_GLSL

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

// Conventional scene depth clears sky to exactly 1.0. A broad epsilon drops
// valid far geometry; Drift's ridge reaches approximately 0.999904.
bool cloudDepthContainsGeometry(float depth)
{
    return depth < 1.0;
}

#endif // CLOUDSCAPE_DEPTH_GLSL
