// ReservoirCore.glsl — the part of a ReSTIR reservoir that does not know what
// the sample IS. The GLSL twin of Renderer/ReSTIR/ReservoirCore.h, extracted
// from Reservoir.glsl for issue #1169 (ReSTIR GI), function for function.
//
// THE TEST FOR WHAT BELONGS HERE, and it is the only one: a piece belongs here
// when it contains no reference to what the sample is. Streaming RIS does not
// care whether the survivor is a point on an emitter or a one-bounce hit
// vertex; neither does the normaliser, the balance heuristic, the M cap, or the
// arithmetic that gets an integer through a float lane.
//
// WHAT DELIBERATELY DOES NOT BELONG HERE: the target function, candidate
// generation, visibility, and the DEGENERATE ARM of the shift — a delta light
// for DI, an environment sample for GI. Each domain's header owns its own.
// docs/design/restir-gi-reconnection-shift.md §8 states the split rule.
//
// WHY THE FOUR STATE OPERATIONS ARE A MACRO. GLSL has no templates and no
// generics, and the four operations below need the reservoir and sample TYPES.
// The alternative was to write them out once per domain, which would put the
// selection test — the one line in this file that is easy to get wrong and
// impossible to spot wrong — in two places. A macro is the only construct GLSL
// offers that keeps it in one. The C++ twin uses templates for the same reason
// and reaches the same shape.
#ifndef OLO_RESERVOIR_CORE_GLSL
#define OLO_RESERVOIR_CORE_GLSL

const uint OLO_RESTIR_BIAS_MODE_BIASED = 0u;
const uint OLO_RESTIR_BIAS_MODE_UNBIASED_MIS = 1u;

// ReservoirCore.h's kMinimumShiftCosine. The smallest vertex-side cosine the
// Jacobian may divide by; below it the reuse is REJECTED, not scaled.
const float OLO_RESERVOIR_MIN_SHIFT_COSINE = 1.0e-4;

// Per-pixel diagnostics, packed into a reservoir's state plane so the debug
// views can show WHERE the estimator intervened rather than only that it did.
// Shared by every domain: a bit means the same thing in DI and in GI, and a
// domain that needs a bit of its own appends above OLO_RESERVOIR_DIAG_DOMAIN_0.
const uint OLO_RESERVOIR_DIAG_NONE = 0u;
const uint OLO_RESERVOIR_DIAG_TEMPORAL_ACCEPTED = 1u << 0u;
const uint OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED = 1u << 1u;
const uint OLO_RESERVOIR_DIAG_SPATIAL_ACCEPTED = 1u << 2u;
const uint OLO_RESERVOIR_DIAG_JACOBIAN_REJECTED = 1u << 3u;
const uint OLO_RESERVOIR_DIAG_M_CAPPED = 1u << 4u;
const uint OLO_RESERVOIR_DIAG_RADIANCE_CLAMPED = 1u << 5u;
const uint OLO_RESERVOIR_DIAG_VISIBILITY_KILLED = 1u << 6u;
// The first bit a domain may claim for itself. ReservoirGI.glsl uses three.
const uint OLO_RESERVOIR_DIAG_DOMAIN_0 = 1u << 7u;

bool OloReservoirFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool OloReservoirFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
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

// -----------------------------------------------------------------------------
// The four state operations, once
// -----------------------------------------------------------------------------
//
// OLO_DEFINE_RESERVOIR_STATE_OPS(RESERVOIR, SAMPLE, PREFIX) defines
//
//     bool PREFIX##Update(inout RESERVOIR r, SAMPLE s, float weight, float targetPdf, float xi)
//     void PREFIX##FinalizeInitial(inout RESERVOIR r)
//     void PREFIX##FinalizeCombined(inout RESERVOIR r, uint biasMode, float summedM)
//     void PREFIX##ApplyMCap(inout RESERVOIR r, float cap)
//
// for a reservoir type whose members are named Sample, TargetPdf, WeightSum, M,
// W and Diagnostics. Both domains spell them identically for exactly this
// reason.
//
// THE SELECTION TEST is `xi * WeightSum < weight`, NOT `xi < weight /
// WeightSum`. The first is false when WeightSum is zero — exactly the "nothing
// added yet, and this candidate is worthless" case. The second divides by zero
// and then selects on a comparison against a NaN, whose result GLSL does not
// define. The C++ twin spells it the same way for the same reason. This is the
// line the macro exists to keep in one place.
//
// A non-finite or negative weight is DROPPED, not clamped: clamping a garbage
// candidate would still fold it into M, and M is a claim about how many
// candidates were seen.
//
// FinalizeInitial is always 1/M and takes no bias mode: candidates drawn from
// one source pdf at ONE pixel DO share a domain, so 1/M is the correct unbiased
// normaliser there and the mode does not apply. That distinction is why it is
// its own function.
//
// ApplyMCap rescales WeightSum with M. Capping M alone would multiply the
// pixel's radiance by (cap / M), brightening exactly the pixels that have been
// stable longest.
#define OLO_DEFINE_RESERVOIR_STATE_OPS(RESERVOIR, SAMPLE, PREFIX)                                            \
    bool PREFIX##Update(inout RESERVOIR r, SAMPLE s, float weight, float targetPdf, float xi)                \
    {                                                                                                        \
        if (!OloReservoirFinite(weight) || weight < 0.0 || !OloReservoirFinite(targetPdf) ||                  \
            targetPdf < 0.0)                                                                                 \
            return false;                                                                                    \
        r.M += 1.0;                                                                                          \
        r.WeightSum += weight;                                                                                \
        if (xi * r.WeightSum < weight)                                                                       \
        {                                                                                                    \
            r.Sample = s;                                                                                    \
            r.TargetPdf = targetPdf;                                                                         \
            return true;                                                                                     \
        }                                                                                                    \
        return false;                                                                                        \
    }                                                                                                        \
    void PREFIX##FinalizeInitial(inout RESERVOIR r)                                                          \
    {                                                                                                        \
        r.W = OloReservoirContributionWeight(r.WeightSum, r.M, r.TargetPdf);                                 \
    }                                                                                                        \
    void PREFIX##FinalizeCombined(inout RESERVOIR r, uint biasMode, float summedM)                           \
    {                                                                                                        \
        float normaliser = (biasMode == OLO_RESTIR_BIAS_MODE_UNBIASED_MIS) ? 1.0 : summedM;                  \
        r.M = summedM;                                                                                       \
        r.W = OloReservoirContributionWeight(r.WeightSum, normaliser, r.TargetPdf);                          \
    }                                                                                                        \
    void PREFIX##ApplyMCap(inout RESERVOIR r, float cap)                                                     \
    {                                                                                                        \
        if (!(cap > 0.0) || !(r.M > cap))                                                                    \
            return;                                                                                          \
        float scale = cap / r.M;                                                                             \
        r.WeightSum *= scale;                                                                                \
        r.M = cap;                                                                                           \
        r.Diagnostics |= OLO_RESERVOIR_DIAG_M_CAPPED;                                                        \
    }

// -----------------------------------------------------------------------------
// The shift's geometric core
// -----------------------------------------------------------------------------

// J = (cos(phiDest) / cos(phiSource)) * (dSourceSq / dDestSq), for ONE FIXED
// VERTEX seen from two shading points, with BOTH cosines taken at the VERTEX.
// Taking them at the shading points is the plausible-looking mistake: the
// shading-point cosine belongs to the integrand, not to the measure.
//
// IT HAS NO DEGENERATE-CASE ARM. A delta light (DI) and an environment sample
// (GI) both shift with J = 1, but for different reasons in different measures,
// and each domain's wrapper decides that before calling here.
//
// Returns 0 for a degenerate configuration. Zero means REJECT the reuse; it
// never means "no change", which 1 would.
float OloReconnectionJacobian(vec3 vertexPosition, vec3 vertexNormal, vec3 destShadingPoint,
                              vec3 sourceShadingPoint)
{
    vec3 toDest = destShadingPoint - vertexPosition;
    vec3 toSource = sourceShadingPoint - vertexPosition;
    float destDistanceSq = dot(toDest, toDest);
    float sourceDistanceSq = dot(toSource, toSource);
    if (!(destDistanceSq > 0.0) || !(sourceDistanceSq > 0.0))
        return 0.0;

    float normalLengthSq = dot(vertexNormal, vertexNormal);
    if (!(normalLengthSq > 0.0))
        return 0.0;
    vec3 n = vertexNormal * inversesqrt(normalLengthSq);

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
// differ by a factor of J and both produce a plausible image; ReservoirCore.h's
// ShiftedContributionWeight carries the derivation, and the design note's §4.2
// is the long version.
float OloShiftedContributionWeight(float sourceW, float jacobian)
{
    if (!(sourceW > 0.0) || !(jacobian > 0.0))
        return 0.0;
    float w = sourceW * jacobian;
    return OloReservoirFinite(w) ? w : 0.0;
}

// areaPdf * dSq / cos — the conversion the measure identity in the contract
// tests pins the Jacobian against. No degenerate arm, for the same reason
// OloReconnectionJacobian has none.
float OloSolidAnglePdfFromAreaPdf(float areaPdf, vec3 vertexPosition, vec3 vertexNormal, vec3 shadingPoint)
{
    if (!(areaPdf > 0.0))
        return 0.0;
    vec3 toShading = shadingPoint - vertexPosition;
    float distanceSq = dot(toShading, toShading);
    float normalLengthSq = dot(vertexNormal, vertexNormal);
    if (!(distanceSq > 0.0) || !(normalLengthSq > 0.0))
        return 0.0;
    float cosVertex = abs(dot(vertexNormal * inversesqrt(normalLengthSq), toShading * inversesqrt(distanceSq)));
    if (!(cosVertex > OLO_RESERVOIR_MIN_SHIFT_COSINE))
        return 0.0;
    float pdf = areaPdf * distanceSq / cosVertex;
    return OloReservoirFinite(pdf) ? pdf : 0.0;
}

// -----------------------------------------------------------------------------
// The float-lane encoding
// -----------------------------------------------------------------------------
//
// NUMERIC PACKING, NEVER uintBitsToFloat. THIS IS THE WHOLE REASON THIS BLOCK
// LOOKS THE WAY IT DOES.
//
// The obvious way to put an integer in an RGBA32F lane is to reinterpret its
// bits. It does not survive: `kind | index << 3` is a SMALL integer, and a
// small integer reinterpreted as a float is a DENORMAL (401 becomes 5.6e-43).
// GPUs are permitted to flush denormals to zero and this one does — so in #1140
// the reservoir's identity read back as kind 0 (None), every downstream unpack
// saw an EMPTY reservoir, the resolve wrote black, and the tier silently did
// nothing while every counter said it was active. Nothing in the log, and M
// survived because M is stored as an ordinary float.
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
// still counts by ones.
const float OLO_RESERVOIR_OCT_SCALE = 4095.0;
const float OLO_RESERVOIR_OCT_STRIDE = 4096.0;
// A sample with no meaningful vertex normal. -1 is outside the encoding's range,
// so it cannot collide with a real value the way 0 would (0 is a legitimate
// encoded normal).
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
// Both lanes reach 2^24 - 1 by design, so this is not a corner case — it is most
// of the domain. The GPU parity tests probe that top end deliberately, which is
// how this was found.
const float OLO_RESERVOIR_EXACT_INTEGER_LIMIT = 8388608.0; // 2^23

float OloReservoirRoundToInteger(float packed)
{
    float v = max(packed, 0.0);
    return (v < OLO_RESERVOIR_EXACT_INTEGER_LIMIT) ? floor(v + 0.5) : v;
}

float OloPackReservoirIdentity(uint kind, uint payloadIndex)
{
    return float((kind & OLO_RESERVOIR_KIND_MASK) | (payloadIndex << OLO_RESERVOIR_KIND_BITS));
}

void OloUnpackReservoirIdentity(float packed, out uint kind, out uint payloadIndex)
{
    // Rounded, not truncated: the value went through a render target and back.
    uint bits = uint(OloReservoirRoundToInteger(packed));
    kind = bits & OLO_RESERVOIR_KIND_MASK;
    payloadIndex = bits >> OLO_RESERVOIR_KIND_BITS;
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

#endif // OLO_RESERVOIR_CORE_GLSL
