// GBufferRaySurface.glsl — the shading point a screen-space ray-tracing tier
// reconstructs from the G-Buffer. Extracted from include/ReSTIRDICommon.glsl
// for issue #1169 so ReSTIR DI and ReSTIR GI reconstruct the SAME point from
// the same texels.
//
// WHY THAT MATTERS ENOUGH TO BE A FILE. Both tiers trace rays from this point
// and both convert densities through it, so a half-texel or a sign difference
// between two copies would make one tier's rays start somewhere the other's do
// not — and the symptom is a soft difference in contact shadowing, which reads
// as "the two tiers disagree slightly" rather than as a reconstruction bug.
//
// WHAT THE CALLER MUST HAVE IN SCOPE: u_InvView and u_InvProjection (both
// ReSTIR parameter blocks declare them under those names), and
// include/PBRCommon.glsl.
#ifndef OLO_GBUFFER_RAY_SURFACE_GLSL
#define OLO_GBUFFER_RAY_SURFACE_GLSL

// The weather wetness DeferredLighting applies at lighting time (issue #1336) —
// self-contained (UBO 54 + the always-bound cloud-shadow slot 62).
#include "AtmosphereShading.glsl"

// PBRCommon.glsl's MIN_ROUGHNESS, the floor DeferredLighting.glsl applies when
// it reads the G-Buffer roughness. Spelled here because this file does not
// require PBRCommon to be included first.
const float OLO_GBUFFER_RAY_SURFACE_MIN_ROUGHNESS = 0.04;

// What a pixel's G-Buffer says, in the render-relative frame the TLAS is built
// in (issue #429). Reading absolute world positions here would displace every
// ray by exactly the render origin — zero near the world origin, which is where
// every test and benchmark scene sits, and a 1024 m miss the moment the origin
// grid snaps.
struct OloGBufferSurface
{
    bool Valid;
    vec3 Position;
    vec3 GeometricNormal;
    vec3 ShadingNormal;
    vec3 Albedo;
    float Metallic;
    float Roughness;
    float ViewDepth;
    int PbrModel;
};

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl.
vec3 OloGBufferOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 OloGBufferViewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return view.xyz / view.w;
}

// The G-Buffer gives ONE normal, so the geometric and shading normals are the
// same vector here. That is stated rather than hidden: it means a normal-mapped
// surface offsets its own ray origin along the mapped normal, which is the same
// compromise RayTracedShadow.glsl and every other G-Buffer ray consumer makes,
// and it is why each tier's RayOriginNormalBias exists as a tunable.
OloGBufferSurface OloLoadGBufferSurface(vec2 uv, float depth, vec4 packedNormal, vec4 packedAlbedo,
                                        int pbrModel)
{
    OloGBufferSurface s;
    s.Valid = true;
    vec3 viewPos = OloGBufferViewPosFromDepth(uv, depth);
    s.Position = (u_InvView * vec4(viewPos, 1.0)).xyz;
    s.ViewDepth = -viewPos.z;
    s.ShadingNormal = OloGBufferOctDecode(packedNormal.xy);
    s.GeometricNormal = s.ShadingNormal;
    // THE SAME SURFACE THE DEFERRED PASS SHADES (issue #1336). DeferredLighting
    // floors the G-Buffer roughness at MIN_ROUGHNESS when it reads it and then
    // applies the weather wetness to albedo and roughness; a tier that owns this
    // pixel's light (ReSTIR DI / GI / PT) must evaluate the closure on that
    // surface, not on the raw G-Buffer bytes, or a wet street and a near-mirror
    // Legacy floor shade differently the moment the tier engages.
    s.Roughness = max(packedNormal.z, OLO_GBUFFER_RAY_SURFACE_MIN_ROUGHNESS);
    s.Albedo = packedAlbedo.rgb;
    atmosphereApplyWetness(s.Albedo, s.Roughness, s.ShadingNormal);
    s.Metallic = packedAlbedo.a;
    s.PbrModel = pbrModel;
    return s;
}

OloGBufferSurface OloInvalidGBufferSurface()
{
    OloGBufferSurface s;
    s.Valid = false;
    s.Position = vec3(0.0);
    s.GeometricNormal = vec3(0.0, 0.0, 1.0);
    s.ShadingNormal = vec3(0.0, 0.0, 1.0);
    s.Albedo = vec3(0.0);
    s.Metallic = 0.0;
    s.Roughness = 1.0;
    s.ViewDepth = 0.0;
    s.PbrModel = 0;
    return s;
}

#endif // OLO_GBUFFER_RAY_SURFACE_GLSL
