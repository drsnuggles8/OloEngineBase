#ifndef SKIN_ORAL_SURFACE_GLSL
#define SKIN_ORAL_SURFACE_GLSL

// =============================================================================
// SkinOralSurface.glsl — the wet coat and the cavity weight, issue #1245.
//
// THE MATHS LIVES ON THE CPU, in Renderer/SkinOralSurface.h, and that file is
// where the physical decisions and the energy argument are. Everything here is a
// transcription of it, in the same order, operation for operation, and
// SkinOralSurfaceParityTest drives this file against that one so the two cannot
// drift.
//
// INCLUDED BY include/PBRCommon.glsl, after include/SkinLayeredSpecular.glsl,
// so every shader that can shade skin gets the same arithmetic: the forward
// paths (PBR_MultiLight{,_Skinned}.glsl), the clustered path
// (include/ForwardPlusCommon.glsl) and the deferred lighting pass
// (include/DeferredLightingShared.glsl).
//
// -----------------------------------------------------------------------------
// THE COAT TAKES ENERGY, IT DOES NOT ADD IT
// -----------------------------------------------------------------------------
//
//     attenuation = 1 - strength * F_coat
//     out.Diffuse  = base.Diffuse  * attenuation
//     out.Specular = base.Specular * attenuation + coatSpecular * strength
//
// The film is a layer IN FRONT of the tissue. Light it reflects never reaches
// the tissue, so the tissue's response loses exactly what the film gained —
// `strength * F_coat` and `attenuation` sum to one for every legal input.
//
// THAT IS WHY THE WET SPECULAR IS DISTINCT FROM THE DIFFUSION BY CONSTRUCTION
// and not by tuning. The coat's own lobe is added to `.Specular`, which is the
// half oloSkinDiffusionOutput does NOT hand to the diffusion pass; the diffuse
// half is only ever ATTENUATED here, never brightened. So no amount of coat can
// put energy into the blur, and a wet lip cannot make the tissue under it glow.
//
// -----------------------------------------------------------------------------
// WHAT THE COAT IS APPLIED TO, AND WHAT IT IS NOT
// -----------------------------------------------------------------------------
//
// PUNCTUAL AND AREA LIGHTS: yes. IBL, light probes and the ambient ladder: NO,
// and for the reason include/SkinLayeredSpecular.glsl gives about its second
// lobe — an environment coat means a SECOND prefiltered cubemap fetch per pixel,
// at a different mip, and this file is not going to spend one where that file
// declined to. Stated here rather than left to be noticed: a mouth lit ONLY by
// an environment map will not read as wet, and the fix is a key light, which is
// what every close-up in the evidence matrix uses.
//
// -----------------------------------------------------------------------------
// THE CAVITY WEIGHT GATES THE TRANSMISSION AND NOTHING ELSE
// -----------------------------------------------------------------------------
//
// The transmitted lobe is the only term here that is not already occluded: the
// direct lobes are gated by `lightVisibility` and the ambient ladder by the
// material's AO. A closed mouth casts no shadow onto its own interior at any
// shadow-map resolution this engine runs at, so without this the lips stay lit
// from inside — the "glowing interior" the acceptance criteria name. Applying
// the weight to the ambient as well would apply the AO twice, which is as wrong
// as applying it never.
// =============================================================================

// Fresnel reflectance at normal incidence for a coat of index `ior` seen from
// air. Mirrors SkinOralCoatF0.
//
// PRESENT HERE BUT NOT CALLED BY ANY PRODUCTION PATH, deliberately: the lane
// carries F0 already, converted once on the CPU where a test can look at it.
// This exists so ShaderUnit_SkinOralSurface.glsl can assert that the two sides
// agree about the conversion rather than about a number somebody typed twice.
float oloSkinOralCoatF0(float ior)
{
    float clamped = clamp(ior, 1.0, 2.5);
    float ratio = (clamped - 1.0) / (clamped + 1.0);
    return ratio * ratio;
}

// Schlick Fresnel of the coat. ACHROMATIC — a water film has no absorption
// worth modelling over its thickness, and the tissue underneath is where colour
// comes from.
//
// `cosTheta` is dot(V, H) — the angle of INCIDENCE on the microfacet that
// reflected this light. NOT dot(N, V), and NOT dot(N, H): see
// Renderer/SkinOralSurface.h, which records that dot(N, H) shipped in the first
// version of this file and is invisible at a head-on fixture while being wrong
// by a factor of nine at grazing. Every Fresnel in PBRCommon.glsl passes
// `dot(H, V)`.
float oloSkinOralCoatFresnel(float f0, float cosTheta)
{
    float clampedF0 = clamp(f0, 0.0, 1.0);
    float oneMinus = 1.0 - clamp(cosTheta, 0.0, 1.0);
    // Four multiplies rather than pow(), matching every Schlick in this engine
    // and matching the CPU, which the parity test compares against exactly.
    float oneMinus2 = oneMinus * oneMinus;
    float oneMinus5 = oneMinus2 * oneMinus2 * oneMinus;
    return clampedF0 + (1.0 - clampedF0) * oneMinus5;
}

// How much of the base response survives the coat. In [0, 1] for every input.
float oloSkinOralCoatAttenuation(float strength, float fresnel)
{
    return 1.0 - clamp(strength, 0.0, 1.0) * clamp(fresnel, 0.0, 1.0);
}

// The coat's specular BRDF for one light direction — GGX with the same D and
// the same height-correlated Smith visibility every other closure in this
// engine uses, so the coat and the surface under it are not two different
// microfacet models stacked.
//
// Returns the BRDF, NOT the radiance: the caller multiplies by the incident
// radiance and by dot(N, L), so the coat is gated by the same shadow factor and
// the same light sampling as everything else.
float oloSkinOralCoatSpecular(vec3 N, vec3 V, vec3 L, float coatRoughness, float coatF0)
{
    vec3 sum = V + L;
    float lenSq = dot(sum, sum);
    // A view and light direction that cancel have no half-vector. The
    // `!(x > eps)` form, so a NaN input takes the fallback instead of being
    // normalized into one — the same guard shape PBRCommon.glsl's degeneracy
    // tests use.
    if (!(lenSq > kDegenerateEpsilon))
        return 0.0;

    vec3 H = sum * inversesqrt(lenSq);
    float roughness = clamp(coatRoughness, 0.01, 1.0);

    float D = distributionGGX(N, H, roughness);
    float Vis = visibilitySmithGGXCorrelated(N, V, L, roughness);
    float F = oloSkinOralCoatFresnel(coatF0, dot(V, H));

    return D * Vis * F;
}

// Apply the coat to one light's already-evaluated split. The ONE place the
// partition is written on this side.
//
// `radiance` is the incident radiance ALREADY scaled by dot(N, L) — the same
// number the split it is being applied to was scaled by — so the coat's lobe and
// the surface's lobe are lit identically and the partition holds per light
// rather than only on average over the frame.
//
// `oralLane` is the profile lane: x = CoatStrength, y = CoatRoughness,
// z = CoatF0, w = CavityOcclusion (not read here). A zero x returns the input
// UNTOUCHED — not multiplied by an attenuation that happens to be 1, untouched —
// so a dry profile is bit-identical to the version-3 frame and costs one
// compare rather than a GGX evaluation.
OloSurfaceLighting oloSkinOralApplyCoat(OloSurfaceLighting lighting, vec3 N, vec3 V, vec3 L,
                                        vec3 radiance, vec4 oralLane)
{
    float strength = oralLane.x;
    if (!(strength > 0.0))
        return lighting;

    vec3 sum = V + L;
    float lenSq = dot(sum, sum);
    if (!(lenSq > kDegenerateEpsilon))
        return lighting;
    vec3 H = sum * inversesqrt(lenSq);

    // dot(V, H), matching oloSkinOralCoatSpecular — the attenuation and the
    // lobe MUST use one Fresnel or the partition stops summing to one.
    float fresnel = oloSkinOralCoatFresnel(oralLane.z, dot(V, H));
    float attenuation = oloSkinOralCoatAttenuation(strength, fresnel);

    float coat = oloSkinOralCoatSpecular(N, V, L, oralLane.y, oralLane.z);
    float clampedStrength = clamp(strength, 0.0, 1.0);

    return OloSurfaceLighting(lighting.Diffuse * attenuation,
                              lighting.Specular * attenuation + max(radiance, vec3(0.0)) * (coat * clampedStrength));
}

// How much of the transmitted lobe survives the cavity.
//
// `1 + amount * (ao - 1)` rather than `mix(1, ao, amount)`: at amount == 0 the
// first returns 1 EXACTLY for every finite ao, which is what makes "cavity off"
// a true identity a golden image can assert against.
float oloSkinOralCavityWeight(float occlusion, float cavityOcclusion)
{
    float ao = clamp(occlusion, 0.0, 1.0);
    float amount = clamp(cavityOcclusion, 0.0, 1.0);
    return 1.0 + amount * (ao - 1.0);
}

// Whether a pixel evaluates the oral terms at all.
//
// THE ONE PLACE THE VERSION TEST IS SPELLED on this side, mirroring
// SkinEvaluatesOralSurface in Renderer/SkinOralSurface.h — same name, same
// shape — so the forward paths, the clustered path and the deferred pass cannot
// disagree about which pixels are wet.
//
// An `==` and not a `>=`, matching every version branch before it: a version
// this shader has no arm for applies NOTHING rather than guessing that a later
// transport meant the same thing by these fields.
//
// BELT AND BRACES over the CPU, which already zeroes the lane for every
// non-version-4 profile — but the DEFERRED table is indexed by a slot that can
// be stale by a frame after a scene change, and a stale slot must lose the
// effect rather than acquire someone else's wetness.
//
// A BOOL GATE RATHER THAN A vec4 PASS-THROUGH, and the difference is not
// stylistic — it is the one shape of this helper that shades correctly here.
//
// The obvious spelling is `vec4 oloSkinOralLaneFor(int, int, vec4)` returning
// either the lane or vec4(0), which is exactly what oloSkinLobeFor does one
// file up. Written that way, on THIS box (NVIDIA, OpenGL 4.6, the SPIR-V ->
// SPIRV-Cross -> GLSL round trip this engine uses), the forward shader silently
// stopped applying the SKIN SPECULAR TINT — a term seventy lines away with no
// data dependency on any of it. SkinProfileParityScene caught it: the skin
// sphere's highlight came out neutral instead of red, on Forward and Forward+
// but not Deferred, which is the path that does not read this lane out of the
// material UBO.
//
// It is a MISCOMPILE, not a logic error, and it was bisected to that: the same
// gate written as a ternary at the call site is correct, the same helper with
// an unconditional body is correct, and the helper that branches on its int
// parameters and returns the vec4 parameter is not. It reproduces with a cold
// shader cache, so it is not a stale binary. The trigger is most likely the
// size of this fragment shader — its SPIR-V is about 111 000 words before any
// of this — but the mechanism is not proven and this comment does not claim it.
//
// What IS established is the shape that works, so that is the shape this file
// ships: the gate returns a bool and the caller selects the lane. The version
// test still lives in exactly one place, which was the point of the helper.
bool oloSkinEvaluatesOralSurface(int materialKind, int evaluationModel)
{
    return materialKind == OLO_MATERIAL_KIND_SKIN && evaluationModel == OLO_SKIN_MODEL_ORAL_SURFACE;
}

#endif // SKIN_ORAL_SURFACE_GLSL
