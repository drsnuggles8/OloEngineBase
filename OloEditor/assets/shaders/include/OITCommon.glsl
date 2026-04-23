// OITCommon.glsl - Weighted-blended OIT helpers (McGuire & Bavoil 2013).
//
// Transparent passes include this and call ComputeOITWeight / OITEmit
// to produce accum + revealage outputs. The weight is designed to give
// near-correct compositing order without explicit sorting, using depth
// to bias nearby fragments more heavily.
//
// References:
//   Weighted Blended Order-Independent Transparency
//   http://casual-effects.blogspot.com/2014/03/weighted-blended-order-independent.html

#ifndef OIT_COMMON_GLSL
#define OIT_COMMON_GLSL

// McGuire & Bavoil "Weighted Blended" weight function.
// alpha  : fragment alpha in [0, 1]
// viewZ  : linear view-space depth in world units (positive = in front of camera)
// Returns a weight in approximately [1e-2, 3e3]. Fragments closer to the
// camera get a larger weight; depth is normalised by 200 meters (arbitrary
// but works across typical scene scales — tune per-scene if needed).
float ComputeOITWeight(float alpha, float viewZ)
{
    // Normalise depth (200 m is a reasonable "typical scene" depth scale).
    float normZ = viewZ * (1.0 / 200.0);
    // Exponential weighting. Clamp avoids weights that would overflow the
    // RGBA16F accumulator when many fragments overlap.
    float w = alpha * clamp(0.03 / (1e-5 + pow(normZ, 4.0)), 1e-2, 3e3);
    return w;
}

// Convenience wrapper: given a premultiplied-color `color` (rgb * alpha)
// and the computed weight, produce the two fragment outputs.
//
// outAccum     : location 0 — vec4(Ci * ai * wi, ai * wi)
// outRevealage : location 1 — vec4(ai, 0, 0, 0)  (R channel is the
//                 product factor; blend func GL_ZERO/GL_ONE_MINUS_SRC_COLOR
//                 accumulates prod(1 - ai) for us).
void OITPack(vec3 color, float alpha, float weight, out vec4 outAccum, out vec4 outRevealage)
{
    outAccum     = vec4(color * alpha * weight, alpha * weight);
    outRevealage = vec4(alpha, 0.0, 0.0, 0.0);
}

#endif // OIT_COMMON_GLSL
