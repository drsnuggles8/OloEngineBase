#ifndef SCREEN_VELOCITY_GLSL
#define SCREEN_VELOCITY_GLSL

// THE ENGINE'S SCREEN-SPACE VELOCITY (#1552): current minus previous, in UV
// units, with the TAA jitter taken out. Both view-projections carry their own
// frame's sub-pixel jitter, and a jittered projection moves every vertex by one
// constant offset, so a plain difference of the two NDC positions moves by the
// jitter's change where nothing moved at all -- and the temporal resolves then
// resampled their history by it every frame (a stochastic coat kept 3x of an
// 11x accumulation). Every velocity writer goes through this, so static content
// writes exactly zero.
//
// Needs the camera block's u_JitterUV in scope: CameraCommon.glsl includes this
// after its block, and a stage that declares its own CameraMatrices block
// carries the member through to u_JitterUV and includes this after it.
vec2 oloVelocityFromNdc(vec2 ndcCurr, vec2 ndcPrev)
{
    return ((ndcCurr - ndcPrev) * 0.5) - (u_JitterUV.xy - u_JitterUV.zw);
}

// The same correction for a stage that hands a fragment stage WITHOUT a camera
// block the clip positions its velocity is computed from: x - 2jw over w is
// x/w - 2j at every interpolated fragment, so that fragment stage keeps the
// plain (ndcCurr - ndcPrev) * 0.5 and gets the unjittered answer. Pass
// u_JitterUV.xy with the current position and u_JitterUV.zw with the previous.
vec4 oloUnjitterClip(vec4 clip, vec2 jitterUV)
{
    return vec4(clip.xy - (2.0 * jitterUV * clip.w), clip.zw);
}

#endif // SCREEN_VELOCITY_GLSL
