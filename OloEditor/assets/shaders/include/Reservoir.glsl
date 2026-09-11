// The ReSTIR DI reservoir and its arithmetic — the GLSL twin of
// Renderer/ReSTIR/ReservoirDI.h (#1140). Function for function, guard for
// guard, and the PACKING is driven through a real RGBA32F target by
// ReSTIRDIReservoirGpuParityTest rather than trusted to agree — see that file
// for why a same-invocation round-trip would not do.
//
// THE DOMAIN-NEUTRAL HALF MOVED TO include/ReservoirCore.glsl (issue #1169).
// Streaming RIS, the two normalisers, the M cap, the geometric core of the
// shift and the float-lane encoding do not know what the sample is, and ReSTIR
// GI needs every one of them. Nothing this file exports was renamed: the state
// operations come back with the same names through the core's
// OLO_DEFINE_RESERVOIR_STATE_OPS, so no DI draw changed.
//
// Read ReservoirDI.h's header comment for the MEASURE CONVENTION, which is the
// whole reason the Jacobian below exists. In one line: the sample is a POINT on
// the emitter (area measure), the target function is evaluated in SOLID ANGLE
// at the shading point, and OloReservoirShiftJacobian is where the two meet.
//
// Layout version and the enum values are PACKED INTO TEXTURES. Append, never
// renumber; bump OLO_RESERVOIR_LAYOUT_VERSION and the C++ constant together.
#ifndef OLO_RESERVOIR_GLSL
#define OLO_RESERVOIR_GLSL

#include "ReservoirCore.glsl"

const uint OLO_RESERVOIR_LAYOUT_VERSION = 2u;

const uint OLO_LIGHT_SAMPLE_NONE = 0u;
const uint OLO_LIGHT_SAMPLE_PUNCTUAL = 1u;
const uint OLO_LIGHT_SAMPLE_DIRECTIONAL = 2u;
const uint OLO_LIGHT_SAMPLE_SPHERE_AREA = 3u;
const uint OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE = 4u;
const uint OLO_LIGHT_SAMPLE_KIND_COUNT = 5u;

struct OloLightSample
{
    uint Kind;
    uint LightIndex;
    // Render-relative world point on the emitter, or, for a DIRECTIONAL light,
    // the unit direction TOWARD it. Which reading applies comes from Kind and
    // never from inspecting the vector.
    vec3 Position;
    // Outward emitter normal at Position; unread for a delta light.
    vec3 Normal;
    // Radiance leaving Position toward the shading point that selected it, with
    // the emitter's texture, distance attenuation and spot cone already folded
    // in, so a reusing pixel needs no light record at all.
    vec3 Radiance;
};

// The member NAMES are the interface OLO_DEFINE_RESERVOIR_STATE_OPS binds to.
// ReservoirGI.glsl's reservoir spells them identically for that reason.
struct OloReservoir
{
    OloLightSample Sample;
    float TargetPdf; // pHat(y) at the pixel that owns this reservoir
    float WeightSum; // running sum of candidate weights; TRANSIENT in a combine
    float M;         // confidence weight
    float W;         // unbiased contribution weight — what shading multiplies by
    uint Diagnostics;
};

OloLightSample OloMakeEmptyLightSample()
{
    OloLightSample s;
    s.Kind = OLO_LIGHT_SAMPLE_NONE;
    s.LightIndex = 0u;
    s.Position = vec3(0.0);
    s.Normal = vec3(0.0);
    s.Radiance = vec3(0.0);
    return s;
}

OloReservoir OloMakeEmptyReservoir()
{
    OloReservoir r;
    r.Sample = OloMakeEmptyLightSample();
    r.TargetPdf = 0.0;
    r.WeightSum = 0.0;
    r.M = 0.0;
    r.W = 0.0;
    r.Diagnostics = OLO_RESERVOIR_DIAG_NONE;
    return r;
}

bool OloReservoirIsEmpty(OloReservoir r)
{
    return r.Sample.Kind == OLO_LIGHT_SAMPLE_NONE || !(r.M > 0.0);
}

// A sample with no area to reconnect through. ReservoirDI.h's IsDeltaLight, and
// DI's DEGENERATE ARM of the shift — the reason OloReservoirShiftJacobian is a
// wrapper around the core's OloReconnectionJacobian rather than being it.
bool OloLightSampleIsDelta(uint kind)
{
    return kind == OLO_LIGHT_SAMPLE_PUNCTUAL || kind == OLO_LIGHT_SAMPLE_DIRECTIONAL;
}

// -----------------------------------------------------------------------------
// Streaming RIS and normalisation — the core's, under DI's names
// -----------------------------------------------------------------------------
//
// Defines OloReservoirUpdate, OloReservoirFinalizeInitial,
// OloReservoirFinalizeCombined and OloReservoirApplyMCap with exactly the
// signatures and behaviour they had before the split. The bodies, including the
// `xi * WeightSum < weight` selection test and its reason, are in
// include/ReservoirCore.glsl.
OLO_DEFINE_RESERVOIR_STATE_OPS(OloReservoir, OloLightSample, OloReservoir)

// -----------------------------------------------------------------------------
// The shift and its Jacobian — DI's wrapper around the geometric core
// -----------------------------------------------------------------------------

// J = (cos(phiDest) / cos(phiSource)) * (dSourceSq / dDestSq), with BOTH
// cosines taken at the EMITTER end — OloReconnectionJacobian in
// include/ReservoirCore.glsl is that expression.
//
// WHAT THIS WRAPPER ADDS, and the only thing it adds: the DELTA-LIGHT ARM. A
// delta light is the same point in a discrete measure from both pixels, so
// there is nothing to convert. That is a statement about DI's sample kinds,
// which is why it cannot live in the core.
//
// Returns 0 for a degenerate configuration. Zero means REJECT the reuse; it
// never means "no change", which 1 would.
float OloReservoirShiftJacobian(OloLightSample s, vec3 destShadingPoint, vec3 sourceShadingPoint)
{
    if (OloLightSampleIsDelta(s.Kind))
        return 1.0;
    return OloReconnectionJacobian(s.Position, s.Normal, destShadingPoint, sourceShadingPoint);
}

// areaPdf * dSq / cos — the conversion the measure identity in
// ReSTIRDIContractTest pins the Jacobian against. The delta arm is this
// wrapper's: a delta light's "area pdf" is already a probability.
float OloAreaPdfToSolidAnglePdf(float areaPdf, OloLightSample s, vec3 shadingPoint)
{
    if (OloLightSampleIsDelta(s.Kind))
        return areaPdf;
    return OloSolidAnglePdfFromAreaPdf(areaPdf, s.Position, s.Normal, shadingPoint);
}

// -----------------------------------------------------------------------------
// The packed screen-space layout — OLO_RESERVOIR_LAYOUT_VERSION 2
// -----------------------------------------------------------------------------
//
// Three RGBA32F planes per reservoir, ping-ponged through the temporal-history
// registry. Float planes rather than a uint image because the render graph's
// history extraction, the resize path and the readback helpers all already
// speak RGBA32F attachments, and a reservoir is mostly floats anyway; the two
// integer fields ride in numerically-packed lanes.
//
//   plane 0  xyz = Sample.Position          w = Kind | LightIndex << 3
//   plane 1  xyz = Sample.Radiance          w = oct-packed Sample.Normal
//   plane 2  x   = W   y = M   z = TargetPdf  w = Diagnostics
//
// Kind occupies the low 3 bits, which is exactly enough for
// OLO_LIGHT_SAMPLE_KIND_COUNT. The encoding itself — why it is numeric and not
// bit-reinterpreted, and why the round is not `+ 0.5` — is in
// include/ReservoirCore.glsl.

void OloPackReservoir(OloReservoir r, out vec4 plane0, out vec4 plane1, out vec4 plane2)
{
    plane0 = vec4(r.Sample.Position, OloPackReservoirIdentity(r.Sample.Kind, r.Sample.LightIndex));
    plane1 = vec4(r.Sample.Radiance, OloPackReservoirNormal(r.Sample.Normal));
    plane2 = vec4(r.W, r.M, r.TargetPdf, float(r.Diagnostics));
}

// A reservoir that cannot be trusted comes back EMPTY, never clamped: clamping
// keeps M, and a false M suppresses every future candidate at that pixel, so a
// transient corruption becomes permanent. A NaN reaching WeightSum otherwise
// spreads through spatial reuse as a growing blot.
OloReservoir OloUnpackReservoir(vec4 plane0, vec4 plane1, vec4 plane2)
{
    OloReservoir r = OloMakeEmptyReservoir();
    if (!OloReservoirFinite(plane0.xyz) || !OloReservoirFinite(plane1.xyz) || !OloReservoirFinite(plane2.x) ||
        !OloReservoirFinite(plane2.y) || !OloReservoirFinite(plane2.z))
    {
        return r;
    }

    uint kind;
    uint lightIndex;
    OloUnpackReservoirIdentity(plane0.w, kind, lightIndex);
    if (kind >= OLO_LIGHT_SAMPLE_KIND_COUNT)
        return r;
    if (plane2.x < 0.0 || plane2.y < 0.0 || plane2.z < 0.0)
        return r;

    r.Sample.Kind = kind;
    r.Sample.LightIndex = lightIndex;
    r.Sample.Position = plane0.xyz;
    r.Sample.Radiance = plane1.xyz;
    r.Sample.Normal = OloUnpackReservoirNormal(plane1.w);
    r.W = plane2.x;
    r.M = plane2.y;
    r.TargetPdf = plane2.z;
    // WeightSum is NOT persisted: it is transient inside a combine, and the
    // (W, M, TargetPdf) triple is what a later combine needs. Reconstructing it
    // as W * M * TargetPdf here would look harmless and would double-apply the
    // normaliser on the next merge.
    r.WeightSum = 0.0;
    r.Diagnostics = uint(OloReservoirRoundToInteger(plane2.w));
    return r;
}

#endif // OLO_RESERVOIR_GLSL
