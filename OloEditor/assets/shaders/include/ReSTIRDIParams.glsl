// The ReSTIR DI parameter block and its enums, alone in a file so a shader can
// declare them BEFORE the helpers that read them. Issue #1140.
//
// The split exists for one concrete reason: the alpha-MASK test a visibility ray
// needs reads the GPU Scene slot counts and the material-texture table out of
// this block, and it must be defined before include/RayTracingAlphaTest.glsl is
// pulled in — which is itself before ReSTIRDICommon.glsl, which needs the alpha
// test. Same arrangement as TerrainParamsBlock.glsl.
#ifndef OLO_RESTIR_DI_PARAMS_GLSL
#define OLO_RESTIR_DI_PARAMS_GLSL

// UBO_RAY_TRACING (65) — the SAME binding the path tracer and the ray-traced
// shadow tier refill per dispatch. UBO_TERRAIN_BRUSH (83) is the last engine
// binding and UBO_BINDING_LIMIT (84) is the GL 4.6 guaranteed floor, so there is
// no free binding left to take: the dispatch-local block pattern (#691) is a
// requirement here, not a style choice. Shared verbatim by all four ReSTIR
// draws so one upload feeds the chain; mirrored on the CPU by
// UBOStructures::ReSTIRDIUBO and pinned by ReSTIRDIContractTest.
layout(std140, binding = 65) uniform ReSTIRDIParams
{
    mat4 u_InvView;
    mat4 u_InvProjection;
    mat4 u_View;
    // Last frame's view-projection, for the temporal draw's reprojection when
    // the G-Buffer carries no velocity.
    mat4 u_PrevViewProjection;
    // xy = TLAS device address, z = instance mask, w = frame index (the sampler
    // decorrelator; a fixed value makes a run reproducible).
    uvec4 u_TlasAddressAndFrame;
    // x = instance slots, y = geometry slots, z = material slots, w = LIVE light slots
    uvec4 u_SlotCounts;
    // xy = emissive table device address, z = triangle count, w = flags (OLO_RESTIR_FLAG_*)
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
    // tracer's u_Environment.a carries, from the same EmissiveTriangleTable;
    // y = max ray distance; z = max radiance clamp (<= 0 off); w = debug view.
    vec4 u_EstimatorParams;
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
};

#define OLO_RESTIR_FLAG_HISTORY_VALID 1u
#define OLO_RESTIR_FLAG_TEXTURES 2u
#define OLO_RESTIR_FLAG_VISIBILITY_REUSE 4u
#define OLO_RESTIR_FLAG_TEMPORAL_REUSE 8u
#define OLO_RESTIR_FLAG_SPATIAL_REUSE 16u

// Debug views — mirror ReSTIRDIDebugView (ReSTIRDITechnique.h). Append only.
#define OLO_RESTIR_VIEW_RADIANCE 0
#define OLO_RESTIR_VIEW_RAW_CANDIDATE 1
#define OLO_RESTIR_VIEW_HISTORY_VALIDITY 2
#define OLO_RESTIR_VIEW_VARIANCE 3
#define OLO_RESTIR_VIEW_RESERVOIR_M 4
#define OLO_RESTIR_VIEW_RESERVOIR_W 5
#define OLO_RESTIR_VIEW_SAMPLE_KIND 6
#define OLO_RESTIR_VIEW_BIAS_CLAMP 7

// Compile-time loop bounds with runtime breaks, mirroring
// kReSTIRDIMaxInitialCandidates / kReSTIRDIMaxSpatialNeighbours. A
// uniform-driven bound cannot be unrolled and a bad upload can hang the GPU
// (RayTracedShadow.glsl's rule); ReSTIRDIContractTest pins these against the
// C++ constants.
#define OLO_RESTIR_MAX_INITIAL_CANDIDATES 64u
#define OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS 16u

// Every ray starts this far along its own direction, on top of the normal
// offset: the normal offset is perpendicular to the error a grazing ray makes,
// not along it. Metres. Same constant and same reason as RayTracedShadow.glsl.
const float OLO_RESTIR_RAY_TMIN = 0.005;

#endif // OLO_RESTIR_DI_PARAMS_GLSL
