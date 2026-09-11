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
// ReSTIR GI, draw A of four: INITIAL CANDIDATES. Issue #1169 (#979 Phase 3).
//
// WHAT IT DOES. For every G-Buffer pixel, trace `u_ResamplingCounts.x`
// cosine-weighted bounce rays, shade each vertex it finds with its DIFFUSE lobe
// (one NEE draw plus the probe cache read AT THAT VERTEX), and resample them
// down to one with RIS against the target function.
//
// WHY THIS DRAW IS THE EXPENSIVE ONE, AND WHY ITS DEFAULT CANDIDATE COUNT IS 1
// WHERE DI'S IS 32. A DI candidate is a table lookup and some arithmetic; 32 of
// them cost the one visibility ray at the end. A GI candidate is a BOUNCE RAY
// plus an NEE SHADOW RAY at the vertex it lands on, so the cost is linear in the
// candidate count with a factor of two rays. The whole economy of the tier is
// that REUSE, not candidate count, is what lowers the variance.
//
// WHAT IT DELIBERATELY DOES NOT COLLECT: EMISSION at the bounce vertex, and a
// sphere light the bounce ray stops at. Light that leaves a surface and arrives
// with no reflection in between is DIRECT lighting, so it is ReSTIR DI's — and
// taking it here would double-count every emissive surface in the scene EXACTLY
// (design note §1.1). An emissive-lit room would come out twice as bright, which
// reads as "the new GI tier is a bit strong".
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

// The reservoir, in the packed layout ReservoirGI.glsl defines. Three RGBA32F
// planes, plus the surface plane the temporal draw needs to answer #976's
// history-validity question next frame.
layout(location = 0) out vec4 o_Reservoir0;
layout(location = 1) out vec4 o_Reservoir1;
layout(location = 2) out vec4 o_Reservoir2;
// xy = oct shading normal, z = roughness, w = VIEW depth. The #976 record for
// this frame; extracted into the ReSTIRGI SurfaceGeometry history plane, and
// ALSO what next frame's temporal draw reconstructs last frame's shading point
// from for its reconnection Jacobian (design note §6.3).
layout(location = 3) out vec4 o_Surface;
// The RAW resampled contribution, before any reuse — the un-denoised signal the
// RawCandidate debug view shows. #979's non-goal is explicit that a tier whose
// raw signal cannot be looked at is "one noisy sample plus an opaque denoiser".
// Alpha carries the GLOSSY-VERTEX count for this pixel, normalised by the
// candidate count: the fraction of its bounces that landed somewhere polished
// enough for the dropped specular lobe to matter.
layout(location = 4) out vec4 o_RawCandidate;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)   // TEX_POSTPROCESS_DEPTH
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)  // TEX_GBUFFER_ALBEDO
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)  // TEX_GBUFFER_NORMAL
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45) // TEX_GBUFFER_EMISSIVE
#define u_EnvironmentCube OLO_HEAP_TEX_CUBE(11) // TEX_USER_1: the environment, at LOD 0
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;    // scene depth (nonlinear, [0,1])
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;   // RT0: rgb = albedo, a = metallic
layout(binding = 44) uniform sampler2D u_GBufferNormal;   // RT1: rg = oct world normal, z = roughness, w = ao
layout(binding = 45) uniform sampler2D u_GBufferEmissive; // RT2: rgb = emissive, a = packed flags
layout(binding = 11) uniform samplerCube u_EnvironmentCube; // TEX_USER_1: the environment, at LOD 0
#endif

#include "include/SkyDepth.glsl"
#include "include/PBRCommon.glsl"
// THE PROBE CACHE, READ AT THE BOUNCE VERTEX — the whole DDGI hand-off
// (design note §5). This is the ONLY ReSTIR GI draw that includes it: the cache
// is read once per path, at x1, and never at x0 while this tier is live.
// LightProbeSampling.glsl brings its own bindings (UBO 22 and 51, SSBO 8,
// samplers 56 / 58 / 64), which is exactly why the other three draws do not
// include it.
#include "include/LightProbeSampling.glsl"
// The GPU Scene tables, the parameter block, the shared ray-hit machinery and
// the shared estimator, in the one order that compiles.
#include "include/ReSTIRGISceneAccess.glsl"

#define OLO_RESTIR_GI_PROBE_IRRADIANCE(worldPos, normal, viewDir)                                            \
    sampleProbeVolumeIrradiance(worldPos, normal, viewDir)
#define OLO_RESTIR_GI_ENVIRONMENT_CUBE u_EnvironmentCube
#include "include/ReSTIRGIBounce.glsl"

void main()
{
    // Every output defaults to EMPTY, not to some plausible value. A sky pixel,
    // a frame with no lights and a frame where the trace could not run all write
    // the same empty reservoir, so the worst a stale or absent reservoir can do
    // is fail to light something — never light it wrongly. The counters do the
    // saying; the default makes sure the failure is not also garbage.
    OloGIReservoir reservoir = OloMakeEmptyGIReservoir();
    o_RawCandidate = vec4(0.0);
    o_Surface = vec4(0.0);

    const float depth = texture(u_DepthTexture, v_TexCoord).r;
    const vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    const vec4 packedAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const vec4 packedEmissive = texture(u_GBufferEmissive, v_TexCoord);

    const int gbFlags = oloDecodeGBufferFlags(packedEmissive.a);
    const bool shadeable = !oloDepthIsSky(depth) && !oloGBufferFlagsAreUnlit(gbFlags);

    OloGBufferSurface surface = OloInvalidGBufferSurface();
    if (shadeable)
    {
        surface = OloLoadGBufferSurface(v_TexCoord, depth, packedNormal, packedAlbedo,
                                        oloGBufferFlagsPbrModel(gbFlags));
        // The #976 record for this pixel, written whether or not any candidate
        // survives: next frame's temporal draw needs the SURFACE both to judge
        // validity and to reconstruct the previous shading point, and a pixel
        // whose reservoir is empty is exactly the pixel whose history matters
        // most.
        o_Surface = vec4(packedNormal.xy, packedNormal.z, surface.ViewDepth);
    }

    if (surface.Valid)
    {
        const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - surface.Position);
        const uint candidateCount =
            min(max(u_ResamplingCounts.x, 1u), OLO_RESTIR_GI_MAX_INITIAL_CANDIDATES);
        const ivec2 pixel = ivec2(gl_FragCoord.xy);
        // The path tracer's own sampler, seeded the same way — so a run at a
        // fixed frame index is reproducible and the bounce sequence is
        // comparable with the oracle's.
        OloPathSampler pathSampler = oloPtMakeSampler(
            oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), u_TlasAddressAndFrame.w), u_TlasAddressAndFrame.w);

        uint glossyVertices = 0u;
        for (uint i = 0u; i < OLO_RESTIR_GI_MAX_INITIAL_CANDIDATES; ++i)
        {
            if (i >= candidateCount)
                break;

            // Dimensions are drawn UNCONDITIONALLY, before any validity test, so
            // every pixel spends the same ones in the same order. A sampler whose
            // dimension index depends on which candidates happened to be valid
            // correlates neighbouring pixels, which spatial reuse then amplifies
            // into visible structure.
            const vec2 xiDirection = oloPtGet2D(pathSampler);
            const float xiLightSelect = oloPtGet1D(pathSampler);
            const vec2 xiLightPoint = oloPtGet2D(pathSampler);
            const float xiAccept = oloPtGet1D(pathSampler);

            OloGISample candidate;
            float sourcePdf;
            bool glossyVertex;
            if (!OloGITraceBounceCandidate(surface, xiDirection, xiLightSelect, xiLightPoint, candidate,
                                           sourcePdf, glossyVertex))
            {
                // A rejected draw still counts toward M: it WAS a candidate the
                // estimator considered, and dropping it would inflate every
                // surviving candidate's weight by the rejection rate.
                reservoir.M += 1.0;
                continue;
            }
            if (glossyVertex)
                glossyVertices += 1u;

            if (!(sourcePdf > 0.0))
            {
                reservoir.M += 1.0;
                continue;
            }

            // The source density is ALREADY in solid angle at this shading point
            // — the bounce was drawn cosine-weighted over the hemisphere — and
            // the target function is evaluated in the same measure, so the RIS
            // weight needs no conversion here. The AREA measure the reservoir
            // stores the vertex in only becomes visible at REUSE, as the shift
            // Jacobian (design note §4.1). Writing a conversion here "for
            // symmetry with DI" would apply the Jacobian's geometry twice.
            const float targetPdf = OloGITargetPdf(surface, candidate, viewDirection);
            // OloGIReservoirUpdate increments M itself, so this arm must not.
            OloGIReservoirUpdate(reservoir, candidate, targetPdf / sourcePdf, targetPdf, xiAccept);
        }

        OloGIReservoirFinalizeInitial(reservoir);
        if (glossyVertices != 0u)
            reservoir.Diagnostics |= OLO_GI_DIAG_GLOSSY_VERTEX;
        if (!OloGIReservoirIsEmpty(reservoir) && OloGISampleIsDistant(reservoir.Sample.Kind))
            reservoir.Diagnostics |= OLO_GI_DIAG_ENVIRONMENT;

        // The raw signal, before any reuse: what one RIS-resampled bounce looks
        // like on its own. Alpha is the fraction of this pixel's bounces that
        // landed on a polished vertex, so the dropped specular lobe is a number
        // the resolve can put on screen rather than a caveat in a comment.
        vec3 raw = vec3(0.0);
        if (!OloGIReservoirIsEmpty(reservoir) && reservoir.W > 0.0)
            raw = OloGIBounceContribution(surface, reservoir.Sample, viewDirection) * reservoir.W;
        o_RawCandidate = vec4(raw, float(glossyVertices) / float(candidateCount));

        // NO VISIBILITY RAY HERE, and that is the substantive difference from
        // DI's initial draw. DI traces one because its target function is
        // unshadowed and a shadowed emitter sample would otherwise be reused
        // across a neighbourhood. GI's sample vertex was found BY a ray, so it
        // is visible from this pixel by construction — the segment that can be
        // occluded is the RECONNECTION one, which only exists once a NEIGHBOUR
        // reuses this sample, and the resolve is where that is tested
        // (design note §6.2).
    }

    OloPackGIReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
}
