// =============================================================================
// PBRCommon.glsl - Complete PBR shader include file
// Part of OloEngine Enhanced PBR System
// This file combines all PBR constants, functions, and lighting calculations
// =============================================================================

#ifndef PBR_GLSL
#define PBR_GLSL

// =============================================================================
// LIGHT TYPE CONSTANTS
// =============================================================================
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2
// Sphere area lights reuse the MultiLightData layout: SpotParams.z encodes the
// emitter sphere radius and the standard Position/Range fields drive distance
// falloff. Specular is shaded via the Karis 2013 representative-point trick;
// diffuse uses a solid-angle correction that collapses to a point light as
// the radius approaches zero.
#define SPHERE_AREA_LIGHT 3

// =============================================================================
// MATHEMATICAL CONSTANTS
// =============================================================================
#define PI 3.14159265359
#define TWO_PI 6.28318530718
#define HALF_PI 1.57079632679
#define INV_PI 0.31830988618
#define INV_TWO_PI 0.15915494309
#define EPSILON 0.0001
#define LARGE_EPSILON 0.001

// =============================================================================
// PBR MATERIAL CONSTANTS
// =============================================================================
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define MIN_ROUGHNESS 0.04  // Minimum roughness to avoid numerical issues
#define MAX_ROUGHNESS 1.0

// =============================================================================
// PBR CLOSURE MODEL SELECTOR (issue #975)
// =============================================================================
// Mirrors PBRModel in Renderer/PBRModel.h and the u_PBRModel lane of
// PBRMaterialProperties (UBO binding 2). Legacy is the default: existing
// materials keep the exact closure they shipped with, bit for bit; ClosureV2
// is an explicit per-material opt-in. See PBR CLOSURE V2 below.
#define OLO_PBR_MODEL_LEGACY 0
#define OLO_PBR_MODEL_CLOSURE_V2 1

// The G-Buffer RT2 alpha "MaterialFlags" lane, encoded here so its layout has
// ONE executable home shared by every G-Buffer writer (PBR_GBuffer{,_Skinned},
// VirtualGBufferFragment, VirtualVisibilityResolve) and one decode
// (DeferredLightingShared.glsl):
//
//   bit 0 — unlit (the overlay shaders write a literal 1.0; PBR writers 0)
//   bit 1 — PBR closure model. A SINGLE bit: PBRModel.h's numbering is
//           append-only, so a third model REQUIRES widening this lane in the
//           same change (the min() below otherwise truncates it to Legacy on
//           the deferred path only — a silent Forward/Deferred divergence).
//
// KNOWN LIMITATION: the lane is not average-safe. In the resolved-MSAA
// deferred mode (MSAA > 1 with per-sample lighting off — both non-default,
// session-local toggles) the resolve blit averages samples, and an averaged
// bitfield decodes wrongly at silhouettes (e.g. ClosureV2 2.0 over Legacy 0.0
// averages to the unlit code 1.0). The pre-#975 single-flag encoding had the
// milder variant of the same defect; fixing it means excluding the lane from
// the averaged resolve, which is deliberately out of this slice.
float oloEncodeGBufferPbrFlags(int pbrModel)
{
    return float(min(pbrModel, OLO_PBR_MODEL_CLOSURE_V2) * 2);
}

// =============================================================================
// RENDERING CONSTANTS
// =============================================================================
#define GAMMA 2.2
#define INV_GAMMA 0.45454545455
#define MAX_LIGHTS 256
#define MAX_BONES 100

// Tone mapping constants
#define TONEMAP_NONE 0
#define TONEMAP_REINHARD 1
#define TONEMAP_ACES 2
#define TONEMAP_UNCHARTED2 3

// =============================================================================
// TEXTURE BINDING CONSTANTS (from ShaderBindingLayout.h)
// =============================================================================
#define TEX_DIFFUSE 0
#define TEX_SPECULAR 1
#define TEX_NORMAL 2
#define TEX_HEIGHT 3
#define TEX_AMBIENT 4
#define TEX_EMISSIVE 5
#define TEX_ENVIRONMENT 9
#define TEX_USER_0 10  // Irradiance map
#define TEX_USER_1 11  // Prefilter map
#define TEX_USER_2 12  // BRDF LUT

// =============================================================================
// UNIFORM BUFFER BINDING CONSTANTS
// =============================================================================
#define UBO_CAMERA 0
#define UBO_LIGHTS 1
#define UBO_MATERIAL 2
#define UBO_MODEL 3
#define UBO_BONES 4
#define UBO_MULTI_LIGHTS 5

// =============================================================================
// QUALITY SETTINGS
// =============================================================================
#define IBL_SAMPLE_COUNT_LOW 256
#define IBL_SAMPLE_COUNT_MEDIUM 512
#define IBL_SAMPLE_COUNT_HIGH 1024
#define IBL_SAMPLE_COUNT_ULTRA 2048

// =============================================================================
// COLOR SPACE CONVERSION
// =============================================================================
#define SRGB_TO_LINEAR(color) pow(color, vec3(GAMMA))
#define LINEAR_TO_SRGB(color) pow(color, vec3(INV_GAMMA))

// =============================================================================
// UTILITY MACROS
// =============================================================================
#define SATURATE(x) clamp(x, 0.0, 1.0)
#define SQUARE(x) ((x) * (x))
#define MAX3(a, b, c) max(a, max(b, c))
#define MIN3(a, b, c) min(a, min(b, c))

// Shared Pow2/4/5, bitwise RadicalInverse_VdC, Hammersley, branchless
// OrthonormalBasis and ImportanceSampleGGX. PI is already defined above, so
// MathCommon's #ifndef guard leaves it alone. Bare path: nested includes
// resolve relative to this file's own directory (assets/shaders/include),
// matching LightProbeSampling.glsl's `#include "SphericalHarmonics.glsl"`.
#include "MathCommon.glsl"

// =============================================================================
// FRESNEL FUNCTIONS
// =============================================================================

// Schlick-Fresnel approximation. Pow5 (multiply chain) over pow(x, 5.0): this
// runs for every light at every lit pixel, so the avoided exp2/log2 pair is a
// real per-frame win, and the result is bit-near-identical for x in [0, 1].
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * Pow5(SATURATE(1.0 - cosTheta));
}

// Fresnel with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * Pow5(SATURATE(1.0 - cosTheta));
}

// (A spherical-Gaussian Fresnel approximation lived here. It was never wired up,
//  and now that Schlick uses the Pow5 multiply chain it would be slower — its
//  exp2 costs more than five MULs — so it was removed rather than kept as dead
//  code. See issue #262 — shader performance review.)

// =============================================================================
// DISTRIBUTION FUNCTIONS
// =============================================================================

// GGX/Trowbridge-Reitz normal distribution function.
// alpha = roughness^2 (`a`), and `a2` is alpha^2. This is the reference
// convention THE ALPHA LEDGER below records; every G in this file is stated
// relative to it.
float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / max(denom, EPSILON);
}

// Anisotropic GGX distribution.
// Takes ALPHAS, not roughnesses, despite the parameter names: `a2` here is
// alphaX * alphaY. Callers must square the roughness themselves. Deliberate
// exception to THE ALPHA LEDGER below; currently unreferenced.
float distributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B, float roughnessX, float roughnessY)
{
    float TdotH = dot(T, H);
    float BdotH = dot(B, H);
    float NdotH = dot(N, H);

    float a2 = roughnessX * roughnessY;
    vec3 v = vec3(roughnessY * TdotH, roughnessX * BdotH, a2 * NdotH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;

    return a2 * w2 * w2 * INV_PI;
}

// =============================================================================
// GEOMETRY FUNCTIONS
// =============================================================================
//
// -----------------------------------------------------------------------------
// THE ALPHA LEDGER — read before touching any D or G in this file (issue #904)
// -----------------------------------------------------------------------------
// GGX is parameterised by `alpha`, the microfacet slope distribution width.
// This engine's authoring parameter is `roughness`, and the mapping every
// function here agrees on is Burley's:
//
//     alpha = roughness * roughness
//
// A `D` and a `G` evaluated at DIFFERENT alphas describe two different
// surfaces. The result is plausible rather than obviously broken — no NaN, no
// black pixel, just a highlight subtly shaped for a surface nobody authored —
// which is exactly how the mismatch this ledger exists to prevent survived
// unnoticed. So every function below states its convention, and the two
// deliberate exceptions state WHY they differ instead of quietly differing:
//
//   distributionGGX                alpha = roughness^2   (a  = alpha, a2 = alpha^2)
//   distributionGGXUnclamped       alpha = roughness^2   (v2 closure + sampling
//                                  densities; NO denominator value clamp — the
//                                  v2 closure clamps ROUGHNESS instead, see the
//                                  PBR CLOSURE V2 section)
//   visibilitySmithGGXCorrelated   alpha = roughness^2   (a2 = alpha^2)
//   ggxSmithLambda / ggxVNDFWeight alpha = roughness^2   (see A NOTE ON ALPHA below)
//
//   geometrySchlickGGX / geometrySmith — DELIBERATE EXCEPTION.
//       Karis 2013's UE4 direct-lighting remap, k = (roughness + 1)^2 / 8.
//       This is NOT a third alpha convention: it is a closed-form
//       APPROXIMATION of the Smith G for the same alpha = roughness^2 surface,
//       with Disney's "reduce the hotness of direct lighting" adjustment folded
//       into the remap. Left as-is on purpose: `cookTorranceBRDF` — the BRDF
//       every shipping lit pass actually calls — uses it, it is mirrored in
//       Renderer/PathTracing/ReferenceBRDF.h so the offline reference tracer
//       integrates the same thing, and replacing it would move every lit golden
//       in the suite. That is a separate, deliberate decision, not part of #904.
//
//   distributionGGXAnisotropic — DELIBERATE EXCEPTION.
//       Takes ALPHAS directly despite its parameter names: its
//       `a2 = roughnessX * roughnessY` is alphaX * alphaY, so callers must pass
//       roughness^2 themselves. Currently has no callers.
// -----------------------------------------------------------------------------

// Smith's method for masking-shadowing function (single direction).
// Schlick-GGX with the UE4 direct-lighting k remap — a deliberate exception to
// the alpha ledger above; see it before "fixing" this.
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / max(denom, EPSILON);
}

// Smith's method considering both geometry obstruction and geometry shadowing.
// Separable (not height-correlated) form, built on the UE4 k remap above — the
// same deliberate exception to the alpha ledger.
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Height-correlated Smith VISIBILITY term for GGX (Heitz 2014, "Understanding
// the Masking-Shadowing Function", in the algebraic form Filament ships as
// V_SmithGGXCorrelated).
//
// Two things this returns that its old name `geometrySmithHeightCorrelated`
// did not say — both fixed under issue #904:
//
//  1. IT IS V, NOT G.  V = G2 / (4 * NdotV * NdotL): the 4*NdotV*NdotL from the
//     Cook-Torrance denominator is already folded in, and cancels analytically
//     against the sqrt terms. That cancellation is the entire point of the V
//     form — it removes the 0/0 at grazing incidence. A caller must therefore
//     write  D * V * F,  and must NOT divide by 4*NdotV*NdotL again. The old
//     name read as a G, and its one caller (cookTorranceBRDFEnhanced) duly
//     divided a second time.
//
//  2. alpha = roughness^2, matching distributionGGX. The previous version set
//     `a2 = roughness * roughness`, squaring roughness ONCE where the formula
//     wants alpha^2 = roughness^4.
//
//     Which direction that errs in is worth stating, because issue #904 states
//     it BACKWARDS and the wrong version is the intuitive one. `a2 = r^2`
//     means an effective alpha of r, while the correct alpha is r^2 — and
//     r > r^2 for every r in (0, 1). So the old G modelled a ROUGHER surface
//     than D did and masked MORE, not less. Correcting it makes those pixels
//     BRIGHTER.
//
//     The error is also not largest on rough surfaces. It scales with the
//     ratio r / r^2 = 1/r, so it peaks at LOW-to-mid roughness at grazing
//     angles: measured over a (roughness x NdotV x NdotL) sweep, the worst G2
//     discrepancy is 78% relative at roughness ~0.15 with both cosines near
//     grazing, against ~14% at roughness 0.8 head-on.
//
// Pinned two ways, because neither catches the other's failure:
//   * ReferenceBRDFTest.HeightCorrelatedVisibilityMatchesTheVndfLambda — an
//     exact algebraic identity against ggxSmithLambda (the Lambda the VNDF path
//     pairs with D). It holds for alpha = roughness^2 and no other convention,
//     so reintroducing the single-square fails it at every roughness.
//   * ReferenceBRDFGpuParity — this compiled GLSL against the C++ mirror in
//     Renderer/PathTracing/ReferenceBRDF.h, so the two cannot drift.
float visibilitySmithGGXCorrelated(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);

    return 0.5 / max(GGXV + GGXL, EPSILON);
}

// =============================================================================
// BRDF CALCULATIONS
// =============================================================================

// Cook-Torrance BRDF implementation
vec3 cookTorranceBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);

    // Calculate F0 based on metallic workflow
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    // Calculate the three components of the BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    // Calculate specular BRDF
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON;
    vec3 specular = numerator / denominator;

    // Calculate diffuse contribution
    vec3 kS = F; // Energy of light that gets reflected
    vec3 kD = vec3(1.0) - kS; // Remaining energy for refraction
    kD *= 1.0 - metallic; // Metallic materials don't refract light

    return kD * albedo * INV_PI + specular;
}

// Enhanced BRDF using the height-correlated Smith visibility term.
//
// NOTE: currently has no callers — its only caller,
// calculateLightContributionEnhanced below, is itself unreferenced. The
// shipping lit passes all call cookTorranceBRDF above. Kept (and made correct)
// rather than deleted so that wiring it up is a one-line change that does not
// also have to re-derive the math; that wiring is deliberately NOT part of
// issue #904, because it would move every lit golden in the suite.
vec3 cookTorranceBRDFEnhanced(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);

    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    float NDF = distributionGGX(N, H, roughness);
    // Vis is the VISIBILITY term: G2 / (4 * NdotV * NdotL), with the
    // Cook-Torrance denominator already folded in. Multiply, do not divide
    // again — see visibilitySmithGGXCorrelated.
    float Vis = visibilitySmithGGXCorrelated(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = NDF * Vis * F;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    return kD * albedo * INV_PI + specular;
}

// =============================================================================
// NORMAL MAPPING UTILITIES
// =============================================================================

// -----------------------------------------------------------------------------
// SINGLE SOURCE OF TRUTH for normal mapping. Every path that consumes a normal
// map goes through decodeTangentNormal + applyNormalMapTBN below:
//
//   * classic forward/deferred and the virtual-geometry HARDWARE raster
//     (VirtualMeshGBuffer.glsl)      -> getNormalFromMap      (dFdx/dFdy derivatives)
//   * the virtual-geometry SOFTWARE raster's visibility resolve
//     (VirtualVisibilityResolve.glsl)-> getNormalFromMapGrad  (analytic derivatives)
//
// The resolve pass is a FULLSCREEN draw, so it cannot use dFdx: neighbouring
// pixels can belong to a different triangle, a different cluster, even a
// different instance. It therefore passes explicit per-pixel screen-space
// derivatives of world position and texcoord — the same quantities dFdx/dFdy
// would produce — instead of re-deriving a tangent frame of its own.
//
// It used to re-derive one, and it had DRIFTED from this file in two independent
// ways: it sampled z from the blue channel (re-introducing the #440 BC5/RGTC
// inversion for software-rasterized clusters) and it used B = +cross(N,T), i.e.
// the opposite handedness. Both halves of a single virtual-geometry frame shaded
// the same material differently. Keeping ONE decode and ONE TBN construction here
// is what makes that class of bug impossible rather than merely fixed.
// -----------------------------------------------------------------------------

// Decode a tangent-space normal from a normal-map sample's xy.
//
// Reconstruct z from xy rather than sampling the blue channel. A tangent-space
// unit normal always has z = sqrt(1 - x^2 - y^2), so this is correct for ordinary
// 3-channel (RGB) normal maps AND required for 2-channel BC5/RGTC2 normal maps,
// whose blue channel is 0 on the GPU (sampling it would give z = -1 and invert the
// normal). Reconstruction is done after the xy intensity scale so the result stays
// unit length. (#440)
vec3 decodeTangentNormal(vec2 sampledXY, float normalScale)
{
    vec2 nxy = sampledXY * 2.0 - 1.0;
    nxy *= normalScale;
    float nz = sqrt(max(0.0, 1.0 - min(1.0, dot(nxy, nxy))));
    return vec3(nxy, nz);
}

// The epsilon every degeneracy guard below tests a SQUARED length against.
// 1e-20 => a vector shorter than 1e-10, which is where fp32 normalisation stops
// being meaningful (the squared length underflows to a denormal at ~1e-19).
const float kDegenerateEpsilon = 1e-20;

// The interpolated vertex normal, made SAFE to normalize.
//
// `normalize(normal)` is NaN for a zero-length input, and NaN in => NaN out for a
// non-finite one. Both inputs occur in real imported data: a zero-area triangle has
// no face normal for the generator to accumulate, a vertex whose smooth normals
// cancel sums to zero, and an importer can hand us a NaN outright. MeshOptimization
// DETECTS zero-area triangles and deliberately KEEPS them (they carry real 3D area),
// so the renderer is the thing that has to cope. A NaN normal is written into the
// octahedral G-Buffer and resolves in deferred lighting as a blown-out white pixel —
// the same failure the UV-degenerate guard below fixes, one line higher up.
//
// Fallback: the GEOMETRIC normal of the surface, rebuilt from the screen-space
// position derivatives. cross(dP/dx, dP/dy) is the plane's normal and, with GL's
// lower-left window origin, points TOWARD the viewer — so a fragment we are looking
// at gets a normal facing us, which is the only defensible answer for a surface whose
// authored normal carries no direction at all. It is exact for a flat triangle and
// correct to first order otherwise; unlike a constant it stays orientation-aware, and
// unlike the raw input it is always finite and unit-length. If the position gradient
// is degenerate too (a fully collapsed fragment) there is nothing left to derive, so
// return a fixed axis: still wrong, but finite — a NaN would poison every downstream
// channel, a unit vector only mis-shades one pixel.
//
// Guards are written `!(x > eps)` rather than `x < eps` so a NaN also takes the
// fallback (every comparison against NaN is false).
// Pinned by PbrNormalMapTest.ZeroAndNaNVertexNormalsFallBackToTheGeometricNormal.
vec3 sanitizeSurfaceNormal(vec3 normal, vec3 dpdx, vec3 dpdy)
{
    float nLenSq = dot(normal, normal);
    if (nLenSq > kDegenerateEpsilon)
        return normal * inversesqrt(nLenSq);

    vec3 gRaw = cross(dpdx, dpdy);
    float gLenSq = dot(gRaw, gRaw);
    if (!(gLenSq > kDegenerateEpsilon))
        return vec3(0.0, 0.0, 1.0);

    return gRaw * inversesqrt(gLenSq);
}

// Rotate a tangent-space normal into world space with a TBN built from the
// surface's SCREEN-SPACE derivatives: dpdx/dpdy = d(worldPos)/d(x,y),
// duvdx/duvdy = d(texCoord)/d(x,y). Any consistent pair works, but they must be
// screen-space (not triangle-edge) derivatives: the sign of the UV Jacobian
// determinant — which fixes the tangent's handedness — flips with the triangle's
// screen-space winding, so a back face of a two-sided material has to see the
// flipped basis exactly as the hardware rasterizer's dFdx does.
vec3 applyNormalMapTBN(vec3 tangentNormal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec3 normal)
{
    // NOT normalize(normal): that is NaN for a zero/NaN input normal, and the NaN
    // would flow straight through the cross products below into the G-Buffer.
    vec3 N = sanitizeSurfaceNormal(normal, dpdx, dpdy);

    // A UV-DEGENERATE triangle — one whose corners share a texcoord, so the
    // interpolated UV is constant and both uv derivatives are zero — makes the
    // derivative tangent collapse to the zero vector. normalize(vec3(0)) is NaN,
    // and that NaN poisons the TBN, the returned normal, the G-Buffer it is
    // written to, and finally the deferred lighting, which resolves it as a
    // blown-out white pixel. Real assets have these: Sponza's mesh ships 314 of
    // them (zero UV area, non-zero 3D area), and mesh simplification — the
    // Nanite-style cluster LOD DAG (issue #629) — produces more, which is what
    // drew a white lacework along every leaf silhouette of the potted vines.
    //
    // With no UV gradient there is no tangent frame to rotate the tangent-space
    // normal by, so the only meaningful answer is the geometric normal. Same for
    // a tangent that comes out parallel to N (the cross product then collapses).
    // Guard with `!(x > eps)` rather than `x < eps` so a NaN derivative — the
    // other way this math can go bad — also takes the fallback.
    // Pinned by PbrNormalMapTest.DegenerateUvGradientFallsBackToGeometricNormal.
    //
    // NOT guarded, deliberately: the UV-COLLINEAR triangle (three DISTINCT texcoords
    // that are collinear in UV space, so the UV Jacobian determinant is zero but
    // neither derivative is). It is the MAJORITY case in real data — 234 of Sponza's
    // 314 UV-degenerate triangles — and it produces NO NaN: tRaw is non-zero, so the
    // code below builds a perfectly finite, unit-length, in-plane tangent. It is just
    // an ARBITRARY one (the true dP/du does not exist when u is constant along the
    // triangle), and being derivative-based it is camera-dependent, so the perturbation
    // swims as the view rotates. Falling back to N there would need a threshold on the
    // determinant, and the only scale-invariant form of that test — |det| small
    // relative to |duvdx||duvdy| — is ALSO small for a legitimately mapped surface seen
    // at a grazing angle, where the two UV gradients are near-parallel. That would kill
    // normal mapping at glancing incidence: a visible, global regression traded for a
    // bounded, local one. Detecting it needs per-vertex tangents (mesh-space, not
    // screen-space), which is a different fix. Covered as a finite/unit-length contract by
    // quadrant C of PbrNormalMapTest.ZeroAndNaNVertexNormalsFallBackToTheGeometricNormal.
    vec3 tRaw = dpdx * duvdy.t - dpdy * duvdx.t;
    float tLenSq = dot(tRaw, tRaw);
    if (!(tLenSq > kDegenerateEpsilon))
        return N;

    vec3 T = tRaw * inversesqrt(tLenSq);
    vec3 bRaw = cross(N, T);
    float bLenSq = dot(bRaw, bRaw);
    if (!(bLenSq > kDegenerateEpsilon))
        return N;

    vec3 B = -bRaw * inversesqrt(bLenSq);
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * tangentNormal);
}

// Get normal from normal map using the derivative method (hardware-rasterized
// fragments: dFdx/dFdy are available and address the real neighbouring pixels).
vec3 getNormalFromMap(sampler2D normalMap, vec2 texCoords, vec3 worldPos, vec3 normal, float normalScale)
{
    vec3 tangentNormal = decodeTangentNormal(texture(normalMap, texCoords).xy, normalScale);
    return applyNormalMapTBN(tangentNormal,
                             dFdx(worldPos), dFdy(worldPos),
                             dFdx(texCoords), dFdy(texCoords),
                             normal);
}

// Same, for callers that must supply their derivatives explicitly (the software
// rasterizer's fullscreen visibility resolve). The texture sample uses the same
// uv gradients as the TBN, so mip selection matches the hardware path too.
vec3 getNormalFromMapGrad(sampler2D normalMap, vec2 texCoords,
                          vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy,
                          vec3 normal, float normalScale)
{
    vec3 tangentNormal = decodeTangentNormal(textureGrad(normalMap, texCoords, duvdx, duvdy).xy, normalScale);
    return applyNormalMapTBN(tangentNormal, dpdx, dpdy, duvdx, duvdy, normal);
}

// =============================================================================
// COLOR UTILITIES
// =============================================================================

// Linear to sRGB conversion (accurate)
vec3 linearToSRGB(vec3 color)
{
    return pow(color, vec3(INV_GAMMA));
}

// sRGB to linear conversion (accurate)
vec3 sRGBToLinear(vec3 color)
{
    return pow(color, vec3(GAMMA));
}

// ACES tone mapping
vec3 acesToneMapping(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;

    return SATURATE((color * (a * color + b)) / (color * (c * color + d) + e));
}

// Reinhard tone mapping
vec3 reinhardToneMapping(vec3 color)
{
    return color / (color + vec3(1.0));
}

// Uncharted 2 tone mapping
vec3 uncharted2ToneMapping(vec3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;

    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

// Unified post-processing function - tone mapping + gamma correction in one pass
// This prevents redundant gamma correction by combining operations
vec3 postProcessColor(vec3 hdrColor, int tonemapOperator, bool applyGamma)
{
    vec3 toneMappedColor;

    // Apply tone mapping
    switch (tonemapOperator)
    {
        case TONEMAP_REINHARD:
            toneMappedColor = reinhardToneMapping(hdrColor);
            break;
        case TONEMAP_ACES:
            toneMappedColor = acesToneMapping(hdrColor);
            break;
        case TONEMAP_UNCHARTED2:
            toneMappedColor = uncharted2ToneMapping(hdrColor);
            break;
        default: // TONEMAP_NONE
            toneMappedColor = SATURATE(hdrColor);
            break;
    }

    // Apply gamma correction only if requested (prevents double application)
    if (applyGamma) {
        return linearToSRGB(toneMappedColor);
    }

    return toneMappedColor;
}

// =============================================================================
// SAMPLING UTILITIES
// =============================================================================

// camelCase aliases over the shared MathCommon primitives, kept so existing
// call sites (e.g. calculateIBLImportanceSampled) compile unchanged. The
// canonical bodies — bitwise radical inverse and the branchless orthonormal
// basis — now live in MathCommon.glsl, so they can't drift from the bake path.
float vanDerCorputSequence(uint bits)               { return RadicalInverse_VdC(bits); }
vec2  hammersleySequence(uint i, uint N)            { return Hammersley(i, N); }
vec3  importanceSampleGGX(vec2 Xi, vec3 N, float r) { return ImportanceSampleGGX(Xi, N, r); }

// =============================================================================
// GGX VNDF IMPORTANCE SAMPLING (issue #706)
//
// Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018.
//
// importanceSampleGGX above samples the FULL normal distribution D. At grazing
// angles most of the microfacets it draws are backfacing or masked, so their
// contribution is thrown away — the sample is wasted and the variance stays
// high exactly where a rough surface is most visible. Sampling the VISIBLE
// normal distribution D_vis(H) = G1(V) * max(0, V.H) * D(H) / (N.V) instead
// only ever draws microfacets the viewer can actually see.
//
// ---------------------------------------------------------------------------
// THE WEIGHT IS NOT OPTIONAL, AND OMITTING IT FAILS SILENTLY
// ---------------------------------------------------------------------------
// With VNDF sampling the specular estimator collapses to
//
//     f * cos(theta_L) / pdf  ==  F * (G2 / G1)
//
// so a VNDF-sampled ray must be weighted by ggxVNDFWeight(). Drop it and every
// sample is weighted 1 instead of G2/G1 <= 1, which OVERESTIMATES the shadowed
// portion of the lobe. Nothing throws, no test that only checks "is the image
// plausible" notices, and the resulting image is believably lit and
// permanently, quietly wrong — brighter than it should be, most at grazing
// angles. Pinned by StochasticSamplerTest.VndfEstimatorMatchesBruteForce,
// which also asserts the unweighted form FAILS the same comparison.
//
// ---------------------------------------------------------------------------
// A NOTE ON ALPHA
// ---------------------------------------------------------------------------
// These functions use alpha = roughness * roughness, matching distributionGGX
// above (whose `a` is roughness*roughness and `a2` its square). The Lambda
// below is the one that pairs with D, which is what unbiasedness requires.
//
// This block used to record a divergence: geometrySmithHeightCorrelated
// squared roughness only once, so the engine's D and G described different
// surfaces, and that was noted here rather than fixed because it was believed
// to move every lit golden. Issue #904 resolved it — the mismatched function
// turned out to be on an unreferenced path, so nothing rendered had to change.
// It is now visibilitySmithGGXCorrelated and agrees with the alpha used here;
// the exact identity between the two is asserted by
// ReferenceBRDFTest.HeightCorrelatedVisibilityMatchesTheVndfLambda. See THE
// ALPHA LEDGER in the GEOMETRY FUNCTIONS section for the whole picture.
// =============================================================================

// Smith's Lambda for GGX, from the cosine with the (macro)surface normal.
float ggxSmithLambda(float NdotX, float alpha)
{
    float c = clamp(abs(NdotX), 1.0e-4, 1.0);
    float c2 = c * c;
    float tan2 = (1.0 - c2) / c2;
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

// The VNDF sample weight G2/G1 (Heitz 2018 eq. 20), height-correlated Smith.
// Multiply the sampled direction's radiance by this. Returns 0 below the
// horizon, where the sample carries no energy.
float ggxVNDFWeight(float NdotV, float NdotL, float roughness)
{
    if (NdotL <= 0.0 || NdotV <= 0.0)
        return 0.0;

    float alpha = roughness * roughness;
    float lambdaV = ggxSmithLambda(NdotV, alpha);
    float lambdaL = ggxSmithLambda(NdotL, alpha);
    return (1.0 + lambdaV) / (1.0 + lambdaV + lambdaL);
}

// Sample a visible microfacet normal in TANGENT space (z = macrosurface normal).
// `Ve` is the view direction in the same space, pointing AWAY from the surface.
// Heitz 2018, section 3.2 — the reference listing, transcribed.
vec3 sampleGGXVNDFTangent(vec3 Ve, float alphaX, float alphaY, vec2 Xi)
{
    // 1. Stretch the view direction so the ellipsoid becomes a hemisphere.
    vec3 Vh = normalize(vec3(alphaX * Ve.x, alphaY * Ve.y, Ve.z));

    // 2. Orthonormal basis around Vh (degenerate when Vh is the pole).
    float lenSq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = (lenSq > 0.0) ? (vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lenSq)) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);

    // 3. Uniform point on the projected area: a disk, with the half below the
    //    horizon squashed so it lands inside the visible hemisphere's silhouette.
    float r = sqrt(Xi.x);
    float phi = TWO_PI * Xi.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;

    // 4. Lift back onto the hemisphere, then unstretch to the ellipsoid.
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(vec3(alphaX * Nh.x, alphaY * Nh.y, max(0.0, Nh.z)));
}

// World-space wrapper: returns a visible half-vector around N for view dir V.
// Isotropic; pass roughness, not alpha.
vec3 sampleGGXVNDF(vec3 N, vec3 V, float roughness, vec2 Xi)
{
    vec3 tangent;
    vec3 bitangent;
    OrthonormalBasis(N, tangent, bitangent);

    vec3 Ve = vec3(dot(V, tangent), dot(V, bitangent), dot(V, N));
    float alpha = roughness * roughness;
    vec3 H = sampleGGXVNDFTangent(Ve, alpha, alpha, Xi);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// =============================================================================
// PBR CLOSURE V2 (issue #975)
// =============================================================================
//
// A VERSIONED closure. cookTorranceBRDF above is the Legacy model and is
// frozen: every existing material, scene and golden keeps it bit for bit.
// ClosureV2 is the explicit opt-in (Material::SetPBRModel, the u_PBRModel UBO
// lane, ReferenceMaterial::Model) and differs from Legacy in exactly four
// documented ways:
//
//   1. ONE geometry term. The specular lobe is D * V * F with the
//      height-correlated Smith VISIBILITY term (visibilitySmithGGXCorrelated).
//      The Schlick-GGX k remap stays reachable only through Legacy.
//   2. Near-mirror handling clamps ALPHA, not the denominator.
//      closureV2Roughness floors perceptual roughness at MIN_ROUGHNESS, so
//      alpha >= MIN_ROUGHNESS^2 and the unclamped NDF is finite everywhere.
//      The lobe NARROWS and BRIGHTENS toward a mirror as authored roughness
//      approaches 0, instead of collapsing toward black the way Legacy's
//      max(denom, EPSILON) makes it (ReferenceBRDF.h's DistributionGGX notes
//      quantify that collapse).
//   3. Multiple-scattering energy compensation (Kulla & Conty 2017), driven
//      by the generated tables in PBRClosureV2Energy.glsl. Reciprocal by
//      construction, and closes the white furnace exactly for F0 = 1.
//   4. Because evaluation alpha is clamped and the denominator is not, ONE D
//      serves Evaluate, Sample and Pdf. The three cannot disagree about the
//      lobe — the property a path tracer / ReSTIR estimator depends on, and
//      the reason Legacy needed two Ds (see DistributionGGXSamplingDensity in
//      ReferenceBRDF.h: a PDF describes the SAMPLER, never the integrand).
//
// The Evaluate / Sample / Pdf triple is mirrored in C++ and pinned:
//   Renderer/PathTracing/ReferenceBRDF.h   — function-for-function twins
//   Renderer/PathTracing/PBRClosureBSDF.h  — the versioned dispatch the
//                                            reference path tracer integrates
//   ReferenceBRDFGpuParityTest (GPU) and the ClosureV2 tests (headless).
//
// Deliberate approximations, recorded so they are decisions rather than
// surprises (each keeps Legacy behaviour on that sub-path, not a regression):
//   * Sphere-AREA lights shade v2 materials through the Legacy
//     representative-point evaluator — the Karis normalization rescales D in
//     a way that has no v2 derivation yet.
//   * IBL / ambient stays on the split-sum LUT with no multi-scatter term;
//     energy compensation applies to punctual direct lighting and the CPU
//     reference tracer in this slice.
//   * The diffuse split keeps the (1 - F(H)) * (1 - metallic) Lambert term,
//     documented as THE energy split. F at the half vector is symmetric in
//     wo/wi, so the v2 closure is reciprocal.

#include "PBRClosureV2Energy.glsl"

// v2 perceptual-roughness clamp — the ONLY roughness guard in the v2 closure.
// Applied identically before evaluating, sampling and computing the density.
float closureV2Roughness(float roughness)
{
    return clamp(roughness, MIN_ROUGHNESS, MAX_ROUGHNESS);
}

// The TRUE (unclamped) GGX NDF on a cosine — GLSL twin of ReferenceBRDF.h's
// DistributionGGXSamplingDensity. alpha = roughness^2 per THE ALPHA LEDGER.
// The denominator floor is a denormal guard, not a value clamp; with the v2
// alpha floor it never engages.
float distributionGGXUnclamped(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float c = max(NdotH, 0.0);
    float denom = (c * c * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 1.17549435e-38);
}

// Bilinear lookup into the generated 16x16 single-scatter energy-LOSS table
// (1 - Ess). Cell-centered grid; clamped at the edges, never extrapolates.
// `roughness` is the AUTHORED perceptual roughness (the table rows bake the
// v2 alpha clamp in, so callers pass it un-floored). Entries decode from the
// half-packed constants in PBRClosureV2Energy.glsl — see that file's header
// for why the packing itself is load-bearing (NVIDIA C5025 at link).
float ggxEnergyLoss(float mu, float roughness)
{
    float fs = float(OLO_GGX_ENERGY_TABLE_SIZE);
    float x = clamp(clamp(mu, 0.0, 1.0) * fs - 0.5, 0.0, fs - 1.0);
    float y = clamp(clamp(roughness, 0.0, 1.0) * fs - 0.5, 0.0, fs - 1.0);
    int x0 = int(floor(x));
    int y0 = int(floor(y));
    int x1 = min(x0 + 1, OLO_GGX_ENERGY_TABLE_SIZE - 1);
    int y1 = min(y0 + 1, OLO_GGX_ENERGY_TABLE_SIZE - 1);
    float fx = x - float(x0);
    float fy = y - float(y0);
    float v00 = ggxEnergyLossEntry(y0 * OLO_GGX_ENERGY_TABLE_SIZE + x0);
    float v10 = ggxEnergyLossEntry(y0 * OLO_GGX_ENERGY_TABLE_SIZE + x1);
    float v01 = ggxEnergyLossEntry(y1 * OLO_GGX_ENERGY_TABLE_SIZE + x0);
    float v11 = ggxEnergyLossEntry(y1 * OLO_GGX_ENERGY_TABLE_SIZE + x1);
    return mix(mix(v00, v10, fx), mix(v01, v11, fx), fy);
}

// Linear lookup of 1 - E_avg(roughness) over the same cell-centered axis.
float ggxEnergyLossAverage(float roughness)
{
    float fs = float(OLO_GGX_ENERGY_TABLE_SIZE);
    float y = clamp(clamp(roughness, 0.0, 1.0) * fs - 0.5, 0.0, fs - 1.0);
    int y0 = int(floor(y));
    int y1 = min(y0 + 1, OLO_GGX_ENERGY_TABLE_SIZE - 1);
    float fy = y - float(y0);
    return mix(ggxEnergyLossAvgEntry(y0), ggxEnergyLossAvgEntry(y1), fy);
}

// Kulla-Conty multiple-scattering compensation lobe:
//
//     f_ms = F_ms * (1 - Ess(NdotV)) * (1 - Ess(NdotL)) / (pi * (1 - E_avg))
//     F_ms = F_avg^2 * E_avg / (1 - F_avg * (1 - E_avg)),  F_avg = F0 + (1-F0)/21
//
// Symmetric in NdotV/NdotL, so it preserves reciprocity. With F_avg == 1 the
// hemispherical integral of f_ms * cos is exactly 1 - Ess(NdotV): the white
// furnace closes to 1 analytically, which is what the furnace test asserts.
vec3 closureV2MultiScatter(float NdotV, float NdotL, float roughness, vec3 F0)
{
    float lossAvg = ggxEnergyLossAverage(roughness);
    // Below the table's resolution the lobe is near-mirror and sheds nothing
    // worth compensating; this also guards the 1/lossAvg denominator.
    if (lossAvg < 1.0e-4)
        return vec3(0.0);

    float lossV = ggxEnergyLoss(NdotV, roughness);
    float lossL = ggxEnergyLoss(NdotL, roughness);
    float eAvg = 1.0 - lossAvg;

    vec3 fAvg = F0 + (vec3(1.0) - F0) * (1.0 / 21.0);
    vec3 fresnelMs = fAvg * fAvg * eAvg / (vec3(1.0) - fAvg * lossAvg);

    return fresnelMs * (lossV * lossL) / (PI * lossAvg);
}

// v2 Evaluate: f(V, L) WITHOUT the cosine term, matching cookTorranceBRDF's
// convention — callers multiply by NdotL.
vec3 closureV2Evaluate(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    float r = closureV2Roughness(roughness);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    float D = distributionGGXUnclamped(NdotH, r);
    float Vis = visibilitySmithGGXCorrelated(N, V, L, r);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    // The energy lookup takes the AUTHORED roughness per its contract — the
    // table rows bake the v2 clamp in, so row 0 IS the r = 0.04 row and
    // passing the clamped r here would double-apply the clamp (14% into row 1
    // for authored r < 0.04). C++ twin agrees (ClosureV2Evaluate).
    vec3 specular = D * Vis * F + closureV2MultiScatter(NdotV, NdotL, roughness, F0);

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return kD * albedo * INV_PI + specular;
}

// The versioned dispatch every model-aware call site routes through. Legacy
// materials take the EXACT cookTorranceBRDF path — the branch is on a
// per-draw uniform, so existing pixels cannot move.
vec3 evaluatePBRClosure(int pbrModel, vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    if (pbrModel == OLO_PBR_MODEL_CLOSURE_V2)
        return closureV2Evaluate(N, V, L, albedo, metallic, roughness);
    return cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
}

// ---------------------------------------------------------------------------
// v2 Sample / Pdf — the other two thirds of the contract triple. No raster
// pass calls these yet; they exist so a GPU path tracer / ReSTIR pass
// inherits a sampler and density that already agree with Evaluate. Coverage
// is honest about its shape: closureV2Pdf and closureV2Evaluate are
// parity-tested texel-for-texel against the C++ twins
// (ClosureV2GpuParityTest); closureV2SampleBRDF is compile-covered through
// that probe's include and its DENSITY is the pinned closureV2Pdf, but the
// draw itself runs for the first time in its first consumer — extend the
// parity probe with a sampled-tuple channel in that PR.
// ---------------------------------------------------------------------------

// Rec. 709 luminance — twin of ReferenceBRDF.h's Luminance.
float closureV2Luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Lobe selection probability — twin of ReferenceBRDF.h's
// SpecularLobeProbability (see its comment for why the clamp to [0.1, 0.9]
// is load-bearing: a zero probability on a lobe that carries energy is the
// classic silent bias of one-sample mixture estimators).
float closureV2SpecularProbability(vec3 albedo, float metallic)
{
    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    vec3 diffuse = albedo * (1.0 - metallic);
    float specularWeight = closureV2Luminance(F0);
    float diffuseWeight = closureV2Luminance(diffuse);
    float total = specularWeight + diffuseWeight;
    if (!(total > 0.0))
        return 0.5;
    return clamp(specularWeight / total, 0.1, 0.9);
}

// Cosine-weighted hemisphere sample about N (Malley's method) — twin of
// ReferenceBRDF.h's CosineSampleHemisphere. Body-identical to
// StochasticCommon.glsl's OloCosineHemisphere, which this file CANNOT call
// (StochasticCommon includes PBRCommon, not the reverse); if the shared
// sampler ever moves to MathCommon.glsl, delete this copy and call it — and
// keep StochasticCommon's stratification contract in mind (Xi.x drives the
// radius and must be the stratified component).
vec3 closureV2CosineSampleHemisphere(vec2 Xi, vec3 N)
{
    float r = sqrt(max(Xi.x, 0.0));
    float phi = TWO_PI * Xi.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - Xi.x));

    vec3 tangent;
    vec3 bitangent;
    OrthonormalBasis(N, tangent, bitangent);
    return normalize(tangent * x + bitangent * y + N * z);
}

// v2 Pdf: the density of closureV2SampleBRDF below, and nothing else. The
// specular term is the VNDF density through the reflection Jacobian,
//     pdf(L) = G1(NdotV) * D(NdotH) / (4 * NdotV),
// on the SAME clamped-alpha D that Evaluate uses. Directions below the
// horizon report 0, matching the Legacy BsdfPdf convention.
float closureV2Pdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    float NdotL = dot(N, L);
    if (NdotL <= 0.0)
        return 0.0;

    float r = closureV2Roughness(roughness);
    float alpha = r * r;
    float pSpecular = closureV2SpecularProbability(albedo, metallic);
    float pdfDiffuse = NdotL * INV_PI;

    float pdfSpecular = 0.0;
    float NdotV = dot(N, V);
    if (NdotV > 0.0)
    {
        vec3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);
        float G1V = 1.0 / (1.0 + ggxSmithLambda(NdotV, alpha));
        pdfSpecular = G1V * distributionGGXUnclamped(NdotH, r) / (4.0 * NdotV);
    }

    return pSpecular * pdfSpecular + (1.0 - pSpecular) * pdfDiffuse;
}

struct ClosureV2Sample
{
    vec3 L;       // sampled direction
    vec3 Value;   // full closureV2Evaluate(V, L); cosine NOT included
    float Pdf;    // the mixture density; closureV2Pdf(L) returns the same number
};

// v2 Sample: one-sample mixture over the VNDF specular lobe and the cosine
// diffuse lobe. Always evaluates the FULL closure and reports the COMBINED
// density — the standard unbiased one-sample MIS estimator.
//
// FAILURE CONVENTION (differs from the C++ twin, on purpose — GLSL has no
// out-bool ergonomics): a below-horizon draw comes back with Pdf == 0 and
// Value == vec3(0). The consumer MUST treat Pdf <= 0 as a terminated path
// and never form Value / Pdf there — 0/0 is NaN, and one NaN in an
// accumulation buffer spreads through temporal reuse. The C++ twin
// (BSDF::Sample) expresses the same event as `return false`, so its
// integrator structurally cannot divide; a GLSL consumer must write the
// guard itself.
ClosureV2Sample closureV2SampleBRDF(vec3 N, vec3 V, vec3 albedo, float metallic,
                                    float roughness, float lobeXi, vec2 Xi)
{
    ClosureV2Sample s;
    float pSpecular = closureV2SpecularProbability(albedo, metallic);
    if (lobeXi < pSpecular)
    {
        // The VNDF sampler is defined only for views ABOVE the surface; a
        // backfacing shading normal would feed it garbage while closureV2Pdf
        // reports zero specular density. Terminate via the documented
        // Pdf <= 0 convention (C++ twin returns false here).
        if (dot(N, V) <= 0.0)
        {
            s.L = vec3(0.0, 0.0, 1.0);
            s.Value = vec3(0.0);
            s.Pdf = 0.0;
            return s;
        }
        vec3 H = sampleGGXVNDF(N, V, closureV2Roughness(roughness), Xi);
        s.L = reflect(-V, H);
    }
    else
    {
        s.L = closureV2CosineSampleHemisphere(Xi, N);
    }
    s.Pdf = closureV2Pdf(N, V, s.L, albedo, metallic, roughness);
    s.Value = (s.Pdf > 0.0) ? closureV2Evaluate(N, V, s.L, albedo, metallic, roughness)
                            : vec3(0.0);
    return s;
}

// =============================================================================
// LIGHT DATA STRUCTURE
// =============================================================================

struct LightData {
    vec4 position;         // Position in world space (w = light type)
    vec4 direction;        // Direction for directional/spot lights
    vec4 color;            // Light color and intensity (w = intensity)
    vec4 attenuationParams; // (constant, linear, quadratic, range)
    vec4 spotParams;       // (inner_cutoff, outer_cutoff, falloff, enabled)
};

// =============================================================================
// ATTENUATION FUNCTIONS
// =============================================================================

// Calculate physically-based attenuation for point and spot lights
float calculateAttenuation(vec3 lightPos, vec3 fragPos, vec4 attenuationParams)
{
    float distance = length(lightPos - fragPos);
    float range = attenuationParams.w;

    // Early exit if beyond range
    if (distance > range) return 0.0;

    float constant = attenuationParams.x;
    float linear = attenuationParams.y;
    float quadratic = attenuationParams.z;

    // Standard attenuation formula
    float attenuation = 1.0 / (constant + linear * distance + quadratic * (distance * distance));

    // Smooth cutoff at range boundary
    float falloff = SATURATE(1.0 - Pow4(distance / range));
    return attenuation * falloff * falloff;
}

// Epic Games' physically-based attenuation
float calculateAttenuationEpic(vec3 lightPos, vec3 fragPos, float lightRadius)
{
    float distance = length(lightPos - fragPos);
    float falloff = SQUARE(SATURATE(1.0 - SQUARE(SQUARE(distance / lightRadius))));
    return falloff / (SQUARE(distance) + 1.0);
}

// =============================================================================
// SPOT LIGHT FUNCTIONS
// =============================================================================

// Calculate spot light intensity with smooth falloff
float calculateSpotIntensity(vec3 lightDir, vec3 spotDir, vec4 spotParams)
{
    float innerCutoff = spotParams.x;
    float outerCutoff = spotParams.y;

    float theta = dot(lightDir, normalize(-spotDir));
    float epsilon = innerCutoff - outerCutoff;
    float intensity = SATURATE((theta - outerCutoff) / epsilon);

    // Smooth falloff function
    return intensity * intensity;
}

// Advanced spot light with custom falloff curve
float calculateSpotIntensityAdvanced(vec3 lightDir, vec3 spotDir, vec4 spotParams)
{
    float innerCutoff = spotParams.x;
    float outerCutoff = spotParams.y;
    float falloffExponent = spotParams.z;

    float theta = dot(lightDir, normalize(-spotDir));

    if (theta > innerCutoff)
        return 1.0;
    else if (theta > outerCutoff)
    {
        float t = (theta - outerCutoff) / (innerCutoff - outerCutoff);
        return pow(t, falloffExponent);
    }

    return 0.0;
}

// =============================================================================
// AREA LIGHT FUNCTIONS
// =============================================================================

// Representative point technique for area lights
vec3 calculateAreaLightContribution(vec3 N, vec3 V, vec3 lightPos, vec3 lightSize,
                                   vec3 albedo, float metallic, float roughness, vec3 worldPos)
{
    vec3 centerToRay = dot(lightPos - worldPos, N) * N - (lightPos - worldPos);
    vec3 closestPoint = lightPos + centerToRay * SATURATE(length(centerToRay) / lightSize.x);

    vec3 L = normalize(closestPoint - worldPos);
    float distance = length(closestPoint - worldPos);

    // Use standard BRDF calculation
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    // Area light attenuation
    float attenuation = 1.0 / (distance * distance + 1.0);

    return brdf * attenuation;
}

// =============================================================================
// SPHERE AREA LIGHT (Karis 2013 representative point)
// =============================================================================

// Compute the representative light direction for a sphere area light's specular
// term. Conceptually: find the point on the emitter sphere that the surface's
// reflection ray would hit, then shade as if the light were that point.
// fragPos     — surface position (world space)
// N           — surface normal (world space)
// V           — view vector (world space, from surface to camera)
// lightPos    — sphere center (world space)
// sphereRadius — physical emitter radius
//
// Reference: Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013, eq. 12.
vec3 calculateSphereAreaLightRepresentativePoint(vec3 fragPos, vec3 N, vec3 V,
                                                  vec3 lightPos, float sphereRadius)
{
    vec3 r = reflect(-V, N);
    vec3 L = lightPos - fragPos;
    vec3 centerToRay = dot(L, r) * r - L;
    vec3 closestPoint = L + centerToRay * clamp(sphereRadius / max(length(centerToRay), EPSILON), 0.0, 1.0);
    return normalize(closestPoint);
}

// Energy-conservation rescale for the GGX normalization when the light has
// a physical radius. Without this, larger radii produce brighter highlights.
// (Karis 2013, eq. 14 — derived from the analytic solid angle of a sphere.)
float sphereAreaLightNormalization(float roughness, float distance, float sphereRadius)
{
    float alpha = roughness * roughness;
    float alphaPrime = clamp(alpha + sphereRadius / max(2.0 * distance, EPSILON), 0.0, 1.0);
    // Squared ratio so the BRDF integrates to 1 as radius -> 0 (point light).
    float ratio = alpha / max(alphaPrime, EPSILON);
    return ratio * ratio;
}

// Evaluate a sphere area light at the surface.
// Returns the radiance contribution (radiance * NdotL * BRDF).
vec3 calculateSphereAreaLightContribution(vec3 N, vec3 V, vec3 lightPos, float sphereRadius,
                                           vec3 lightColor, float lightIntensity, float range,
                                           vec3 albedo, float metallic, float roughness, vec3 worldPos)
{
    vec3 toLight = lightPos - worldPos;
    float distance = length(toLight);

    // Early-out: outside range. Range is measured from the centre, matching
    // the way light culling treats the bounding sphere.
    if (distance > range) return vec3(0.0);

    // Standard L for diffuse — use the light centre, not the representative
    // point (the diffuse term integrates over the full hemisphere already).
    vec3 Ldiff = toLight / max(distance, EPSILON);
    float NdotL = max(dot(N, Ldiff), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);

    // Smooth distance attenuation matching the Forward+ point-light falloff.
    float distRatio = distance / max(range, EPSILON);
    float distAtten = max(1.0 - distRatio * distRatio, 0.0);
    distAtten = distAtten * distAtten / (distance * distance + 1.0);

    // Representative point for specular. Closer-to-zero radius converges to
    // the centre direction, recovering point-light behaviour.
    vec3 Lspec = calculateSphereAreaLightRepresentativePoint(worldPos, N, V, lightPos, sphereRadius);

    // Split BRDF: diffuse uses Ldiff (centre), specular uses Lspec (rep point).
    // We compute diffuse + specular separately to avoid double-counting fresnel
    // off the wrong half-vector.
    vec3 H = normalize(V + Lspec);
    float NdotV = max(dot(N, V), 0.0);
    float NdotLspec = max(dot(N, Lspec), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Lspec, roughness);
    vec3  F = fresnelSchlick(VdotH, F0);

    // Energy-conservation rescale (Karis eq. 14).
    float normFactor = sphereAreaLightNormalization(roughness, distance, sphereRadius);
    D *= normFactor;

    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotLspec, EPSILON);

    // Diffuse uses the centre direction; energy lost to specular is removed.
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 diffuse = kD * albedo * INV_PI;

    vec3 radiance = lightColor * lightIntensity * distAtten;
    // Diffuse term scales by NdotL (Lambertian); specular by NdotLspec.
    return (diffuse * NdotL + specular * NdotLspec) * radiance;
}

// =============================================================================
// MULTI-LIGHT CALCULATION
// =============================================================================

// Calculate contribution from a single light. The pbrModel overload is the
// real body; the trailing selector routes the punctual-light BRDF through
// evaluatePBRClosure (Legacy pixels are bit-identical — the Legacy branch IS
// cookTorranceBRDF). Sphere-area lights deliberately keep the Legacy
// representative-point evaluator for every model — see PBR CLOSURE V2.
vec3 calculateLightContribution(LightData light, vec3 N, vec3 V, vec3 albedo,
                               float metallic, float roughness, vec3 worldPos,
                               int pbrModel)
{
    int lightType = int(light.position.w);
    vec3 lightColor = light.color.rgb;
    float lightIntensity = light.color.w;

    // Sphere area lights take a dedicated evaluator — the representative-point
    // trick splits the BRDF differently, so we cannot route it through the
    // common L + evaluatePBRClosure path below.
    if (lightType == SPHERE_AREA_LIGHT)
    {
        float sphereRadius = light.spotParams.z;       // Packed by Scene::ProcessScene3DSharedLogic
        float range        = light.attenuationParams.w;
        return calculateSphereAreaLightContribution(N, V, light.position.xyz, sphereRadius,
                                                    lightColor, lightIntensity, range,
                                                    albedo, metallic, roughness, worldPos);
    }

    vec3 L;
    float attenuation = 1.0;

    // Calculate light direction and attenuation based on type
    if (lightType == DIRECTIONAL_LIGHT)
    {
        L = normalize(-light.direction.xyz);
        // No attenuation for directional lights
    }
    else if (lightType == POINT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuation(light.position.xyz, worldPos, light.attenuationParams);
    }
    else if (lightType == SPOT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuation(light.position.xyz, worldPos, light.attenuationParams);
        float spotIntensity = calculateSpotIntensity(L, light.direction.xyz, light.spotParams);
        attenuation *= spotIntensity;
    }
    else
    {
        return vec3(0.0); // Unknown light type
    }

    // Early exit if light has no contribution
    if (attenuation <= EPSILON) return vec3(0.0);

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);

    // Calculate BRDF
    vec3 radiance = lightColor * lightIntensity * attenuation;
    vec3 brdf = evaluatePBRClosure(pbrModel, N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// Legacy-model convenience overload: call sites with no material model (the
// terrain and voxel shaders) keep their existing signature and closure.
vec3 calculateLightContribution(LightData light, vec3 N, vec3 V, vec3 albedo,
                               float metallic, float roughness, vec3 worldPos)
{
    return calculateLightContribution(light, N, V, albedo, metallic, roughness, worldPos,
                                      OLO_PBR_MODEL_LEGACY);
}

// Enhanced light contribution with better energy conservation
vec3 calculateLightContributionEnhanced(LightData light, vec3 N, vec3 V, vec3 albedo,
                                       float metallic, float roughness, vec3 worldPos)
{
    int lightType = int(light.position.w);
    vec3 lightColor = light.color.rgb;
    float lightIntensity = light.color.w;

    // Sphere area lights split the BRDF differently; use the dedicated evaluator.
    if (lightType == SPHERE_AREA_LIGHT)
    {
        float sphereRadius = light.spotParams.z;
        float range        = light.attenuationParams.w;
        return calculateSphereAreaLightContribution(N, V, light.position.xyz, sphereRadius,
                                                    lightColor, lightIntensity, range,
                                                    albedo, metallic, roughness, worldPos);
    }

    vec3 L;
    float attenuation = 1.0;

    if (lightType == DIRECTIONAL_LIGHT)
    {
        L = normalize(-light.direction.xyz);
    }
    else if (lightType == POINT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuationEpic(light.position.xyz, worldPos, light.attenuationParams.w);
    }
    else if (lightType == SPOT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuationEpic(light.position.xyz, worldPos, light.attenuationParams.w);
        float spotIntensity = calculateSpotIntensityAdvanced(L, light.direction.xyz, light.spotParams);
        attenuation *= spotIntensity;
    }
    else
    {
        return vec3(0.0);
    }

    if (attenuation <= EPSILON) return vec3(0.0);

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);

    // Use enhanced BRDF with height-correlated Smith G function
    vec3 radiance = lightColor * lightIntensity * attenuation;
    vec3 brdf = cookTorranceBRDFEnhanced(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// =============================================================================
// SHADOW FUNCTIONS
// =============================================================================

// PCF (Percentage Closer Filtering) for soft shadow edges
float sampleShadowPCF(sampler2DArrayShadow shadowMap, vec3 projCoords, float layer, float bias, int resolution)
{
    float shadow = 0.0;
    float texelSize = 1.0 / float(resolution);

    // 3x3 PCF kernel
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            // sampler2DArrayShadow: texture(sampler, vec4(uv.x, uv.y, layer, compareRef))
            shadow += texture(shadowMap, vec4(projCoords.xy + offset, layer, projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

// =============================================================================
// PCSS — Percentage-Closer Soft Shadows (contact-hardening variable penumbra)
// =============================================================================
// Sharp where an occluder meets the receiver, softening with separation. Two
// stages: (1) a blocker search over the RAW depth array (a comparison-OFF view
// bound alongside the hardware-comparison array — the comparison sampler can't
// return raw occluder depth) to find the average occluder depth, then (2) a
// variable-radius PCF whose radius is the estimated penumbra. Gated by
// u_SoftShadowMode (passed in as softMode) so the legacy fixed PCF stays the
// default fallback.

// Shared 16-tap Poisson disk (unit disk) for both the blocker search and PCF.
const vec2 POISSON_DISK_16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// Per-fragment rotation angle to decorrelate the Poisson pattern (cheap hash of
// world position — turns sampling banding into noise the eye tolerates better).
float pcssRotationAngle(vec3 worldPos)
{
    float h = fract(sin(dot(worldPos.xy + worldPos.yz, vec2(12.9898, 78.233))) * 43758.5453);
    return h * 6.2831853; // 0..2pi
}

// Blocker search on the raw (comparison-OFF) array. Returns the average blocker
// depth in shadow-map NDC depth space, or -1.0 if no blocker is found.
// searchRadiusUV / depths are in shadow-map UV / NDC units.
float pcssBlockerSearch(sampler2DArray rawMap, vec2 uv, float layer, float zReceiver,
                        float searchRadiusUV, mat2 rot, out int numBlockers)
{
    float blockerSum = 0.0;
    numBlockers = 0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 offs = (rot * POISSON_DISK_16[i]) * searchRadiusUV;
        float d = texture(rawMap, vec3(uv + offs, layer)).r;
        if (d < zReceiver) // closer to the light than the receiver -> occluder
        {
            blockerSum += d;
            numBlockers += 1;
        }
    }
    return (numBlockers == 0) ? -1.0 : (blockerSum / float(numBlockers));
}

// Variable-radius PCF using the hardware comparison sampler (free 2x2 bilinear
// PCF per tap). filterRadiusUV in shadow-map UV units.
float pcssVariablePCF(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float zReceiver,
                      float bias, float filterRadiusUV, mat2 rot)
{
    float sum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 offs = (rot * POISSON_DISK_16[i]) * filterRadiusUV;
        sum += texture(shadowMap, vec4(uv + offs, layer, zReceiver - bias));
    }
    return sum / 16.0;
}

// Full PCSS visibility for one shadow-map layer. `softness` is the light's
// apparent size knob (ShadowParams.z); larger -> softer. Returns [0,1] (1 lit).
float pcssShadowFactor(sampler2DArrayShadow shadowMap, sampler2DArray rawMap,
                       vec2 uv, float layer, float zReceiver, float bias,
                       float softness, int resolution, mat2 rot)
{
    float texelSizeUV = 1.0 / float(resolution);

    // Light apparent size in texels. Softness == 1 -> ~4-texel light.
    float lightSizeTexels = max(softness, 0.05) * 4.0;
    float searchRadiusUV = max(lightSizeTexels, 2.0) * texelSizeUV;

    int numBlockers;
    float avgBlocker = pcssBlockerSearch(rawMap, uv, layer, zReceiver, searchRadiusUV, rot, numBlockers);
    if (numBlockers == 0)
        return 1.0; // no occluder -> fully lit
    if (numBlockers == 16)
        return 0.0; // every search tap occluded -> deep umbra, skip the filter

    // Penumbra from occluder/receiver depth gap (contact-hardening). The gain
    // absorbs the cascade depth scale; tuned for a natural soft edge. Clamp to
    // [1 texel, lightSizeTexels*4] to bound cost and avoid over-blurring —
    // wide scattered Poisson taps thrash the texture cache, and this radius
    // cap (down from *8) measured as one of the biggest PCSS costs on Sponza.
    const float PCSS_PENUMBRA_GAIN = 220.0;
    float depthGap = max(zReceiver - avgBlocker, 0.0);
    float filterRadiusTexels = clamp(depthGap * lightSizeTexels * PCSS_PENUMBRA_GAIN,
                                     1.0, lightSizeTexels * 4.0);

    // Contact-hardened fragments (radius clamped to the 1-texel floor) collapse
    // to a single hardware-PCF tap — its 2x2 bilinear footprint already covers
    // a 1-texel radius, so the 16-tap Poisson disk adds cost but no quality.
    if (filterRadiusTexels <= 1.0)
        return texture(shadowMap, vec4(uv, layer, zReceiver - bias));

    float filterRadiusUV = filterRadiusTexels * texelSizeUV;

    return pcssVariablePCF(shadowMap, uv, layer, zReceiver, bias, filterRadiusUV, rot);
}

// Dispatch one layer's shadow test to PCSS (softMode==1) or the legacy 3x3 PCF.
float sampleShadowLayer(sampler2DArrayShadow shadowMap, sampler2DArray rawMap,
                        vec3 projCoords, float layer, float bias, int resolution,
                        int softMode, float softness, mat2 rot)
{
    if (softMode == 1)
        return pcssShadowFactor(shadowMap, rawMap, projCoords.xy, layer, projCoords.z, bias, softness, resolution, rot);
    return sampleShadowPCF(shadowMap, projCoords, layer, bias, resolution);
}

// Calculate CSM shadow factor for directional lights
// shadowMap: sampler2DArrayShadow bound at TEX_SHADOW (binding 8)
// worldPos: fragment world position
// surfaceNormal: fragment shading normal for world-space receiver offset
// viewDepth: fragment view-space depth (needed for cascade selection)
// lightSpaceMatrices[4]: per-cascade light VP matrices
// cascadePlaneDistances: view-space far distances for each cascade
// shadowParams: x=bias, y=normalBias, z=softness, w=maxShadowDistance
// shadowMapResolution: shadow map size in pixels
float calculateCascadedShadowFactorCSM(
    sampler2DArrayShadow shadowMap,
    sampler2DArray rawShadowMap,
    vec3 worldPos,
    vec3 surfaceNormal,
    float viewDepth,
    mat4 lightSpaceMatrices[4],
    vec4 cascadePlaneDistances,
    vec4 shadowParams,
    int shadowMapResolution,
    int softMode)
{
    float maxShadowDistance = shadowParams.w;

    // PCSS sampling state (ignored by the legacy PCF path). softness == light
    // apparent size (ShadowParams.z); rot decorrelates the Poisson kernel.
    float softness = shadowParams.z;
    float rotAngle = pcssRotationAngle(worldPos);
    mat2 shadowRot = mat2(cos(rotAngle), -sin(rotAngle), sin(rotAngle), cos(rotAngle));
    if (-viewDepth > maxShadowDistance)
    {
        return 1.0; // Beyond shadow distance
    }

    // Select cascade based on view-space depth
    int cascadeIndex = 3; // Default to last cascade
    float cascadeDists[4] = float[4](
        cascadePlaneDistances.x,
        cascadePlaneDistances.y,
        cascadePlaneDistances.z,
        cascadePlaneDistances.w
    );

    for (int i = 0; i < 4; ++i)
    {
        if (-viewDepth < cascadeDists[i])
        {
            cascadeIndex = i;
            break;
        }
    }

    // Offset the receiver along its shading normal before projection. The
    // light component's normal-bias setting is in world metres (as VSM's
    // normal offset is), so applying it to compare depth would be both the
    // wrong space and wildly over-biased. This prevents a thin receiver from
    // comparing against its own rasterized depth at grazing angles.
    vec3 biasedWorldPos = worldPos + normalize(surfaceNormal) * shadowParams.y;

    // Transform to light space
    vec4 lightSpacePos = lightSpaceMatrices[cascadeIndex] * vec4(biasedWorldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5; // NDC [-1,1] -> [0,1]

    // Out of shadow map bounds
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 1.0;
    }

    // Scale bias by cascade (farther cascades need more bias)
    float baseBias = shadowParams.x;
    float cascadeBias = baseBias * float(cascadeIndex + 1);

    // PCSS only for the two nearest cascades: distant fragments cover too few
    // shadow-map texels for contact hardening to read, while the blocker
    // search + wide Poisson PCF stay just as expensive. Cascades 2-3 fall back
    // to the legacy 3x3 hardware PCF (the cascade cross-fade below blends the
    // transition).
    int layerSoftMode = (cascadeIndex < 2) ? softMode : 0;

    float shadow = sampleShadowLayer(shadowMap, rawShadowMap, projCoords, float(cascadeIndex),
                                     cascadeBias, shadowMapResolution, layerSoftMode, softness, shadowRot);

    // Cascade blending: smooth cross-fade in the last 10% of each cascade
    if (cascadeIndex < 3)
    {
        float cascadeFar = cascadeDists[cascadeIndex];
        float blendZone = cascadeFar * 0.1;
        float blendFactor = smoothstep(cascadeFar - blendZone, cascadeFar, -viewDepth);

        if (blendFactor > 0.0)
        {
            // Sample next cascade
            vec4 nextLightSpacePos = lightSpaceMatrices[cascadeIndex + 1] * vec4(biasedWorldPos, 1.0);
            vec3 nextProjCoords = nextLightSpacePos.xyz / nextLightSpacePos.w;
            nextProjCoords = nextProjCoords * 0.5 + 0.5;
            float nextBias = baseBias * float(cascadeIndex + 2);
            int nextSoftMode = (cascadeIndex + 1 < 2) ? softMode : 0;
            float nextShadow = sampleShadowLayer(shadowMap, rawShadowMap, nextProjCoords, float(cascadeIndex + 1),
                                                 nextBias, shadowMapResolution, nextSoftMode, softness, shadowRot);
            shadow = mix(shadow, nextShadow, blendFactor);
        }
    }

    // Distance fade: smoothly transition to no shadow at max shadow distance.
    // The fade band is (1 - fadeStartFraction) of MaxShadowDistance — at 0.7
    // that's the last 30% of the range, wide enough that a player walking
    // toward the edge of CSM coverage sees a gradual falloff rather than a
    // hard "pop" appearing within a few meters. Tightening this fraction
    // (e.g. 0.95) compresses the fade into a narrow band and reads as a
    // sudden binary transition; loosening it (e.g. 0.5) starts the fade
    // earlier and shadows visibly thin out at mid-range.
    // TODO(olbu): future enhancements to lift the cascade-reach vs. fade-distance coupling:
    //   - Expose fadeStartFraction as a per-scene/per-light parameter instead
    //     of a shader literal.
    //   - Add a separate ShadowFadeDistance distinct from MaxShadowDistance so
    //     cascade splits don't stretch when you only want a longer fade tail.
    //   - Unreal-style Far Shadow Cascades: a second pool of low-res cascades
    //     for long-range geometry tagged with bCastFarShadow, so we get
    //     "shadows visible to the horizon" without losing near-camera resolution.
    //   - Distance Field Shadows beyond CSM range as a cheap soft-shadow fallback.
    float fadeStart = maxShadowDistance * 0.7;
    float distanceFade = 1.0 - smoothstep(fadeStart, maxShadowDistance, -viewDepth);
    shadow = mix(1.0, shadow, distanceFade);

    return shadow;
}

// =============================================================================
// SHADOW ATLAS SAMPLING (issue #435)
// =============================================================================
// Every shadowed local light lives in ONE budgeted atlas texture (a 1-layer
// depth array): a spot light owns one square tile, a point / sphere-area light
// owns six (its cube faces, rendered projectively — no more linear-distance
// cubemaps). Each atlas ENTRY carries a light VP matrix plus the UV
// scale/offset of its tile; these helpers take them as parameters so callers
// index the ShadowData UBO arrays (u_AtlasEntryMatrices /
// u_AtlasEntryScaleOffset) themselves. All filter taps are clamped inside the
// tile (half-texel inset) so neighbouring tiles can never bleed.

// Dominant-axis cube face selector — face order matches
// ShadowMap::BuildPointLightFaceMatrices: +X,-X,+Y,-Y,+Z,-Z.
int atlasCubeFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z)
        return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

// 3x3 PCF over an atlas tile. projCoords are the entry's light-space [0,1]
// coords; scaleOffset (xy = scale, zw = offset) maps them into the atlas.
float sampleShadowAtlasPCF(sampler2DArrayShadow atlas, vec3 projCoords, vec4 scaleOffset,
                           float bias, int atlasResolution)
{
    float texel = 1.0 / float(atlasResolution);
    vec2 tileMin = scaleOffset.zw + vec2(texel * 0.5);
    vec2 tileMax = scaleOffset.zw + scaleOffset.xy - vec2(texel * 0.5);
    vec2 baseUV = projCoords.xy * scaleOffset.xy + scaleOffset.zw;

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 uv = clamp(baseUV + vec2(float(x), float(y)) * texel, tileMin, tileMax);
            shadow += texture(atlas, vec4(uv, 0.0, projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

// PCSS over an atlas tile (spot entries): the same two-stage blocker-search +
// variable-radius Poisson PCF as the CSM path, with every tap clamped to the
// entry's tile.
float pcssShadowAtlasFactor(sampler2DArrayShadow atlas, sampler2DArray rawAtlas,
                            vec3 projCoords, vec4 scaleOffset, float bias,
                            float softness, int atlasResolution, mat2 rot)
{
    float texel = 1.0 / float(atlasResolution);
    vec2 tileMin = scaleOffset.zw + vec2(texel * 0.5);
    vec2 tileMax = scaleOffset.zw + scaleOffset.xy - vec2(texel * 0.5);
    vec2 baseUV = projCoords.xy * scaleOffset.xy + scaleOffset.zw;
    float zReceiver = projCoords.z;

    float lightSizeTexels = max(softness, 0.05) * 4.0;
    float searchRadiusUV = max(lightSizeTexels, 2.0) * texel;

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 uv = clamp(baseUV + (rot * POISSON_DISK_16[i]) * searchRadiusUV, tileMin, tileMax);
        float d = texture(rawAtlas, vec3(uv, 0.0)).r;
        if (d < zReceiver)
        {
            blockerSum += d;
            numBlockers += 1;
        }
    }
    if (numBlockers == 0)
        return 1.0;
    if (numBlockers == 16)
        return 0.0;
    float avgBlocker = blockerSum / float(numBlockers);

    const float PCSS_PENUMBRA_GAIN = 220.0;
    float depthGap = max(zReceiver - avgBlocker, 0.0);
    float filterRadiusTexels = clamp(depthGap * lightSizeTexels * PCSS_PENUMBRA_GAIN,
                                     1.0, lightSizeTexels * 4.0);
    if (filterRadiusTexels <= 1.0)
        return texture(atlas, vec4(clamp(baseUV, tileMin, tileMax), 0.0, zReceiver - bias));

    float filterRadiusUV = filterRadiusTexels * texel;
    float sum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 uv = clamp(baseUV + (rot * POISSON_DISK_16[i]) * filterRadiusUV, tileMin, tileMax);
        sum += texture(atlas, vec4(uv, 0.0, zReceiver - bias));
    }
    return sum / 16.0;
}

// Visibility for one atlas entry: project worldPos by the entry matrix,
// remap into its tile, and filter. Returns 1.0 (lit) outside the entry's
// frustum. Spot entries pass softMode = u_SoftShadowMode (PCSS-capable);
// point cube-face entries pass softMode = 0 (PCF only, matching the old
// cubemap path which never had PCSS).
float calculateAtlasEntryShadow(vec3 worldPos, mat4 entryMatrix, vec4 scaleOffset,
                                sampler2DArrayShadow atlas, sampler2DArray rawAtlas,
                                float bias, int atlasResolution, int softMode, float softness)
{
    vec4 lightSpacePos = entryMatrix * vec4(worldPos, 1.0);
    if (lightSpacePos.w <= 0.0)
        return 1.0; // behind the light's perspective projection
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0 || projCoords.z < 0.0)
    {
        return 1.0;
    }

    if (softMode == 1)
    {
        float rotAngle = pcssRotationAngle(worldPos);
        mat2 rot = mat2(cos(rotAngle), -sin(rotAngle), sin(rotAngle), cos(rotAngle));
        return pcssShadowAtlasFactor(atlas, rawAtlas, projCoords, scaleOffset, bias, softness, atlasResolution, rot);
    }
    return sampleShadowAtlasPCF(atlas, projCoords, scaleOffset, bias, atlasResolution);
}

// =============================================================================
// IBL LIGHTING FUNCTIONS
// =============================================================================

// Calculate ambient lighting using IBL
vec3 calculateIBL(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                  samplerCube irradianceMap, samplerCube prefilterMap, sampler2D brdfLUT)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // Diffuse IBL
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuse = irradiance * albedo;

    // Specular IBL
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    return kD * diffuse + specular;
}

// calculateIBL with the specular prefilter fetch hoisted out: the caller
// resolves `prefilteredColor` itself (global prefilter map, or the
// distance-impostor probe blend from include/ReflectionProbes.glsl — issue
// #705) and this applies the identical BRDF split. Keep the body in lockstep
// with calculateIBL above.
vec3 calculateIBLPrefiltered(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                             samplerCube irradianceMap, sampler2D brdfLUT, vec3 prefilteredColor)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // Diffuse IBL
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuse = irradiance * albedo;

    // Specular from the caller-resolved prefiltered radiance
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    return kD * diffuse + specular;
}

// Simple ambient lighting fallback when IBL is not available
// NOTE: Does NOT include AO — caller applies AO uniformly via `ambient * ao`
// to match the IBL / light-probe paths which also omit AO from their returns.
vec3 calculateSimpleAmbient(vec3 albedo, float metallic, float ao)
{
    vec3 ambient = vec3(0.03) * albedo;
    return ambient;
}

// Enhanced IBL with importance sampling (for real-time global illumination)
vec3 calculateIBLImportanceSampled(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                                  samplerCube environmentMap, int sampleCount)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 color = vec3(0.0);
    float totalWeight = 0.0;

    // Sample environment using importance sampling
    for (int i = 0; i < sampleCount; ++i)
    {
        vec2 Xi = hammersleySequence(uint(i), uint(sampleCount));
        vec3 H = importanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            vec3 sampleColor = texture(environmentMap, L).rgb;

            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);

            float D = distributionGGX(N, H, roughness);
            float G = geometrySmith(N, V, L, roughness);
            vec3 F = fresnelSchlick(VdotH, F0);

            vec3 numerator = D * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + EPSILON;
            vec3 specular = numerator / denominator;

            color += sampleColor * specular * NdotL;
            totalWeight += NdotL;
        }
    }

    return color / max(totalWeight, EPSILON);
}

// =============================================================================
// LIGHT PROBE AMBIENT FUNCTIONS
// =============================================================================

// Calculate ambient lighting from light probe irradiance
// Mirrors calculateIBL energy conservation: kD *= (1.0 - metallic)
vec3 calculateLightProbeAmbient(vec3 probeIrradiance, vec3 albedo, float metallic, float roughness,
                                vec3 N, vec3 V)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    return kD * probeIrradiance * albedo;
}

// Combine light probe diffuse with IBL specular
// Probes provide diffuse irradiance; IBL prefilter map provides specular reflections
vec3 calculateCombinedAmbient(vec3 probeIrradiance, vec3 N, vec3 V, vec3 albedo,
                              float metallic, float roughness,
                              samplerCube prefilterMap, sampler2D brdfLUT)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // Diffuse from probes
    vec3 diffuse = probeIrradiance * albedo;

    // Specular from IBL prefilter
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    return kD * diffuse + specular;
}

// calculateCombinedAmbient with the specular prefilter fetch hoisted out —
// same contract as calculateIBLPrefiltered (issue #705). Keep the body in
// lockstep with calculateCombinedAmbient above.
vec3 calculateCombinedAmbientPrefiltered(vec3 probeIrradiance, vec3 N, vec3 V, vec3 albedo,
                                         float metallic, float roughness,
                                         sampler2D brdfLUT, vec3 prefilteredColor)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // Diffuse from probes
    vec3 diffuse = probeIrradiance * albedo;

    // Specular from the caller-resolved prefiltered radiance
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    return kD * diffuse + specular;
}

// =============================================================================
// SHADER-SPECIFIC LIGHT CALCULATIONS
// =============================================================================

// Calculate directional light contribution using shader uniform
vec3 calculateDirectionalLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                                     vec3 lightDirection, vec3 lightDiffuse)
{
    vec3 L = normalize(-lightDirection);
    vec3 radiance = lightDiffuse;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// Calculate point light contribution using shader uniform
vec3 calculatePointLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                               vec3 worldPos, vec3 lightPosition, vec3 lightDiffuse, vec4 attParams)
{
    vec3 L = normalize(lightPosition - worldPos);
    float distance = length(lightPosition - worldPos);
    float attenuation = 1.0 / (attParams.x + attParams.y * distance + attParams.z * distance * distance);
    vec3 radiance = lightDiffuse * attenuation;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// Calculate spot light contribution using shader uniform
vec3 calculateSpotLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                              vec3 worldPos, vec3 lightPosition, vec3 lightDirection,
                              vec3 lightDiffuse, vec4 attParams, vec4 spotParams)
{
    vec3 L = normalize(lightPosition - worldPos);
    float distance = length(lightPosition - worldPos);
    float attenuation = 1.0 / (attParams.x + attParams.y * distance + attParams.z * distance * distance);

    float theta = dot(L, normalize(-lightDirection));
    float epsilon = spotParams.x - spotParams.y;
    float intensity = clamp((theta - spotParams.y) / epsilon, 0.0, 1.0);

    vec3 radiance = lightDiffuse * attenuation * intensity;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// =============================================================================
// MATERIAL SAMPLING FUNCTIONS
//
// OPTIMIZATION NOTE: These functions now avoid unnecessary texture lookups
// by checking use flags before sampling. This reduces memory bandwidth usage
// and improves performance when textures are not used.
//
// GAMMA CORRECTION NOTE: Albedo textures should be in sRGB format and will be
// automatically converted to linear space by the GPU. Metallic, roughness,
// normal, and AO maps should already be in linear space.
// =============================================================================

// Sample base color/albedo (assumes sRGB texture, GPU converts to linear)
vec3 sampleAlbedo(sampler2D albedoMap, vec2 texCoord, vec3 baseColorFactor, bool useMap)
{
    if (useMap)
    {
        return baseColorFactor.rgb * texture(albedoMap, texCoord).rgb;
    }
    return baseColorFactor.rgb;
}

// Sample metallic and roughness (linear textures)
vec2 sampleMetallicRoughness(sampler2D metallicRoughnessMap, vec2 texCoord,
                             float metallicFactor, float roughnessFactor, bool useMap)
{
    if (useMap) {
        vec3 metallicRoughness = texture(metallicRoughnessMap, texCoord).rgb;
        return vec2(metallicFactor * metallicRoughness.b,   // Blue channel = metallic
                    roughnessFactor * metallicRoughness.g); // Green channel = roughness
    }
    return vec2(metallicFactor, roughnessFactor);
}

// Sample ambient occlusion (linear texture)
float sampleAO(sampler2D aoMap, vec2 texCoord, float occlusionStrength, bool useMap)
{
    if (useMap) {
        float ao = texture(aoMap, texCoord).r;
        return mix(1.0, ao, occlusionStrength);
    }
    return 1.0;
}

// Sample emissive (assumes sRGB texture if used for color, linear for HDR)
vec3 sampleEmissive(sampler2D emissiveMap, vec2 texCoord, vec3 emissiveFactor, bool useMap)
{
    if (useMap) {
        return emissiveFactor * texture(emissiveMap, texCoord).rgb;
    }
    return emissiveFactor;
}

#endif // PBR_GLSL
