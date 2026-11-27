// ReSTIRRenderScale.glsl — the ReSTIR DI and GI passes under the dynamic
// render scale (issues #1537, #1823).
//
// Under Renderer3D::SetRenderScale the G-Buffer frame fills only the
// [0, bounds) corner of the display-sized targets these passes read
// (include/RenderScaleBounds.glsl). The passes sample at texture uv, which
// stays 1:1; this include opts the shared headers into the frame mapping:
//
//   - include/GBufferRaySurface.glsl reconstructs the shading point from the
//     frame uv (OLO_GBUFFER_RAY_SURFACE_BOUNDS);
//   - include/TemporalResolve.glsl tests and reprojects history inside the
//     rendered corner (OLO_TEMPORAL_HISTORY_BOUNDS).
//
// The bounds come from the DRS UBO at binding 33, which every ReSTIR DI and GI
// shader leaves free. Include it after PBRCommon.glsl and BEFORE any header
// that pulls in GBufferRaySurface.glsl or TemporalResolve.glsl (the scene
// access headers, SurfaceHistory.glsl), or those headers keep the scale-free
// default. ReSTIR PT does not include this file: its device rig publishes no
// DRS UBO, so it reads the bounds from its own parameter block.
#ifndef OLO_RESTIR_RENDER_SCALE_GLSL
#define OLO_RESTIR_RENDER_SCALE_GLSL

#include "RenderScaleBounds.glsl"

#define OLO_GBUFFER_RAY_SURFACE_BOUNDS oloRenderScaleBounds()
#define OLO_TEMPORAL_HISTORY_BOUNDS oloRenderScaleBounds()

#endif // OLO_RESTIR_RENDER_SCALE_GLSL
