// =============================================================================
// RayTracingAlphaTest.glsl — the shared alpha-tested candidate-confirmation
// helper. Issue #978.
//
// A ray query does not get an any-hit shader for free: when a BLAS geometry is
// not flagged opaque, the ray stops on it as a CANDIDATE and the shader has to
// decide. This file is that decision, written once so the ray-query path and
// any future ray-tracing-pipeline any-hit shader cannot drift apart — and, more
// importantly, so neither can drift from the RASTER path.
//
// RASTER/RT CUTOFF PARITY IS STRUCTURAL, NOT COPIED. Both paths read
// `GPUSceneMaterial::AlphaCutoff` and `AlphaMode` out of the same GPU Scene
// material record. There is no second cutoff constant anywhere in this file to
// fall out of sync — which is the failure the acceptance criterion is guarding
// against, and the reason this helper resolves the material rather than taking
// a cutoff as an argument.
//
// THE FOUR STEPS THE ISSUE ASKS FOR, in order:
//   1. barycentrics + primitive index, from the ray query;
//   2. the geometry record, reached from the instance's GPU Scene slot;
//   3. UVs, reconstructed by fetching the triangle's three vertices through the
//      geometry record's DEVICE ADDRESSES and interpolating;
//   4. the alpha-mask sample and the confirm/reject.
//
// LOD POLICY, stated because the issue asks for it to be documented: this
// helper samples the alpha mask at an EXPLICIT LOD 0 (`textureLod(..., 0.0)`).
// A ray query has no implicit derivatives — it is not a quad-shaded fragment —
// so there is no correct hardware LOD available, and any ray-differential
// scheme is explicitly out of scope for #978. LOD 0 is the conservative choice:
// it is the sharpest mip, so a thin cutout stays present rather than dissolving
// into a filtered average at distance, which is the artefact that would make RT
// shadows disagree with raster ones. The cost is alpha-mask cache pressure on
// long rays, and that is the trade this comment exists to record.
//
// WHO SUPPLIES THE TEXTURE. Everything above is complete here. The texture
// FETCH is not, and cannot be yet: outside `OLO_BINDLESS` (a GL-only path this
// engine does not ship enabled) a shader reaches a material texture through a
// PER-DRAW slot binding, and a single ray-query dispatch has no per-draw scope
// to bind arbitrary materials into. So the fetch is a caller-supplied macro,
// `OLO_RT_SAMPLE_ALPHA(materialIndex, uv)`. A consumer that can bind its
// material set defines it and gets the whole helper; a consumer that needs
// arbitrary materials is blocked on the shader-visible sampler heap recorded as
// ADR 0011 §1.2a's follow-up, not on anything in this file.
// =============================================================================

#ifndef OLO_RAY_TRACING_ALPHA_TEST_GLSL
#define OLO_RAY_TRACING_ALPHA_TEST_GLSL

// Sibling include: the resolver processes a nested #include relative to the
// including FILE's own directory (OpenGLShader::ProcessIncludesInternal passes
// fullPath.parent_path()), so a file in include/ names its siblings bare —
// the same form GPUSceneMaterialResolve.glsl uses.
#include "GPUScene.glsl"

// Vertex mirrors OloEngine::Vertex (Vertex.h): 32 bytes, position at 0, normal
// at 12, texcoord at 24. The C++ side static_asserts those three offsets, and
// OLO_RT_VERTEX_STRIDE below is asserted against sizeof(Vertex) by
// RayTracingAlphaParityTest.
#define OLO_RT_VERTEX_STRIDE 32u
#define OLO_RT_VERTEX_TEXCOORD_OFFSET 24u

// The triangle's three vertices are fetched through the geometry record's
// device addresses. This is the same buffer_reference mechanism the probe
// shader uses for its ray and hit arrays, and it is what makes a single
// dispatch able to read ANY mesh's vertex data without a binding.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer OloRtIndexStream
{
    uint Indices[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer OloRtVertexUVStream
{
    // Deliberately a float stream rather than a Vertex struct: std430 would
    // pad a vec3-vec3-vec2 struct differently from the C++ 32-byte layout, and
    // indexing floats by a computed offset cannot get that wrong.
    float Floats[];
};

// Interpolate the hit triangle's UV from the ray query's barycentrics.
//
// `barycentrics` is what rayQueryGetIntersectionBarycentricsEXT returns: the
// (b1, b2) pair, with b0 = 1 - b1 - b2 implied. Getting that convention wrong
// is a UV that is right at the vertices and wrong everywhere else, which reads
// as "the alpha mask is subtly misaligned" rather than as an error.
vec2 oloRayTracingHitUV(GPUSceneGeometry geometry, uint primitiveIndex, vec2 barycentrics)
{
    OloRtIndexStream indices = OloRtIndexStream(geometry.IndexAddress);
    OloRtVertexUVStream vertices = OloRtVertexUVStream(geometry.VertexAddress);

    // primitiveIndex is relative to this geometry's range, so FirstIndex and
    // BaseVertex both have to be re-applied — exactly as the raster draw does.
    const uint indexBase = geometry.FirstIndex + primitiveIndex * 3u;
    const int i0 = int(indices.Indices[indexBase + 0u]) + geometry.BaseVertex;
    const int i1 = int(indices.Indices[indexBase + 1u]) + geometry.BaseVertex;
    const int i2 = int(indices.Indices[indexBase + 2u]) + geometry.BaseVertex;

    const uint stride = OLO_RT_VERTEX_STRIDE / 4u;
    const uint uvOffset = OLO_RT_VERTEX_TEXCOORD_OFFSET / 4u;

    const vec2 uv0 = vec2(vertices.Floats[uint(i0) * stride + uvOffset + 0u],
                          vertices.Floats[uint(i0) * stride + uvOffset + 1u]);
    const vec2 uv1 = vec2(vertices.Floats[uint(i1) * stride + uvOffset + 0u],
                          vertices.Floats[uint(i1) * stride + uvOffset + 1u]);
    const vec2 uv2 = vec2(vertices.Floats[uint(i2) * stride + uvOffset + 0u],
                          vertices.Floats[uint(i2) * stride + uvOffset + 1u]);

    const float b0 = 1.0 - barycentrics.x - barycentrics.y;
    return uv0 * b0 + uv1 * barycentrics.x + uv2 * barycentrics.y;
}

// True when this material needs a candidate confirmed at all. Opaque and
// blended materials do not: opaque is committed by the hardware, and a blended
// surface is not an occluder for the effects this substrate serves.
//
// OLO_GPU_SCENE_ALPHA_MODE_MASK mirrors OloEngine::AlphaMode::Mask.
#define OLO_GPU_SCENE_ALPHA_MODE_MASK 1u

bool oloRayTracingNeedsAlphaTest(GPUSceneMaterial material)
{
    return material.AlphaMode == OLO_GPU_SCENE_ALPHA_MODE_MASK;
}

// The confirm/reject decision, given an alpha the caller sampled.
//
// Split from the sampling so the POLICY — which materials are tested, which
// cutoff, which comparison — is one function that a CPU test can mirror
// exactly, independent of how a given consumer gets at its texture. The
// comparison is `>= cutoff` accepts, matching the raster path's
// `if (alpha < cutoff) discard;`.
bool oloRayTracingConfirmAlpha(GPUSceneMaterial material, float sampledAlpha)
{
    if (!oloRayTracingNeedsAlphaTest(material))
    {
        return true;
    }
    return sampledAlpha >= material.AlphaCutoff;
}

// The whole helper, for a consumer that has defined OLO_RT_SAMPLE_ALPHA.
//
// Returns true to CONFIRM the candidate (the ray hits) and false to REJECT it
// (the ray passes through). A material whose albedo map is absent confirms:
// a masked material with no mask is a solid one.
#ifdef OLO_RT_SAMPLE_ALPHA
bool oloRayTracingConfirmCandidate(GPUSceneGeometry geometry, GPUSceneMaterial material, uint materialIndex,
                                   uint primitiveIndex, vec2 barycentrics)
{
    if (!oloRayTracingNeedsAlphaTest(material))
    {
        return true;
    }
    if ((material.Flags & OLO_GPU_SCENE_MATERIAL_ALBEDO_MAP) == 0u)
    {
        return true;
    }
    const vec2 uv = oloRayTracingHitUV(geometry, primitiveIndex, barycentrics);
    // Explicit LOD 0 — see the LOD POLICY note at the top of this file.
    const float sampledAlpha = OLO_RT_SAMPLE_ALPHA(materialIndex, uv);
    return oloRayTracingConfirmAlpha(material, sampledAlpha);
}
#endif // OLO_RT_SAMPLE_ALPHA

#endif // OLO_RAY_TRACING_ALPHA_TEST_GLSL
