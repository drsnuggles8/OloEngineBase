#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED — binding 57
// is the engine-wide vertex-pull binding.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float Data[];
} u_VertexPull;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    const uint base = uint(gl_VertexIndex) * 5u;
    const vec3 position = vec3(u_VertexPull.Data[base + 0u], u_VertexPull.Data[base + 1u],
                              u_VertexPull.Data[base + 2u]);
    v_TexCoord = vec2(u_VertexPull.Data[base + 3u], u_VertexPull.Data[base + 4u]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

// =============================================================================
// ReSTIR DI, draw A of four: INITIAL CANDIDATES. Issue #1140 (#979 Phase 3).
//
// WHAT IT DOES. For every G-Buffer pixel, draw `u_ResamplingCounts.x` light
// samples from the whole emitter set at a UNIFORM source density, resample them
// down to one with RIS against the target function, and — unless visibility
// reuse is switched off — trace ONE ray to the survivor.
//
// WHY THE VISIBILITY RAY IS HERE AND NOT LATER. A shadowed sample that survives
// into the reservoir is reused temporally and spatially, so it lights a whole
// neighbourhood that should be dark and keeps doing it for as long as the M cap
// allows. Testing the survivor once, at the point it is selected, is what bounds
// that: it costs one ray per pixel per frame regardless of the candidate count,
// which is the entire economic argument for RIS.
//
// The target function deliberately EXCLUDES visibility (see
// OloReSTIRUnshadowedContribution) — a target function with a ray in it would
// make every reuse cost a trace. When the ray kills the survivor the reservoir
// keeps its M (the candidates WERE seen) and zeroes its contribution weight, and
// the diagnostics lane records it. Dropping M instead would tell the next
// frame's temporal merge that this pixel has no history, which it does.
//
// WHERE IT SITS. After the last G-Buffer writer and after RayTracingScenePass;
// before the temporal draw. Vulkan + hardware ray query only — GL_EXT_ray_query
// has no GL representation, so the pass never creates these shaders on GL and
// the tier reports RayTracingUnavailable.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
// Runtime-indexed material textures (ADR 0011 amendment (95), the contract
// #1139/#1142 settled): the directives must sit here, before any other token —
// see DescriptorHeapTextures.glsl. The toolchain floor declared by #1142 is what
// makes this unconditional rather than probed.
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require

// The reservoir, in the packed layout Reservoir.glsl defines. Three RGBA32F
// planes, plus the surface plane the temporal draw needs to answer #976's
// history-validity question next frame.
layout(location = 0) out vec4 o_Reservoir0;
layout(location = 1) out vec4 o_Reservoir1;
layout(location = 2) out vec4 o_Reservoir2;
// xy = oct shading normal, z = roughness, w = VIEW depth. The #976 record for
// this frame; extracted into the ReSTIRDI SurfaceGeometry history plane.
layout(location = 3) out vec4 o_Surface;
// The RAW resampled contribution, before any reuse — the un-denoised signal the
// RawCandidate debug view shows. #979's non-goal is explicit that a tier whose
// raw signal cannot be looked at is "one noisy sample plus an opaque denoiser".
layout(location = 4) out vec4 o_RawCandidate;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)   // TEX_POSTPROCESS_DEPTH
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)  // TEX_GBUFFER_ALBEDO
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)  // TEX_GBUFFER_NORMAL
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45) // TEX_GBUFFER_EMISSIVE
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;    // scene depth (nonlinear, [0,1])
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;   // RT0: rgb = albedo, a = metallic
layout(binding = 44) uniform sampler2D u_GBufferNormal;   // RT1: rg = oct world normal, z = roughness, w = ao
layout(binding = 45) uniform sampler2D u_GBufferEmissive; // RT2: rgb = emissive, a = packed flags
#endif

#include "include/SkyDepth.glsl"
#include "include/PBRCommon.glsl"
// The GPU Scene tables, the parameter block, the alpha-MASK test, the shared
// light sampling and the shared estimator, in the one order that compiles.
#include "include/ReSTIRDISceneAccess.glsl"

void main()
{
    // Every output defaults to EMPTY, not to some plausible value. A sky pixel,
    // a frame with no lights and a frame where the trace could not run all
    // write the same empty reservoir, so the worst a stale or absent reservoir
    // can do is fail to light something — never light it wrongly. The counters
    // do the saying; the default makes sure the failure is not also garbage.
    OloReservoir reservoir = OloMakeEmptyReservoir();
    o_RawCandidate = vec4(0.0);
    o_Surface = vec4(0.0);

    const float depth = texture(u_DepthTexture, v_TexCoord).r;
    const vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    const vec4 packedAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const vec4 packedEmissive = texture(u_GBufferEmissive, v_TexCoord);

    const int gbFlags = oloDecodeGBufferFlags(packedEmissive.a);
    const bool shadeable = !oloDepthIsSky(depth) && !oloGBufferFlagsAreUnlit(gbFlags);

    OloReSTIRSurface surface = OloReSTIRInvalidSurface();
    if (shadeable)
    {
        surface = OloReSTIRLoadSurface(v_TexCoord, depth, packedNormal, packedAlbedo,
                                       oloGBufferFlagsPbrModel(gbFlags));
        // The #976 record for this pixel, written whether or not any candidate
        // survives: next frame's temporal draw needs the SURFACE to judge
        // validity, and a pixel whose reservoir is empty is exactly the pixel
        // whose history matters most.
        o_Surface = vec4(packedNormal.xy, packedNormal.z, surface.ViewDepth);
    }

    if (surface.Valid)
    {
        const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - surface.Position);
        const uint candidateCount =
            min(max(u_ResamplingCounts.x, 1u), OLO_RESTIR_MAX_INITIAL_CANDIDATES);
        const ivec2 pixel = ivec2(gl_FragCoord.xy);
        // The path tracer's own sampler, seeded the same way — so a run at a
        // fixed frame index is reproducible and the candidate sequence is
        // comparable with the oracle's NEE draws.
        OloPathSampler pathSampler = oloPtMakeSampler(
            oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), u_TlasAddressAndFrame.w), u_TlasAddressAndFrame.w);

        for (uint i = 0u; i < OLO_RESTIR_MAX_INITIAL_CANDIDATES; ++i)
        {
            if (i >= candidateCount)
                break;

            // Dimensions are drawn UNCONDITIONALLY, before any validity test,
            // so every pixel spends the same ones in the same order. A sampler
            // whose dimension index depends on which candidates happened to be
            // valid correlates neighbouring pixels, which spatial reuse then
            // amplifies into visible structure.
            const float xiSelect = oloPtGet1D(pathSampler);
            const vec2 xiPoint = oloPtGet2D(pathSampler);
            const float xiAccept = oloPtGet1D(pathSampler);

            OloLightSample candidate;
            float sourcePdf;
            if (!OloReSTIRSampleCandidate(surface, xiSelect, xiPoint, candidate, sourcePdf))
            {
                // A rejected draw still counts toward M: it WAS a candidate the
                // estimator considered, and dropping it would inflate every
                // surviving candidate's weight by the rejection rate.
                reservoir.M += 1.0;
                continue;
            }

            const float pdf = OloReSTIRSourcePdfSolidAngle(candidate, sourcePdf, surface.Position);
            if (!(pdf > 0.0))
            {
                reservoir.M += 1.0;
                continue;
            }

            const float targetPdf = OloReSTIRTargetPdf(surface, candidate, viewDirection);
            // The RIS weight. OloReservoirUpdate increments M itself, so this
            // arm must not.
            OloReservoirUpdate(reservoir, candidate, targetPdf / pdf, targetPdf, xiAccept);
        }

        OloReservoirFinalizeInitial(reservoir);

        // The raw signal, before any reuse: what one RIS-resampled sample looks
        // like on its own.
        if (!OloReservoirIsEmpty(reservoir) && reservoir.W > 0.0)
        {
            o_RawCandidate =
                vec4(OloReSTIRUnshadowedContribution(surface, reservoir.Sample, viewDirection) * reservoir.W, 1.0);
        }

        // Visibility reuse: one ray, on the survivor only.
        if ((u_EmissiveTable.w & OLO_RESTIR_FLAG_VISIBILITY_REUSE) != 0u && !OloReservoirIsEmpty(reservoir) &&
            reservoir.W > 0.0)
        {
            if (!OloReSTIRSampleVisible(surface, reservoir.Sample, u_ReuseParams.z, u_ReuseParams.w))
            {
                // M is KEPT: those candidates were seen, and telling next
                // frame's merge otherwise would claim this pixel has no
                // history. Only the contribution goes to zero.
                reservoir.W = 0.0;
                reservoir.WeightSum = 0.0;
                reservoir.Diagnostics |= OLO_RESERVOIR_DIAG_VISIBILITY_KILLED;
            }
        }
    }

    OloPackReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
}
