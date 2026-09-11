// The ReSTIR GI parameter block and its enums, alone in a file so a shader can
// declare them BEFORE the helpers that read them. Issue #1169.
//
// The split exists for the same concrete reason ReSTIRDIParams.glsl's does:
// include/RayTracedSurfaceHit.glsl reads the GPU Scene slot counts, the TLAS
// address and the material-texture table out of this block, and it must be
// defined before that include is pulled in — which is itself before
// ReSTIRGICommon.glsl, which needs the hit machinery. Same arrangement as
// TerrainParamsBlock.glsl.
#ifndef OLO_RESTIR_GI_PARAMS_GLSL
#define OLO_RESTIR_GI_PARAMS_GLSL

// UBO_RAY_TRACING (65) — the SAME binding the path tracer, the ray-traced
// shadow tier and ReSTIR DI refill per dispatch. UBO_TERRAIN_BRUSH (83) is the
// last engine binding and UBO_BINDING_LIMIT (84) is the GL 4.6 guaranteed
// floor, so there is no free binding left to take: the dispatch-local block
// pattern (#691) is a requirement here, not a style choice. Shared verbatim by
// all four ReSTIR GI draws so one upload feeds the chain; mirrored on the CPU by
// UBOStructures::ReSTIRGIUBO and pinned by ReSTIRGIContractTest.
layout(std140, binding = 65) uniform ReSTIRGIParams
{
    mat4 u_InvView;
    mat4 u_InvProjection;
    mat4 u_View;

    // THE PREVIOUS FRAME'S RECONSTRUCTION, AND WHY GI CARRIES ONE WHERE DI
    // REFUSED TO.
    //
    // DI declared a u_PrevViewProjection that no draw ever read — 64 bytes of
    // dead uniform describing a fallback that did not exist — and #1140 deleted
    // it. These two are READ, by the temporal draw, on every frame that reuses
    // history: GI's temporal reuse computes a real reconnection Jacobian, and to
    // do that it has to reconstruct last frame's SHADING POINT from the
    // reprojected UV and the view depth in the surface-history plane.
    //
    // DI is licensed to use J = 1 there because the #976 validity test
    // establishes that last frame's shading point IS this frame's, and DI's
    // Jacobian is insensitive to the sub-pixel difference reprojection leaves.
    // GI's is not: its sample vertex can be centimetres away and dDestSq is in
    // the denominator (design note §6.3). If a change ever makes these dead,
    // delete them the way #1140 deleted DI's.
    mat4 u_PrevInvView;
    mat4 u_PrevInvProjection;

    // xy = TLAS device address, z = instance mask, w = frame index (the sampler
    // decorrelator; a fixed value makes a run reproducible).
    uvec4 u_TlasAddressAndFrame;
    // x = instance slots, y = geometry slots, z = material slots, w = LIVE light slots
    uvec4 u_SlotCounts;
    // xy = emissive table device address, z = triangle count, w = flags (OLO_RESTIR_GI_FLAG_*)
    uvec4 u_EmissiveTable;
    // xy = material texture table device address, z = record count, w = sampler
    // heap byte offset. Carried in the shared block rather than a second one
    // because there is no second binding to put it in.
    uvec4 u_MaterialTable;
    // x = initial candidates, y = spatial neighbours, z = spatial pass index, w = bias mode
    uvec4 u_ResamplingCounts;

    // x = temporal M cap, y = spatial radius in pixels, z = ray epsilon, w = normal bias
    vec4 u_ReuseParams;
    // x = emissive AREA pdf (1 / total emissive area) — the SAME number the path
    // tracer's NEE carries, from the same EmissiveTriangleTable;
    // y = max bounce distance; z = max radiance clamp (<= 0 off); w = debug view.
    vec4 u_EstimatorParams;
    // x = minimum reconnection distance (metres, design note §6.3);
    // y = max sample age in frames (the second staleness bound, §10);
    // z = the roughness below which a bounce vertex's dropped specular lobe is
    //     COUNTED — a reporting threshold, not a correctness gate;
    // w = reserved.
    vec4 u_GIParams;
    // rgb = uniform environment radiance, a = intensity for the environment
    // cube. THE SAME PAIR GpuPathTracer.glsl's u_Environment / u_RayParams.w
    // carry, so a bounce ray that escapes collects what the oracle's escaping
    // ray collects. A second definition of "the sky" is how the two stop
    // agreeing about an outdoor scene's ambient.
    vec4 u_Environment;
    // xyz = previousRenderOrigin - renderOrigin, w = 0. The render origin can
    // SNAP between frames (issue #429), so a previous shading point
    // reconstructed with u_PrevInvView lands in the PREVIOUS render-relative
    // frame; adding this puts it in this one. Zero until the grid snaps, which
    // is why leaving it out looks correct in every test scene near the world
    // origin and misses by exactly the origin 1024 m out.
    vec4 u_PrevOriginDelta;
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
};

#define OLO_RESTIR_GI_FLAG_HISTORY_VALID 1u
#define OLO_RESTIR_GI_FLAG_TEXTURES 2u
// The RESOLVE's reconnection ray, on the surviving sample only. Without it,
// reuse lights surfaces through walls — smoothly, because reuse is spatially
// coherent (design note §6.2).
#define OLO_RESTIR_GI_FLAG_RECONNECTION_VISIBILITY 4u
#define OLO_RESTIR_GI_FLAG_TEMPORAL_REUSE 8u
#define OLO_RESTIR_GI_FLAG_SPATIAL_REUSE 16u
// The MOMENTS plane, deliberately NOT folded into HISTORY_VALID. That flag means
// "reuse last frame's RESERVOIRS", which TemporalReuse=off clears — and the
// Variance debug view is a diagnostic for exactly that configuration. Sharing
// one bit made DI's variance view read a flat instantaneous value whenever
// temporal reuse was off, while the stats still reported all five history planes
// present, so the view looked converged and said nothing.
#define OLO_RESTIR_GI_FLAG_MOMENTS_VALID 32u
// Read the probe cache at the BOUNCE VERTEX as the path tail (design note §5).
// This is what keeps DDGI a cache rather than switching it off.
#define OLO_RESTIR_GI_FLAG_DDGI_TAIL 64u
// The reconnection ray per spatial NEIGHBOUR. Costs k rays per pixel.
#define OLO_RESTIR_GI_FLAG_SPATIAL_RECONNECTION_VISIBILITY 128u
// An environment source is bound, so a bounce ray that escapes has something to
// collect. Without it an escaping ray contributes nothing, which is correct and
// is NOT the same as a black environment being sampled.
#define OLO_RESTIR_GI_FLAG_ENVIRONMENT 256u

// Debug views — mirror ReSTIRGIDebugView (ReSTIRGITechnique.h). Append only.
#define OLO_RESTIR_GI_VIEW_RADIANCE 0
#define OLO_RESTIR_GI_VIEW_RAW_CANDIDATE 1
#define OLO_RESTIR_GI_VIEW_HISTORY_VALIDITY 2
#define OLO_RESTIR_GI_VIEW_VARIANCE 3
#define OLO_RESTIR_GI_VIEW_RESERVOIR_M 4
#define OLO_RESTIR_GI_VIEW_RESERVOIR_W 5
#define OLO_RESTIR_GI_VIEW_SAMPLE_KIND 6
#define OLO_RESTIR_GI_VIEW_SAMPLE_AGE 7
#define OLO_RESTIR_GI_VIEW_SAMPLE_RADIANCE 8
#define OLO_RESTIR_GI_VIEW_RECONNECTION_LENGTH 9
#define OLO_RESTIR_GI_VIEW_BIAS_CLAMP 10

// Compile-time loop bounds with runtime breaks, mirroring
// kReSTIRGIMaxInitialCandidates / kReSTIRGIMaxSpatialNeighbours. A
// uniform-driven bound cannot be unrolled and a bad upload can hang the GPU
// (RayTracedShadow.glsl's rule); ReSTIRGIContractTest pins these against the
// C++ constants.
//
// THE CANDIDATE CEILING IS 8, NOT DI'S 64. A DI candidate is a table lookup; a
// GI candidate is a BOUNCE RAY plus an NEE SHADOW RAY at the vertex it finds,
// so 32 of them would be 64 rays per pixel. The ceiling exists for A/B work
// rather than for shipping, and the default is 1.
#define OLO_RESTIR_GI_MAX_INITIAL_CANDIDATES 8u
#define OLO_RESTIR_GI_MAX_SPATIAL_NEIGHBOURS 16u

// Every ray starts this far along its own direction, on top of the normal
// offset: the normal offset is perpendicular to the error a grazing ray makes,
// not along it. Metres. Same constant and same reason as RayTracedShadow.glsl.
const float OLO_RESTIR_GI_RAY_TMIN = 0.005;

#endif // OLO_RESTIR_GI_PARAMS_GLSL
