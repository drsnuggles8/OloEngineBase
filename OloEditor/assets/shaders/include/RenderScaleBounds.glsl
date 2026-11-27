// =============================================================================
// RenderScaleBounds.glsl — the dynamic render scale's UV mapping (issue #1537).
//
// Under Renderer3D::SetRenderScale every display-sized target keeps its
// storage and the frame is drawn into its [0, render) corner. The bounds are
// (renderWidth / physicalWidth, renderHeight / physicalHeight), (1, 1) at
// scale 1.0 (DRSUBOData in PostProcessSettings.h).
//
// A screen-space pass samples its inputs 1:1 at TEXTURE UV (v_TexCoord over a
// full-target viewport, gl_FragCoord / size, (pixel + 0.5) / size), which
// stays correct under the scale. What changes is the FRAME UV, the [0, 1]
// position in the rendered image that the camera matrices speak:
//
//   frame UV   = texture UV / bounds   — before UV -> NDC reconstruction
//   texture UV = frame UV * bounds     — after projecting a point back to UV
//
// and a "still on screen" test is texture UV in [0, bounds], not [0, 1].
// A velocity stored as a frame-UV delta becomes a texture-UV delta times the
// bounds, and a length in frame pixels is a length in texture pixels times
// the bounds.
//
// The bounds come from the DRS UBO at binding 33 unless the includer defines
// OLO_RENDER_SCALE_BOUNDS first (a vec2 expression), which a shader whose
// binding 33 is already a sampler must do.
// =============================================================================

#ifndef RENDER_SCALE_BOUNDS_GLSL
#define RENDER_SCALE_BOUNDS_GLSL

#ifndef OLO_RENDER_SCALE_BOUNDS
layout(std140, binding = 33) uniform DRSParams
{
    vec2 u_RenderScaleBounds;
    vec2 _drsPad;
};
#define OLO_RENDER_SCALE_BOUNDS u_RenderScaleBounds
#endif

// The bounds, with a block nobody filled (all zeros: a shader-unit harness, a
// capture outside the frame) read as (1, 1) rather than dividing by zero.
vec2 oloRenderScaleBounds()
{
    vec2 bounds = OLO_RENDER_SCALE_BOUNDS;
    return (bounds.x > 0.0 && bounds.y > 0.0) ? min(bounds, vec2(1.0)) : vec2(1.0);
}

vec2 oloFrameUV(vec2 textureUV)
{
    return textureUV / oloRenderScaleBounds();
}

vec2 oloTextureUV(vec2 frameUV)
{
    return frameUV * oloRenderScaleBounds();
}

// True inside the rendered corner, [0, bounds].
bool oloInRenderedFrame(vec2 textureUV)
{
    return all(greaterThanEqual(textureUV, vec2(0.0))) && all(lessThanEqual(textureUV, oloRenderScaleBounds()));
}

#endif // RENDER_SCALE_BOUNDS_GLSL
