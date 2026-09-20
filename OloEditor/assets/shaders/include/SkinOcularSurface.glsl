#ifndef SKIN_OCULAR_SURFACE_GLSL
#define SKIN_OCULAR_SURFACE_GLSL

// =============================================================================
// SkinOcularSurface.glsl — the corneal refraction and the iris response behind
// it, issue #1244.
//
// THE MATHS LIVES ON THE CPU, in Renderer/SkinOcularSurface.h, and that file is
// where the physical decisions and the measured comparison are. Everything here
// is a transcription of it, in the same order, operation for operation, and
// SkinOcularSurfaceParityTest drives this file against that one so the two
// cannot drift.
//
// INCLUDED BY include/PBRCommon.glsl, after include/SkinOralSurface.glsl, so
// every shader that can shade skin gets the same arithmetic.
//
// -----------------------------------------------------------------------------
// CALLED FROM THE MATERIAL STAGE, NOT THE LIGHTING STAGE
// -----------------------------------------------------------------------------
//
// This is the structural difference from every skin term before it and it is
// worth being explicit about, because the four files beside this one all do the
// opposite.
//
// A refraction changes WHICH SURFACE POINT YOU ARE LOOKING AT. It is a function
// of the view alone — no light direction, no radiance, no shadow factor — so it
// belongs where the material is resolved and not where the lighting is
// accumulated. The call sites are therefore:
//
//     PBR_MultiLight.glsl / PBR_MultiLight_Skinned.glsl   (forward, Forward+)
//     PBR_GBuffer.glsl    / PBR_GBuffer_Skinned.glsl      (deferred)
//
// and in all four it runs ONCE, before any light is looked at, rewriting the
// albedo and the shading normal. include/DeferredLightingShared.glsl does not
// call it at all: by the time that file runs, the G-Buffer already carries an
// albedo and a normal with the cornea in them.
//
// THREE THINGS FOLLOW, and they are the answer to three of issue #1244's four
// acceptance criteria:
//
//   * THE PATHS AGREE BY CONSTRUCTION. Forward, Forward+ and Deferred run the
//     same function at the same stage on the same inputs. Criterion 4 asks for
//     cost and quality across the intended paths; the honest answer is
//     "identical", and it is identical because of where the code is rather than
//     because three edits happened to match.
//   * NOTHING TOUCHES THE G-BUFFER LANES. No flags bit, no profile table, no
//     new attachment. The G-Buffer flags lane whose packing #1288 records
//     getting wrong is neither read nor written here.
//   * LAYER SORTING CANNOT GO WRONG. Cornea, iris and tear film are not three
//     depth-sorted surfaces — they are three terms at one surface in a fixed
//     code order: refract (which iris point is this?), iris response (what
//     colour and normal is that point?), then the surface closure and finally
//     #1245's coat. There is no arrangement of them that can sort incorrectly,
//     which is criterion 2's second half answered structurally.
//
// -----------------------------------------------------------------------------
// WHAT THE TEAR LINE IS
// -----------------------------------------------------------------------------
//
// IT IS #1245's WET COAT, and there is no code for it in this file. A tear film
// and a saliva film are the same interface — a thin water layer over wet tissue
// — and their indices differ by 0.007: an F0 of 0.0208 against 0.0201, measured
// in question 7 of experiments/eye-cornea-reference/compare_refraction.py.
//
// So a tear line is authored, not branched: a version-5 profile with
// `Oral.CoatStrength` up and `Oral.CoatIor` 1.337, on the lid-margin geometry.
// That is the same shape #1245 used for teeth, and for the same reason — a
// second implementation of one number is a second opinion about it.
// oloSkinEvaluatesOralSurface accepts version 5 precisely so that this works.
// =============================================================================

// The LINEAR epsilon, for lengths, axial components and interval widths. NOT
// kDegenerateEpsilon (1e-20), which guards SQUARED lengths: comparing a length
// against 1e-20 is comparing it against zero, and a tangential component of
// 1e-12 would then pass a usability test and be divided by. Mirrors
// kOcularLinearEpsilon in Renderer/SkinOcularSurface.cpp.
const float kOcularLinearEpsilon = 1e-6;

// What one eye pixel resolved to. Mirrors SkinOcularResult.
//
// `IrisRadial` is NEGATIVE where the pixel is not on the iris — a sentinel
// rather than a companion bool, for the reason the CPU struct gives: a caller
// that has to read two fields to know whether the third is meaningful gets it
// wrong once.
struct OloSkinOcular
{
    vec3 Albedo;
    vec3 Normal;
    float IrisRadial;
};

// The corneal dome's normal at a point whose GLOBE normal is `globeNormal`.
// Mirrors SkinCornealNormal.
//
//     sin(phi) = curvatureRatio * sin(theta)
//
// See Renderer/SkinOcularSurface.h for the derivation. A ratio of exactly 1
// returns the input UNTOUCHED, which is both the neutral case and the correct
// answer for a mesh that already carries corneal geometry.
vec3 oloSkinCornealNormal(vec3 globeNormal, vec3 axis, float curvatureRatio)
{
    float ratio = clamp(curvatureRatio, 1.0, 4.0);
    if (ratio == 1.0)
        return globeNormal;

    vec3 n = normalize(globeNormal);
    vec3 a = normalize(axis);

    float cosTheta = clamp(dot(n, a), -1.0, 1.0);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

    // The tangential direction, away from the axis. Exactly on the axis there
    // is none, and there is nothing to bend either: the apex's normal IS the
    // optical axis.
    vec3 tangential = n - a * cosTheta;
    float tangentialLen = length(tangential);
    if (!(tangentialLen > kOcularLinearEpsilon))
        return globeNormal;

    float sinPhi = clamp(ratio * sinTheta, 0.0, 1.0);
    float cosPhi = sqrt(max(0.0, 1.0 - sinPhi * sinPhi));

    vec3 bent = a * cosPhi + (tangential / tangentialLen) * sinPhi;
    if (!(dot(bent, bent) > kDegenerateEpsilon))
        return globeNormal;
    return normalize(bent);
}

// Snell's law. Mirrors SkinOcularRefract.
//
// WRITTEN OUT RATHER THAN CALLING THE GLSL BUILTIN `refract()`, and that is a
// parity decision rather than a stylistic one. The builtin returns vec3(0.0) on
// total internal reflection, so a caller has to test the RESULT to learn what
// happened — and a zero direction marches nowhere, landing on the entry point,
// which shades as a painted iris. Detecting it by testing `k` directly, the way
// the CPU does, is the same arithmetic with the failure made visible.
//
// `incident` points ALONG the ray, INTO the surface. A caller holding a view
// vector passes its negation, ONCE, at the call site.
//
// Returns false on total internal reflection and leaves `refracted` untouched.
bool oloSkinOcularRefract(vec3 incident, vec3 normal, float eta, out vec3 refracted)
{
    refracted = vec3(0.0);
    if (!(dot(incident, incident) > kDegenerateEpsilon))
        return false;
    if (!(dot(normal, normal) > kDegenerateEpsilon))
        return false;

    vec3 i = normalize(incident);
    vec3 n = normalize(normal);

    float cosI = -dot(n, i);
    float k = 1.0 - eta * eta * (1.0 - cosI * cosI);
    // `!(k >= 0.0)` rather than `k < 0.0`, so a NaN reports total internal
    // reflection instead of propagating into a square root.
    if (!(k >= 0.0))
        return false;

    vec3 o = eta * i + (eta * cosI - sqrt(k)) * n;
    if (!(dot(o, o) > kDegenerateEpsilon))
        return false;

    refracted = normalize(o);
    return true;
}

// Where a refracted ray lands on the iris plane, in EYE RADII. Mirrors
// SkinIrisPlaneHit.
//
// The whole march is scale-free: in units of the eye's own radius the entry
// point on a unit sphere IS the normal, so this needs no vertex position, no
// eye centre and no world size. `irisPlaneDepth` is measured from the apex, so
// the plane sits at axial coordinate `1 - irisPlaneDepth`.
bool oloSkinIrisPlaneHit(vec3 globeNormal, vec3 axis, vec3 refracted, float irisPlaneDepth, out vec3 hit)
{
    hit = vec3(0.0);

    vec3 a = normalize(axis);
    vec3 entry = normalize(globeNormal);
    vec3 dir = normalize(refracted);

    float depth = clamp(irisPlaneDepth, 0.01, 0.9);
    float planeAxial = 1.0 - depth;

    float entryAxial = dot(entry, a);
    float dirAxial = dot(dir, a);
    // The ray must travel INWARD. Solving for t anyway would place the iris in
    // FRONT of the eye — a landing point that is finite, continuous and behind
    // the camera, which is the shape of bug that survives a screenshot.
    if (!(dirAxial < -kOcularLinearEpsilon))
        return false;

    float t = (entryAxial - planeAxial) / -dirAxial;
    if (!(t >= 0.0))
        return false;

    hit = entry + dir * t;
    return true;
}

// How much of the iris disc this disc coordinate is inside: 1 well within the
// iris, 0 at and beyond its edge. Mirrors SkinIrisDiscMask.
//
// The ONE definition of "where the iris stops" on this side, and it shares its
// boundary with oloSkinIrisLimbalRing below — the iris edge and the limbal ring
// are the same limbus, so two definitions could have disagreed about it.
float oloSkinIrisDiscMask(float radial, float band)
{
    // The SAME [0.02, 0.45] the ring uses — see SkinOcularTintLane: the iris
    // edge and the limbal ring are one boundary, so one clamp.
    float clampedBand = clamp(band, 0.02, 0.45);
    return 1.0 - smoothstep(1.0 - clampedBand, 1.0, radial);
}

// How much iris albedo survives the limbal ring. Mirrors SkinIrisLimbalRing.
//
// A strength of 0 returns EXACTLY 1 — not a smoothstep multiplied by zero and
// subtracted, exactly 1 — so a profile with no ring is bit-identical to one
// with no ring code.
// THE BAND IS FLOORED AT 0.02 AND NOT AT 0, and that floor is load-bearing
// rather than tidy: smoothstep(a, a, x) divides by a zero span, which is
// UNDEFINED in GLSL and comes back NaN on this driver, while the CPU's
// SmoothStep guards the case explicitly and returns a clean 0 or 1. A zero
// width with a non-zero strength would therefore be a NaN albedo on the GPU and
// a correct frame on the CPU — a parity divergence reachable from an authored
// profile. kMinSkinLimbalRingWidthRatio carries the same floor so the lane
// cannot arrive below it either.
//
// The band sits ONE BAND-WIDTH INSIDE the iris edge so that
// oloSkinIrisDiscMask's fade over [1 - band, 1] has somewhere to work; see the
// call site.
float oloSkinIrisLimbalRing(float radial, float band, float strength)
{
    float clampedStrength = clamp(strength, 0.0, 1.0);
    if (!(clampedStrength > 0.0))
        return 1.0;
    float clampedBand = clamp(band, 0.02, 0.45);
    return 1.0 - clampedStrength * smoothstep(1.0 - 2.0 * clampedBand, 1.0 - clampedBand, radial);
}

// How much iris albedo survives the pupil. Mirrors SkinIrisPupilMask.
//
// The soft edge is an anti-aliasing measure and not a look: a real pupil margin
// is tens of microns and would be a hard step at any render resolution, which
// crawls under a temporal upscaler. Its band scales with the pupil so a
// constricted pupil does not become all edge.
float oloSkinIrisPupilMask(float radial, float pupilRadial, float darkening)
{
    float clampedDarkening = clamp(darkening, 0.0, 1.0);
    if (!(clampedDarkening > 0.0))
        return 1.0;

    float edge = clamp(pupilRadial, 0.02, 0.95);
    float band = max(edge * 0.06, kOcularLinearEpsilon);
    float inside = 1.0 - smoothstep(edge - band, edge + band, radial);
    return 1.0 - clampedDarkening * inside;
}

// The shading normal on the iris, tilted by its dish. Mirrors
// SkinIrisShadingNormal.
//
// A PERTURBATION OF THE CORNEAL NORMAL, NOT A REPLACEMENT. The surface being
// shaded is still the cornea, and the bright highlight on an eye IS the corneal
// reflection — a shader that swapped in an iris-plane normal would slide that
// highlight across the eye as the tilt changed, which is the single most
// recognisable way for an eye to look wrong. So the tilt is added and bounded,
// giving the iris a diffuse gradient that moves with the light while leaving
// the specular where the cornea put it.
//
// Ramped to zero at the iris edge, which makes the dish a paraboloid rather
// than a cone: a cone's slope is constant, so its tilt would not vanish at the
// limbus and the normal would step across that boundary — a hard ring at
// exactly the radius the limbal ring is already drawing attention to.
vec3 oloSkinIrisShadingNormal(vec3 normal, vec3 axis, vec3 irisPlanePoint, float irisRadius, float concavity)
{
    float clampedConcavity = clamp(concavity, 0.0, 0.5);
    if (!(clampedConcavity > 0.0))
        return normal;
    if (!(irisRadius > 0.0))
        return normal;

    vec3 a = normalize(axis);
    vec3 perpendicular = irisPlanePoint - a * dot(irisPlanePoint, a);
    float perpendicularLen = length(perpendicular);
    if (!(perpendicularLen > kOcularLinearEpsilon))
        return normal;

    vec3 outward = perpendicular / perpendicularLen;
    float edgeRamp = clamp(1.0 - perpendicularLen / max(irisRadius, kOcularLinearEpsilon), 0.0, 1.0);
    return normalize(normal + outward * (clampedConcavity * edgeRamp));
}

// Resolve one eye pixel. Mirrors ApplySkinOcularSurface, and the ONE place the
// order of the three terms is written on this side.
//
// `view` points FROM the surface TOWARD the eye; the incident direction handed
// to the refraction is its negation, once, here.
OloSkinOcular oloSkinOcularApply(vec3 albedo, vec3 N, vec3 V, vec3 axis,
                                 vec4 corneaLane, vec4 irisLane, vec4 responseLane,
                                 vec4 tintLane)
{
    OloSkinOcular result = OloSkinOcular(albedo, N, -1.0);

    float ocularStrength = irisLane.w;
    // A zero master returns the inputs UNTOUCHED — not blended with themselves,
    // untouched — so a version-5 profile whose author has not asked for an eye
    // is bit-identical to the version-4 frame and costs one compare rather than
    // a refraction.
    if (!(ocularStrength > 0.0))
        return result;
    if (!(dot(axis, axis) > kDegenerateEpsilon))
        return result;

    vec3 n = normalize(N);
    vec3 v = normalize(V);
    vec3 a = normalize(axis);

    // Cornea, or sclera? The limbus is a real anatomical boundary and it is
    // also this feature's boundary: outside it there is no anterior chamber and
    // nothing to refract, and the surface is scleral collagen — which is
    // version-4 skin and is already shading correctly. Returned UNTOUCHED, not
    // "refracted by zero", so an eye's white stays a skin term.
    float cosTheta = dot(n, a);
    if (!(cosTheta >= corneaLane.w))
        return result;

    vec3 cornealNormal = oloSkinCornealNormal(n, a, corneaLane.y);
    vec3 refracted;
    if (!oloSkinOcularRefract(-v, cornealNormal, corneaLane.x, refracted))
        return result;

    vec3 refractedHit;
    if (!oloSkinIrisPlaneHit(n, a, refracted, corneaLane.z, refractedHit))
        return result;

    // The iris point, on the quality ladder. Interpolated as a PERPENDICULAR
    // OFFSET and not as a 3D point, because the two endpoints do not lie on the
    // same plane: the painted arm reads the iris at the surface point's own
    // lateral coordinate and the refracted arm reads it on the iris plane.
    // Interpolating the offsets keeps the result on the iris plane for every
    // value of the ladder, so crossing it is continuous and never lifts the
    // iris off its own plane.
    float refractionStrength = clamp(responseLane.w, 0.0, 1.0);
    vec3 paintedOffset = n - a * dot(n, a);
    vec3 refractedOffset = refractedHit - a * dot(refractedHit, a);
    vec3 offset = paintedOffset + (refractedOffset - paintedOffset) * refractionStrength;

    float planeAxial = 1.0 - clamp(corneaLane.z, 0.01, 0.9);
    vec3 irisPoint = a * planeAxial + offset;

    float irisRadius = irisLane.x;
    if (!(irisRadius > 0.0))
        return result;
    float radial = length(offset) / irisRadius;

    // THE ORDER HERE IS THE LAYER ORDER, and it is fixed rather than
    // depth-sorted: the refraction above decided WHICH iris point this is, and
    // the ring, the pupil and the tilt are all properties OF THAT POINT.
    float ring = oloSkinIrisLimbalRing(radial, irisLane.z, responseLane.x);
    float pupil = oloSkinIrisPupilMask(radial, irisLane.y, responseLane.y);

    float master = clamp(ocularStrength, 0.0, 1.0);
    float irisGain = ring * pupil;

    // THE IRIS TINT, and the DISC MASK that fades the whole iris response out
    // at the limbus so the sclera keeps whatever the material authored.
    //
    // FADING ALL THREE TERMS TOGETHER, not just the tint: the limbus is reached
    // by two different boundaries — the geometric cone test on the surface
    // normal above, and this radial coordinate on the REFRACTED iris point —
    // and they do not coincide, so any term still non-neutral when either is
    // crossed leaves a hard step. Over [1 - band, 1] all three reach exactly 1,
    // which is what the sclera outside already has.
    vec3 tint = clamp(tintLane.rgb, vec3(0.0), vec3(1.0));
    vec3 discGain = tint * irisGain;
    vec3 faded = vec3(1.0) + (discGain - vec3(1.0)) * oloSkinIrisDiscMask(radial, tintLane.w);

    // `1 + master * (gain - 1)` rather than `mix(1, gain, master)`, for the
    // reason oloSkinOralCavityWeight is written that way: at master == 0 the
    // first is 1 EXACTLY for every finite gain.
    result.Albedo = albedo * (vec3(1.0) + master * (faded - vec3(1.0)));
    result.Normal = oloSkinIrisShadingNormal(n, a, irisPoint, irisRadius, responseLane.z * master);
    result.IrisRadial = radial;
    return result;
}

// Whether a pixel evaluates the ocular terms at all.
//
// THE ONE PLACE THE VERSION TEST IS SPELLED on this side, mirroring
// SkinEvaluatesOcularSurface in Renderer/SkinOcularSurface.h — same name, same
// shape — so the forward, clustered and deferred material stages cannot
// disagree about which pixels are eyes.
//
// A BOOL GATE RATHER THAN A vec4 PASS-THROUGH, and this is not a style choice:
// include/SkinOralSurface.glsl records that the obvious
// `vec4 oloSkinOralLaneFor(int, int, vec4)` spelling MISCOMPILED on this box
// (NVIDIA, OpenGL 4.6, through the SPIR-V -> SPIRV-Cross -> GLSL round trip),
// silently killing the skin specular tint seventy lines away with no data
// dependency on any of it. The shape that works is a bool gate with the lane
// selected by a ternary at the call site, so that is the shape this file ships
// — and with THREE lanes to select the temptation to write the helper the other
// way was correspondingly larger.
bool oloSkinEvaluatesOcularSurface(int materialKind, int evaluationModel)
{
    return materialKind == OLO_MATERIAL_KIND_SKIN &&
           (evaluationModel == OLO_SKIN_MODEL_OCULAR_SURFACE ||
            evaluationModel == OLO_SKIN_MODEL_ISOTROPIC_GATHER);
}

#endif // SKIN_OCULAR_SURFACE_GLSL
