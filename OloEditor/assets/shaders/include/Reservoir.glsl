// The ReSTIR DI reservoir and its arithmetic — the GLSL twin of
// Renderer/ReSTIR/ReservoirDI.h (#1140). Function for function, guard for
// guard, and the PACKING at the bottom of this file is driven through a real
// RGBA32F target by ReSTIRDIReservoirGpuParityTest rather than trusted to
// agree — see that file for why a same-invocation round-trip would not do.
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

const uint OLO_RESERVOIR_LAYOUT_VERSION = 2u;

const uint OLO_LIGHT_SAMPLE_NONE = 0u;
const uint OLO_LIGHT_SAMPLE_PUNCTUAL = 1u;
const uint OLO_LIGHT_SAMPLE_DIRECTIONAL = 2u;
const uint OLO_LIGHT_SAMPLE_SPHERE_AREA = 3u;
const uint OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE = 4u;
const uint OLO_LIGHT_SAMPLE_KIND_COUNT = 5u;

const uint OLO_RESTIR_BIAS_MODE_BIASED = 0u;
const uint OLO_RESTIR_BIAS_MODE_UNBIASED_MIS = 1u;

// ReservoirDI.h's kMinimumShiftCosine. The smallest emitter-side cosine the
// Jacobian may divide by; below it the reuse is REJECTED, not scaled.
const float OLO_RESERVOIR_MIN_SHIFT_COSINE = 1.0e-4;

// Per-pixel diagnostics, packed into the reservoir's state plane so the debug
// views can show WHERE the estimator intervened rather than only that it did.
const uint OLO_RESERVOIR_DIAG_NONE = 0u;
const uint OLO_RESERVOIR_DIAG_TEMPORAL_ACCEPTED = 1u << 0u;
const uint OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED = 1u << 1u;
const uint OLO_RESERVOIR_DIAG_SPATIAL_ACCEPTED = 1u << 2u;
const uint OLO_RESERVOIR_DIAG_JACOBIAN_REJECTED = 1u << 3u;
const uint OLO_RESERVOIR_DIAG_M_CAPPED = 1u << 4u;
const uint OLO_RESERVOIR_DIAG_RADIANCE_CLAMPED = 1u << 5u;
const uint OLO_RESERVOIR_DIAG_VISIBILITY_KILLED = 1u << 6u;

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

struct OloReservoir
{
    OloLightSample Sample;
    float TargetPdf; // pHat(y) at the pixel that owns this reservoir
    float WeightSum; // running sum of candidate weights; TRANSIENT in a combine
    float M;         // confidence weight
    float W;         // unbiased contribution weight — what shading multiplies by
    uint Diagnostics;
};

bool OloReservoirFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool OloReservoirFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

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

// A sample with no area to reconnect through. ReservoirDI.h's IsDeltaLight.
bool OloLightSampleIsDelta(uint kind)
{
    return kind == OLO_LIGHT_SAMPLE_PUNCTUAL || kind == OLO_LIGHT_SAMPLE_DIRECTIONAL;
}

// -----------------------------------------------------------------------------
// Streaming RIS
// -----------------------------------------------------------------------------

// The selection test is `xi * WeightSum < weight`, NOT `xi < weight /
// WeightSum`. The first is false when WeightSum is zero — exactly the "nothing
// added yet, and this candidate is worthless" case. The second divides by zero
// and then selects on a comparison against a NaN, whose result GLSL does not
// define. The C++ twin spells it the same way for the same reason.
bool OloReservoirUpdate(inout OloReservoir r, OloLightSample s, float weight, float targetPdf, float xi)
{
    // Dropped, not clamped: clamping a garbage candidate would still fold it
    // into M, and M is a claim about how many candidates were seen.
    if (!OloReservoirFinite(weight) || weight < 0.0 || !OloReservoirFinite(targetPdf) || targetPdf < 0.0)
        return false;

    r.M += 1.0;
    r.WeightSum += weight;
    if (xi * r.WeightSum < weight)
    {
        r.Sample = s;
        r.TargetPdf = targetPdf;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Normalisation
// -----------------------------------------------------------------------------

// W = weightSum / (normaliser * targetPdf). `normaliser` is M under 1/M and 1
// under a MIS-weighted combine, where the per-candidate MIS weights already sum
// to one inside weightSum. One function for both arms is what makes a
// normaliser applied TWICE visible instead of plausible.
float OloReservoirContributionWeight(float weightSum, float normaliser, float targetPdf)
{
    if (!(weightSum > 0.0) || !(normaliser > 0.0) || !(targetPdf > 0.0))
        return 0.0;
    float w = weightSum / (normaliser * targetPdf);
    return OloReservoirFinite(w) ? w : 0.0;
}

// Candidates drawn from one source pdf at ONE pixel DO share a domain, so 1/M
// is the correct unbiased normaliser here and the bias mode does not apply.
void OloReservoirFinalizeInitial(inout OloReservoir r)
{
    r.W = OloReservoirContributionWeight(r.WeightSum, r.M, r.TargetPdf);
}

void OloReservoirFinalizeCombined(inout OloReservoir r, uint biasMode, float summedM)
{
    float normaliser = (biasMode == OLO_RESTIR_BIAS_MODE_UNBIASED_MIS) ? 1.0 : summedM;
    r.M = summedM;
    r.W = OloReservoirContributionWeight(r.WeightSum, normaliser, r.TargetPdf);
}

// Capping M WITHOUT rescaling WeightSum would multiply the pixel's radiance by
// (cap / M), brightening exactly the pixels that have been stable longest.
void OloReservoirApplyMCap(inout OloReservoir r, float cap)
{
    if (!(cap > 0.0) || !(r.M > cap))
        return;
    float scale = cap / r.M;
    r.WeightSum *= scale;
    r.M = cap;
    r.Diagnostics |= OLO_RESERVOIR_DIAG_M_CAPPED;
}

// -----------------------------------------------------------------------------
// The shift and its Jacobian
// -----------------------------------------------------------------------------

// J = (cos(phiDest) / cos(phiSource)) * (dSourceSq / dDestSq), with BOTH
// cosines taken at the EMITTER end. Taking them at the shading end is the
// plausible-looking mistake: the shading-point cosine belongs to the integrand,
// not to the measure.
//
// Returns 0 for a degenerate configuration. Zero means REJECT the reuse; it
// never means "no change", which 1 would.
float OloReservoirShiftJacobian(OloLightSample s, vec3 destShadingPoint, vec3 sourceShadingPoint)
{
    if (OloLightSampleIsDelta(s.Kind))
        return 1.0;

    vec3 toDest = destShadingPoint - s.Position;
    vec3 toSource = sourceShadingPoint - s.Position;
    float destDistanceSq = dot(toDest, toDest);
    float sourceDistanceSq = dot(toSource, toSource);
    if (!(destDistanceSq > 0.0) || !(sourceDistanceSq > 0.0))
        return 0.0;

    float normalLengthSq = dot(s.Normal, s.Normal);
    if (!(normalLengthSq > 0.0))
        return 0.0;
    vec3 n = s.Normal * inversesqrt(normalLengthSq);

    float cosDest = abs(dot(n, toDest * inversesqrt(destDistanceSq)));
    float cosSource = abs(dot(n, toSource * inversesqrt(sourceDistanceSq)));
    if (!(cosSource > OLO_RESERVOIR_MIN_SHIFT_COSINE))
        return 0.0;

    float jacobian = (cosDest / cosSource) * (sourceDistanceSq / destDistanceSq);
    return (OloReservoirFinite(jacobian) && jacobian >= 0.0) ? jacobian : 0.0;
}

// Carry a reservoir's contribution weight through the shift.
//
// THE JACOBIAN MULTIPLIES W. IT DOES NOT DIVIDE THE TARGET FUNCTION. The two
// differ by a factor of J and both produce a plausible image; ReservoirDI.h's
// ShiftedContributionWeight carries the derivation.
float OloShiftedContributionWeight(float sourceW, float jacobian)
{
    if (!(sourceW > 0.0) || !(jacobian > 0.0))
        return 0.0;
    float w = sourceW * jacobian;
    return OloReservoirFinite(w) ? w : 0.0;
}

// areaPdf * dSq / cos — the conversion the measure identity in
// ReSTIRDIContractTest pins the Jacobian against.
float OloAreaPdfToSolidAnglePdf(float areaPdf, OloLightSample s, vec3 shadingPoint)
{
    if (OloLightSampleIsDelta(s.Kind))
        return areaPdf;
    if (!(areaPdf > 0.0))
        return 0.0;
    vec3 toShading = shadingPoint - s.Position;
    float distanceSq = dot(toShading, toShading);
    float normalLengthSq = dot(s.Normal, s.Normal);
    if (!(distanceSq > 0.0) || !(normalLengthSq > 0.0))
        return 0.0;
    float cosLight = abs(dot(s.Normal * inversesqrt(normalLengthSq), toShading * inversesqrt(distanceSq)));
    if (!(cosLight > OLO_RESERVOIR_MIN_SHIFT_COSINE))
        return 0.0;
    float pdf = areaPdf * distanceSq / cosLight;
    return OloReservoirFinite(pdf) ? pdf : 0.0;
}

// -----------------------------------------------------------------------------
// The packed screen-space layout — OLO_RESERVOIR_LAYOUT_VERSION 1
// -----------------------------------------------------------------------------
//
// Three RGBA32F planes per reservoir, ping-ponged through the temporal-history
// registry. Float planes rather than a uint image because the render graph's
// history extraction, the resize path and the readback helpers all already
// speak RGBA32F attachments, and a reservoir is mostly floats anyway; the two
// integer fields ride in bit-reinterpreted lanes.
//
//   plane 0  xyz = Sample.Position          w = bits(Kind | LightIndex << 3)
//   plane 1  xyz = Sample.Radiance          w = bits(oct-packed Sample.Normal)
//   plane 2  x   = W   y = M   z = TargetPdf  w = bits(Diagnostics)
//
// Kind occupies the low 3 bits, which is exactly enough for
// OLO_LIGHT_SAMPLE_KIND_COUNT and leaves 29 bits of light index — more than the
// GPU Scene light table or the emissive triangle table can address.

// NUMERIC PACKING, NEVER uintBitsToFloat. THIS IS THE WHOLE REASON THIS BLOCK
// LOOKS THE WAY IT DOES.
//
// The obvious way to put an integer in an RGBA32F lane is to reinterpret its
// bits. It does not survive: `kind | lightIndex << 3` is a SMALL integer, and a
// small integer reinterpreted as a float is a DENORMAL (401 becomes 5.6e-43).
// GPUs are permitted to flush denormals to zero and this one does — so the
// reservoir's identity read back as kind 0 (None), every downstream unpack saw
// an EMPTY reservoir, the resolve wrote black, and the tier silently did
// nothing while every counter said it was active. Measured on Vulkan before
// this comment existed; nothing in the log, and M survived because M is stored
// as an ordinary float.
//
// So every integer here is stored as its own NUMERIC value. An f32 represents
// every integer up to 2^24 exactly, and no value in the encoding is ever
// denormal.
//
// 2^24 is NOT generous headroom here — the normal lane's largest value is
// exactly 2^24 - 1. Nothing in this encoding may grow without moving to a second
// lane, and nothing may round a lane by adding 0.5 (see
// OloReservoirRoundToInteger). Both traps have already been sprung once.
const uint OLO_RESERVOIR_KIND_BITS = 3u;
const uint OLO_RESERVOIR_KIND_MASK = 7u;
// 12 bits per octahedral axis: about 0.05% of angular resolution, which the
// Jacobian's cosine does not notice. The largest encoded value is
// 4095*4096 + 4095 = 16 777 215, which is 2^24 - 1: the LAST integer an f32
// still counts by ones. Not 16 773 120, which is what this comment said
// while the top of the range was quietly being rounded off the end of it —
// see OloReservoirRoundToInteger below.
const float OLO_RESERVOIR_OCT_SCALE = 4095.0;
const float OLO_RESERVOIR_OCT_STRIDE = 4096.0;
// A delta light has no emitter normal. -1 is outside the encoding's range, so it
// cannot collide with a real value the way 0 would (0 is a legitimate normal).
const float OLO_RESERVOIR_NO_NORMAL = -1.0;

// ROUNDING A LANE THAT IS ALREADY AN EXACT INTEGER.
//
// Every integer in this encoding is stored numerically and read back with a
// defensive round, because a lane that has been through a render target should
// land on the integer it started as rather than one below it. The obvious round
// is `+ 0.5` then truncate, and it is WRONG over the top half of this encoding's
// domain.
//
// At or above 2^23 an f32 has no fractional part left: consecutive
// representable values are one apart. Adding 0.5 therefore does not nudge the
// value toward the nearest integer — it lands exactly halfway between two
// representable numbers and rounds to EVEN, which silently flips the low bit.
// In the identity lane that low bit is the sample KIND, so a punctual light read
// back as directional. In the normal lane the low bit is the octahedral y, and
// at y = 4095 it carries into x: the decoded normal jumps to the far side of the
// octahedron, roughly perpendicular to the one that was stored.
//
// Both lanes reach 2^24 - 1 by design (4095 * 4096 + 4095 for a normal), so this
// is not a corner case — it is most of the domain. ReSTIRDIReservoirGpuParityTest
// probes that top end deliberately, which is how this was found.
const float OLO_RESERVOIR_EXACT_INTEGER_LIMIT = 8388608.0; // 2^23

float OloReservoirRoundToInteger(float packed)
{
    float v = max(packed, 0.0);
    return (v < OLO_RESERVOIR_EXACT_INTEGER_LIMIT) ? floor(v + 0.5) : v;
}

float OloPackReservoirIdentity(uint kind, uint lightIndex)
{
    return float((kind & OLO_RESERVOIR_KIND_MASK) | (lightIndex << OLO_RESERVOIR_KIND_BITS));
}

void OloUnpackReservoirIdentity(float packed, out uint kind, out uint lightIndex)
{
    // Rounded, not truncated: the value went through a render target and back.
    uint bits = uint(OloReservoirRoundToInteger(packed));
    kind = bits & OLO_RESERVOIR_KIND_MASK;
    lightIndex = bits >> OLO_RESERVOIR_KIND_BITS;
}

float OloPackReservoirNormal(vec3 n)
{
    float lengthSq = dot(n, n);
    if (!(lengthSq > 0.0))
        return OLO_RESERVOIR_NO_NORMAL;
    n *= inversesqrt(lengthSq);
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    if (n.z < 0.0)
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    vec2 q = floor(clamp(p * 0.5 + 0.5, 0.0, 1.0) * OLO_RESERVOIR_OCT_SCALE + 0.5);
    return q.x * OLO_RESERVOIR_OCT_STRIDE + q.y;
}

vec3 OloUnpackReservoirNormal(float packed)
{
    if (packed < 0.0)
        return vec3(0.0);
    float v = OloReservoirRoundToInteger(packed);
    float qx = floor(v / OLO_RESERVOIR_OCT_STRIDE);
    float qy = v - qx * OLO_RESERVOIR_OCT_STRIDE;
    vec2 p = (vec2(qx, qy) / OLO_RESERVOIR_OCT_SCALE) * 2.0 - 1.0;
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    float lengthSq = dot(n, n);
    return (lengthSq > 0.0) ? n * inversesqrt(lengthSq) : vec3(0.0);
}

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
