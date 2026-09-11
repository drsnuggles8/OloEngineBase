// The ReSTIR GI reservoir and the arithmetic that depends on what its sample IS
// — the GLSL twin of Renderer/ReSTIR/ReservoirGI.h (#1169). Function for
// function, guard for guard, and the PACKING is driven through a real RGBA32F
// target by ReSTIRGIReservoirGpuParityTest rather than trusted to agree.
//
// READ docs/design/restir-gi-reconnection-shift.md FIRST. It derives the
// measure, the shift and the Jacobian. In one line: the reservoir stores the
// SAMPLE VERTEX x1 (a point, area measure), the target function is evaluated in
// SOLID ANGLE at the shading point, and OloGIShiftJacobian is where the two
// meet.
//
// The domain-neutral half — streaming RIS, the normalisers, the M cap, the
// GEOMETRIC CORE of the Jacobian and the float-lane encoding — is
// include/ReservoirCore.glsl's, shared verbatim with DI.
//
// Layout version and the enum values are PACKED INTO TEXTURES. Append, never
// renumber; bump OLO_GI_RESERVOIR_LAYOUT_VERSION and the C++ constant together.
#ifndef OLO_RESERVOIR_GI_GLSL
#define OLO_RESERVOIR_GI_GLSL

#include "ReservoirCore.glsl"

// Separate from OLO_RESERVOIR_LAYOUT_VERSION on purpose: the two layouts share
// an ENCODING but not a SHAPE (DI's identity lane carries a light index, GI's
// carries a sample age), so a DI packing change must not invalidate GI history.
const uint OLO_GI_RESERVOIR_LAYOUT_VERSION = 1u;

const uint OLO_GI_SAMPLE_NONE = 0u;
// The bounce ray hit geometry. Position is a POINT; Normal is the vertex normal.
const uint OLO_GI_SAMPLE_SURFACE_HIT = 1u;
// The bounce ray escaped. Position is the unit DIRECTION; Normal has no meaning.
const uint OLO_GI_SAMPLE_ENVIRONMENT = 2u;
const uint OLO_GI_SAMPLE_KIND_COUNT = 3u;

// The GI-specific diagnostics, above the shared bits the core defines. Each one
// marks a place this tier deliberately departs from the raw estimate, and each
// has a debug view — #979's non-goal is that a tier whose interventions cannot
// be looked at is "one noisy sample plus an opaque denoiser".
const uint OLO_GI_DIAG_DOMAIN_REJECTED = OLO_RESERVOIR_DIAG_DOMAIN_0 << 0u;  // §3: shift out of domain
const uint OLO_GI_DIAG_AGE_EXPIRED = OLO_RESERVOIR_DIAG_DOMAIN_0 << 1u;      // §10: the second staleness bound
const uint OLO_GI_DIAG_GLOSSY_VERTEX = OLO_RESERVOIR_DIAG_DOMAIN_0 << 2u;    // the dropped specular lobe at x1
const uint OLO_GI_DIAG_ENVIRONMENT = OLO_RESERVOIR_DIAG_DOMAIN_0 << 3u;      // the survivor escaped to the sky

// ReservoirGI.h's kMaxSampleAgeFrames. The age lane saturates here rather than
// wrapping: an age that wrapped would make the OLDEST samples look the freshest,
// which is the one failure an age cap exists to prevent, arriving through the
// cap itself.
const uint OLO_GI_MAX_SAMPLE_AGE = 4096u;

struct OloGISample
{
    uint Kind;
    // Frames since the vertex was traced. Lineage (#1169) and the quantity the
    // age cap bounds.
    uint Age;
    // The sample vertex, RENDER-RELATIVE (issue #429), or — for an ENVIRONMENT
    // sample — the unit direction the ray escaped along. Which reading applies
    // comes from Kind and never from inspecting the vector.
    vec3 Position;
    // The vertex normal, outward. Zero for an environment sample.
    vec3 Normal;
    // L_o(x1): emission plus the DIFFUSE lobe's outgoing radiance and nothing
    // else, so it does not depend on where it is being reused from. The full
    // closure at x1 would make it depend on x0 and reuse would transplant a
    // view-dependent highlight onto a pixel that is not at that view.
    vec3 Radiance;
};

// The member NAMES are the interface OLO_DEFINE_RESERVOIR_STATE_OPS binds to,
// and they are deliberately identical to OloReservoir's: the four state
// operations are the SAME code for both domains and a rename would fork them.
struct OloGIReservoir
{
    OloGISample Sample;
    float TargetPdf;
    float WeightSum;
    float M;
    float W;
    uint Diagnostics;
};

OloGISample OloMakeEmptyGISample()
{
    OloGISample s;
    s.Kind = OLO_GI_SAMPLE_NONE;
    s.Age = 0u;
    s.Position = vec3(0.0);
    s.Normal = vec3(0.0);
    s.Radiance = vec3(0.0);
    return s;
}

OloGIReservoir OloMakeEmptyGIReservoir()
{
    OloGIReservoir r;
    r.Sample = OloMakeEmptyGISample();
    r.TargetPdf = 0.0;
    r.WeightSum = 0.0;
    r.M = 0.0;
    r.W = 0.0;
    r.Diagnostics = OLO_RESERVOIR_DIAG_NONE;
    return r;
}

bool OloGIReservoirIsEmpty(OloGIReservoir r)
{
    return r.Sample.Kind == OLO_GI_SAMPLE_NONE || !(r.M > 0.0);
}

// GI's DEGENERATE ARM of the shift, DERIVED rather than copied from DI's
// delta-light arm. Let x1 = x0 + t*w and take t -> infinity: both distances grow
// at the same rate so dSourceSq/dDestSq -> 1, and the vertex normal opposes w
// from both shading points so both cosines -> 1. Hence J = 1 exactly, and the
// shift carries the DIRECTION unchanged.
bool OloGISampleIsDistant(uint kind)
{
    return kind == OLO_GI_SAMPLE_ENVIRONMENT;
}

// -----------------------------------------------------------------------------
// Streaming RIS and normalisation — the core's, under GI's names
// -----------------------------------------------------------------------------
//
// Defines OloGIReservoirUpdate, OloGIReservoirFinalizeInitial,
// OloGIReservoirFinalizeCombined and OloGIReservoirApplyMCap. The bodies,
// including the `xi * WeightSum < weight` selection test and its reason, are in
// include/ReservoirCore.glsl — the same instantiation DI makes, so the two
// cannot drift.
OLO_DEFINE_RESERVOIR_STATE_OPS(OloGIReservoir, OloGISample, OloGIReservoir)

// -----------------------------------------------------------------------------
// The shift — GI's wrapper around the geometric core
// -----------------------------------------------------------------------------

// Whether the reconnection x0' -> x1 is inside the shift's DOMAIN at all
// (design note §3). A shift that quietly returns something for an out-of-domain
// input is how light leaks through a wall with a perfectly smooth falloff.
//
// The two conditions decidable from geometry alone: the vertex is far enough
// away for the Jacobian to be conditioned (§6.3 — dDestSq is in the
// DENOMINATOR, and x1 can be centimetres away), and it is on the destination's
// upper hemisphere. The third — that x1 is actually VISIBLE from x0' — costs a
// ray and is OloGIReconnectionVisible's, in ReSTIRGICommon.glsl.
bool OloGIReconnectionInDomain(OloGISample s, vec3 destShadingPoint, vec3 destNormal, float minimumDistance)
{
    if (s.Kind == OLO_GI_SAMPLE_NONE)
        return false;
    if (OloGISampleIsDistant(s.Kind))
    {
        float lengthSq = dot(s.Position, s.Position);
        if (!(lengthSq > 0.0))
            return false;
        return dot(destNormal, s.Position * inversesqrt(lengthSq)) > 0.0;
    }

    vec3 toVertex = s.Position - destShadingPoint;
    float distanceSq = dot(toVertex, toVertex);
    float minimum = max(minimumDistance, 0.0);
    if (!(distanceSq > minimum * minimum))
        return false;
    return dot(destNormal, toVertex * inversesqrt(distanceSq)) > 0.0;
}

// J = (cos(phiDest) / cos(phiSource)) * (dSourceSq / dDestSq), with BOTH cosines
// taken at the SAMPLE VERTEX — OloReconnectionJacobian in
// include/ReservoirCore.glsl is that expression, shared verbatim with DI because
// the derivation never looks at what the vertex is.
//
// WHAT THIS WRAPPER ADDS: the ENVIRONMENT ARM, and nothing else.
//
// Returns 0 for a degenerate configuration. Zero means REJECT the reuse; it
// never means "no change", which 1 would.
float OloGIShiftJacobian(OloGISample s, vec3 destShadingPoint, vec3 sourceShadingPoint)
{
    if (OloGISampleIsDistant(s.Kind))
        return 1.0;
    if (s.Kind == OLO_GI_SAMPLE_NONE)
        return 0.0;
    return OloReconnectionJacobian(s.Position, s.Normal, destShadingPoint, sourceShadingPoint);
}

// The area-to-solid-angle conversion at a sample vertex — the single conversion
// point, and what the measure identity in ReSTIRGIContractTest pins the Jacobian
// against.
//
// An ENVIRONMENT sample was drawn in SOLID ANGLE already (a hemisphere direction
// that never landed on a surface), so there is nothing to convert.
float OloGIAreaPdfToSolidAnglePdf(float areaPdf, OloGISample s, vec3 shadingPoint)
{
    if (OloGISampleIsDistant(s.Kind))
        return areaPdf;
    return OloSolidAnglePdfFromAreaPdf(areaPdf, s.Position, s.Normal, shadingPoint);
}

// -----------------------------------------------------------------------------
// Age — the second staleness bound (design note §10)
// -----------------------------------------------------------------------------

// Saturating, never wrapping. See OLO_GI_MAX_SAMPLE_AGE.
uint OloGIAdvanceSampleAge(uint age)
{
    return (age >= OLO_GI_MAX_SAMPLE_AGE) ? OLO_GI_MAX_SAMPLE_AGE : age + 1u;
}

// `maxAge` of 0 would mean "drop everything", which is temporal reuse switched
// off by another name while still reporting itself as running. The settings
// sanitizer keeps it at 1 or more and this treats 0 the same way.
bool OloGISampleAgeAcceptable(uint age, uint maxAge)
{
    return age <= max(maxAge, 1u);
}

// -----------------------------------------------------------------------------
// The packed screen-space layout — OLO_GI_RESERVOIR_LAYOUT_VERSION 1
// -----------------------------------------------------------------------------
//
// Three RGBA32F planes, the same shape and the same ENCODING as DI's. What is
// GI's is what the identity lane's payload MEANS:
//
//   plane 0  xyz = Sample.Position        w = Kind | Age << 3
//   plane 1  xyz = Sample.Radiance        w = oct-packed Sample.Normal
//   plane 2  x   = W  y = M  z = TargetPdf  w = Diagnostics
//
// Age rather than a light index, because a GI sample does not name anything in a
// table — it IS the thing.

void OloPackGIReservoir(OloGIReservoir r, out vec4 plane0, out vec4 plane1, out vec4 plane2)
{
    plane0 = vec4(r.Sample.Position,
                  OloPackReservoirIdentity(r.Sample.Kind, min(r.Sample.Age, OLO_GI_MAX_SAMPLE_AGE)));
    // An environment sample has no vertex normal, so OloPackReservoirNormal's
    // zero-length arm writes the NO_NORMAL sentinel and the unpack reads back
    // vec3(0). That is the value OloGIShiftJacobian's environment arm never
    // looks at, so nothing downstream has to know.
    plane1 = vec4(r.Sample.Radiance, OloPackReservoirNormal(r.Sample.Normal));
    plane2 = vec4(r.W, r.M, r.TargetPdf, float(r.Diagnostics));
}

// A reservoir that cannot be trusted comes back EMPTY, never clamped: clamping
// keeps M, and a false M suppresses every future candidate at that pixel, so a
// transient corruption becomes permanent. A NaN reaching WeightSum otherwise
// spreads through spatial reuse as a growing blot.
OloGIReservoir OloUnpackGIReservoir(vec4 plane0, vec4 plane1, vec4 plane2)
{
    OloGIReservoir r = OloMakeEmptyGIReservoir();
    if (!OloReservoirFinite(plane0.xyz) || !OloReservoirFinite(plane1.xyz) || !OloReservoirFinite(plane2.x) ||
        !OloReservoirFinite(plane2.y) || !OloReservoirFinite(plane2.z))
    {
        return r;
    }

    uint kind;
    uint age;
    OloUnpackReservoirIdentity(plane0.w, kind, age);
    if (kind >= OLO_GI_SAMPLE_KIND_COUNT)
        return r;
    if (age > OLO_GI_MAX_SAMPLE_AGE)
        return r;
    if (plane2.x < 0.0 || plane2.y < 0.0 || plane2.z < 0.0)
        return r;

    r.Sample.Kind = kind;
    r.Sample.Age = age;
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

#endif // OLO_RESERVOIR_GI_GLSL
