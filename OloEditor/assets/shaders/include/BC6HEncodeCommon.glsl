// =============================================================================
// BC6HEncodeCommon.glsl — the GPU BC6H block encoder (#624 item 3).
//
// One invocation per 4x4 block. Reads an RGBA32F source image and writes one
// RGBA32UI texel — 16 bytes, bit-compatible with a BC6H block — per block, which
// is what lets this pass avoid the UBO and SSBO namespaces entirely: the only
// bindings it takes are two image units.
//
// This is a faithful port of Renderer/BC6HEncoder.cpp. Every table below is
// GENERATED from that file (scripts under the PR description), which in turn
// derives them from bcdec's per-mode read sequence, so the three cannot drift:
// change the C++ and regenerate. The quantization, interpolation, index selection
// and bit packing are integer arithmetic and translate one-for-one.
//
// TWO places the two paths can legitimately disagree, and both are small:
//   * the PCA fit and the least-squares refit run in `float` here where the CPU
//     uses `double` (doubles run at a fraction rate on consumer GPUs);
//   * float -> half rounding differs at the tie — glm's packHalf1x16 rounds away
//     from zero, GLSL's packHalf2x16 is round-to-nearest-even — which moves a
//     target by at most one ULP of a half.
// Measured, that leaves 99.6 % (unsigned) / 99.2 % (signed) of blocks bit-identical
// with the same PSNR to two decimals; BC6HGpuEncoderTest is what keeps it honest.
//
// Included by BC6HEncode.comp (unsigned) and BC6HEncodeSigned.comp, which differ
// only in defining OLO_BC6H_SIGNED. Two files rather than a params uniform
// precisely so no UBO binding is needed.
// =============================================================================

#include "BindlessHeap.glsl"

// Heap-bindless conversion (issue #691). Under OLO_BINDLESS the declarations
// move into main() as locals built from the heap descriptors at these image
// units; the body is unchanged either way. The source image is declared
// read-only and therefore takes OLO_HEAP_IMAGE_RW under bindless — the macro
// cannot initialise a `readonly` local (see BindlessHeap.glsl).
#ifndef OLO_BINDLESS
layout(rgba32f, binding = 0) readonly uniform image2D u_Source;
layout(rgba32ui, binding = 1) writeonly uniform uimage2D u_Blocks;
#endif

// ---- Constants shared with the CPU encoder ----------------------------------

const float kMaxFiniteHalf = 65504.0;

#ifdef OLO_BC6H_SIGNED
const bool kSigned = true;
const int kInterpMax = 0x7FFF;
const int kInterpMin = -0x7FFF;
#else
const bool kSigned = false;
const int kInterpMax = 0xFFFF;
const int kInterpMin = 0;
#endif

// aWeight3 (indices 0-7) followed by aWeight4 (8-23). A weight base of 0 selects
// the 3-bit table used by the two-subset modes, 8 the 4-bit table.
const int kWeights[24] = int[](
    0, 9, 18, 27, 37, 46, 55, 64,
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
);
const int kWeight3Base = 0;
const int kWeight4Base = 8;

// Bit t of entry p is the subset texel t belongs to under partition p.
const uint kPartitionSubsets[32] = uint[](
    0xCCCCu, 0x8888u, 0xEEEEu, 0xECC8u, 0xC880u, 0xFEECu, 0xFEC8u, 0xEC80u, 0xC800u, 0xFFECu, 0xFE80u,
    0xE800u, 0xFFE8u, 0xFF00u, 0xFFF0u, 0xF000u, 0xF710u, 0x008Eu, 0x7100u, 0x08CEu, 0x008Cu, 0x7310u,
    0x3100u, 0x8CCEu, 0x088Cu, 0x3110u, 0x6666u, 0x366Cu, 0x17E8u, 0x0FF0u, 0x718Eu, 0x399Cu
);

// The subset-1 anchor ("fixup") texel per partition; subset 0's anchor is always
// texel 0. Both store their index with the high bit dropped.
const uint kPartitionFixup[32] = uint[](
    15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u, 2u, 8u, 2u, 2u, 8u,
    8u, 15u, 2u, 8u, 2u, 2u, 8u, 8u, 2u, 2u
);

// modeBits(3) | modeValue(5)<<3 | baseBits(5)<<8 | dR(4)<<13 | dG(4)<<17
//            | dB(4)<<21 | hasDelta(1)<<25 | subsets(2)<<26
const uint kModeWords[14] = uint[](
    0x0AAAAA02u, 0x0ACCC70Au, 0x0A88AB15u, 0x0A8A8B35u, 0x0AA88B55u, 0x0AAAA975u, 0x0AAAC895u, 0x0AACA8B5u,
    0x0ACAA8D5u, 0x08CCC6F5u, 0x05554A1Du, 0x07332B3Du, 0x07110C5Du, 0x0688907Du
);

// Field-table slice per mode; the trailing entry is the total so a mode's field
// count is offsets[m + 1] - offsets[m].
const uint kFieldOffsets[15] = uint[](
    0u, 20u, 44u, 63u, 84u, 105u, 125u, 145u, 167u, 189u, 213u, 219u, 228u, 237u, 246u
);

// slot(4) | shift(6)<<4 | bits(6)<<10 | reversed(1)<<16, in bitstream order.
// Slot = endpoint * 3 + channel, with slot 12 the 5-bit partition index.
const uint kFieldWords[246] = uint[](
    0x00447u, 0x00448u, 0x0044Bu, 0x02800u, 0x02801u, 0x02802u, 0x01403u, 0x0044Au, 0x01007u, 0x01404u,
    0x0040Bu, 0x0100Au, 0x01405u, 0x0041Bu, 0x01008u, 0x01406u, 0x0042Bu, 0x01409u, 0x0043Bu, 0x0140Cu,
    0x00457u, 0x0044Au, 0x0045Au, 0x01C00u, 0x0040Bu, 0x0041Bu, 0x00448u, 0x01C01u, 0x00458u, 0x0042Bu,
    0x00447u, 0x01C02u, 0x0043Bu, 0x0045Bu, 0x0044Bu, 0x01803u, 0x01007u, 0x01804u, 0x0100Au, 0x01805u,
    0x01008u, 0x01806u, 0x01809u, 0x0140Cu, 0x02800u, 0x02801u, 0x02802u, 0x01403u, 0x004A0u, 0x01007u,
    0x01004u, 0x004A1u, 0x0040Bu, 0x0100Au, 0x01005u, 0x004A2u, 0x0041Bu, 0x01008u, 0x01406u, 0x0042Bu,
    0x01409u, 0x0043Bu, 0x0140Cu, 0x02800u, 0x02801u, 0x02802u, 0x01003u, 0x004A0u, 0x0044Au, 0x01007u,
    0x01404u, 0x004A1u, 0x0100Au, 0x01005u, 0x004A2u, 0x0041Bu, 0x01008u, 0x01006u, 0x0040Bu, 0x0042Bu,
    0x01009u, 0x00447u, 0x0043Bu, 0x0140Cu, 0x02800u, 0x02801u, 0x02802u, 0x01003u, 0x004A0u, 0x00448u,
    0x01007u, 0x01004u, 0x004A1u, 0x0040Bu, 0x0100Au, 0x01405u, 0x004A2u, 0x01008u, 0x01006u, 0x0041Bu,
    0x0042Bu, 0x01009u, 0x0044Bu, 0x0043Bu, 0x0140Cu, 0x02400u, 0x00448u, 0x02401u, 0x00447u, 0x02402u,
    0x0044Bu, 0x01403u, 0x0044Au, 0x01007u, 0x01404u, 0x0040Bu, 0x0100Au, 0x01405u, 0x0041Bu, 0x01008u,
    0x01406u, 0x0042Bu, 0x01409u, 0x0043Bu, 0x0140Cu, 0x02000u, 0x0044Au, 0x00448u, 0x02001u, 0x0042Bu,
    0x00447u, 0x02002u, 0x0043Bu, 0x0044Bu, 0x01803u, 0x01007u, 0x01404u, 0x0040Bu, 0x0100Au, 0x01405u,
    0x0041Bu, 0x01008u, 0x01806u, 0x01809u, 0x0140Cu, 0x02000u, 0x0040Bu, 0x00448u, 0x02001u, 0x00457u,
    0x00447u, 0x02002u, 0x0045Au, 0x0044Bu, 0x01403u, 0x0044Au, 0x01007u, 0x01804u, 0x0100Au, 0x01405u,
    0x0041Bu, 0x01008u, 0x01406u, 0x0042Bu, 0x01409u, 0x0043Bu, 0x0140Cu, 0x02000u, 0x0041Bu, 0x00448u,
    0x02001u, 0x00458u, 0x00447u, 0x02002u, 0x0045Bu, 0x0044Bu, 0x01403u, 0x0044Au, 0x01007u, 0x01404u,
    0x0040Bu, 0x0100Au, 0x01805u, 0x01008u, 0x01406u, 0x0042Bu, 0x01409u, 0x0043Bu, 0x0140Cu, 0x01800u,
    0x0044Au, 0x0040Bu, 0x0041Bu, 0x00448u, 0x01801u, 0x00457u, 0x00458u, 0x0042Bu, 0x00447u, 0x01802u,
    0x0045Au, 0x0043Bu, 0x0045Bu, 0x0044Bu, 0x01803u, 0x01007u, 0x01804u, 0x0100Au, 0x01805u, 0x01008u,
    0x01806u, 0x01809u, 0x0140Cu, 0x02800u, 0x02801u, 0x02802u, 0x02803u, 0x02804u, 0x02805u, 0x02800u,
    0x02801u, 0x02802u, 0x02403u, 0x004A0u, 0x02404u, 0x004A1u, 0x02405u, 0x004A2u, 0x02800u, 0x02801u,
    0x02802u, 0x02003u, 0x108A0u, 0x02004u, 0x108A1u, 0x02005u, 0x108A2u, 0x02800u, 0x02801u, 0x02802u,
    0x01003u, 0x118A0u, 0x01004u, 0x118A1u, 0x01005u, 0x118A2u
);

// How many partition shapes get a full per-mode evaluation (BC6HEncoder.cpp's
// kPartitionsEvaluated).
const int kPartitionsEvaluated = 2;

// ---- Per-invocation state ---------------------------------------------------
// Globals rather than parameters: GLSL passes arrays by value, and copying a
// 16-entry block around the inner loops costs more than the loops themselves.

ivec3 g_Target[16];  // source texels in interpolation space
ivec3 g_Q[4];        // quantized endpoints of the candidate under evaluation
int   g_Indices[16]; // its interpolation indices
ivec3 g_Fit0[2];     // per-subset endpoint fit, E0
ivec3 g_Fit1[2];     // per-subset endpoint fit, E1

// ---- Mode-word accessors ----------------------------------------------------

int ModeBitCount(uint m)  { return int(kModeWords[m] & 7u); }
int ModeBitValue(uint m)  { return int((kModeWords[m] >> 3) & 0x1Fu); }
int ModeBaseBits(uint m)  { return int((kModeWords[m] >> 8) & 0x1Fu); }
int ModeHasDelta(uint m)  { return int((kModeWords[m] >> 25) & 1u); }
int ModeSubsets(uint m)   { return int((kModeWords[m] >> 26) & 3u); }
int ModeDeltaBits(uint m, int channel)
{
    return int((kModeWords[m] >> (13 + 4 * channel)) & 0xFu);
}

// ---- Value-space conversions (mirrors BC6HEncoder.cpp) ----------------------

int CeilDiv(int a, int b)
{
    return (a + b - 1) / b;
}

// Source float -> interpolation space.
int FloatToInterp(float value)
{
    if (isinf(value) || isnan(value))
        value = value > 0.0 ? kMaxFiniteHalf : ((kSigned && value < 0.0) ? -kMaxFiniteHalf : 0.0);
    value = clamp(value, kSigned ? -kMaxFiniteHalf : 0.0, kMaxFiniteHalf);
    // Negative zero is not LESS than zero, so it survives the clamp and packs to
    // 0x8000 — read unsigned, that is the sign bit taken as a magnitude.
    if (!kSigned && !(value > 0.0))
        value = 0.0;

    int halfBits = int(packHalf2x16(vec2(value, 0.0)) & 0xFFFFu);
    if (!kSigned)
    {
        // finish_unquantize (unsigned) floors: half = (interp * 31) >> 6.
        return clamp(CeilDiv(halfBits * 64, 31), 0, kInterpMax);
    }
    int magnitude = halfBits & 0x7FFF;
    int interp = clamp(CeilDiv(magnitude * 32, 31), 0, kInterpMax);
    return (halfBits & 0x8000) != 0 ? -interp : interp;
}

int Unquantize(int value, int bits)
{
    if (!kSigned)
    {
        if (bits >= 15)
            return value;
        if (value == 0)
            return 0;
        if (value == ((1 << bits) - 1))
            return 0xFFFF;
        return ((value << 16) + 0x8000) >> bits;
    }
    if (bits >= 16)
        return value;
    bool negative = value < 0;
    int magnitude = negative ? -value : value;
    int unq;
    if (magnitude == 0)
        unq = 0;
    else if (magnitude >= ((1 << (bits - 1)) - 1))
        unq = 0x7FFF;
    else
        unq = ((magnitude << 15) + 0x4000) >> (bits - 1);
    return negative ? -unq : unq;
}

int Quantize(int target, int bits)
{
    int maxValue = kSigned ? ((1 << (bits - 1)) - 1) : ((1 << bits) - 1);
    int magnitude = min(abs(target), kSigned ? 0x7FFF : 0xFFFF);
    int guess = clamp(bits >= 16 ? magnitude : (magnitude >> (16 - bits)), 0, maxValue);

    int best = guess;
    int bestError = 0x7FFFFFFF;
    for (int c = max(0, guess - 2); c <= min(maxValue, guess + 2); ++c)
    {
        int err = abs(Unquantize(c, bits) - magnitude);
        if (err < bestError)
        {
            bestError = err;
            best = c;
        }
    }
    return (kSigned && target < 0) ? -best : best;
}

int Interpolate(int a, int b, int weight)
{
    return (a * (64 - weight) + b * weight + 32) >> 6;
}

int ClampToDeltaRange(int value, int base, int deltaBits, int baseBits)
{
    int low = base - (1 << (deltaBits - 1));
    int high = base + (1 << (deltaBits - 1)) - 1;
    int valueMin = kSigned ? -((1 << (baseBits - 1)) - 1) : 0;
    int valueMax = kSigned ? ((1 << (baseBits - 1)) - 1) : ((1 << baseBits) - 1);
    return clamp(value, max(low, valueMin), min(high, valueMax));
}

// ---- Endpoint fitting -------------------------------------------------------

// Covariance (packed rr,rg,rb,gg,gb,bb) and mean of the texels in `mask`.
// Returns the texel count.
int Moments(int mask, out vec3 outMean, out float outCov[6])
{
    outMean = vec3(0.0);
    for (int i = 0; i < 6; ++i)
        outCov[i] = 0.0;
    int count = 0;
    for (int t = 0; t < 16; ++t)
    {
        if ((mask & (1 << t)) == 0)
            continue;
        ++count;
        outMean += vec3(g_Target[t]);
    }
    if (count == 0)
        return 0;
    outMean /= float(count);

    for (int t = 0; t < 16; ++t)
    {
        if ((mask & (1 << t)) == 0)
            continue;
        vec3 d = vec3(g_Target[t]) - outMean;
        outCov[0] += d.x * d.x;
        outCov[1] += d.x * d.y;
        outCov[2] += d.x * d.z;
        outCov[3] += d.y * d.y;
        outCov[4] += d.y * d.z;
        outCov[5] += d.z * d.z;
    }
    return count;
}

// Dominant eigenvector by power iteration, seeded from the axis with the largest
// variance. Returns the eigenvalue; `outAxis` is unit length, or zero for a
// degenerate cluster.
float PrincipalAxis(float cov[6], out vec3 outAxis)
{
    vec3 variance = vec3(cov[0], cov[3], cov[5]);
    int seed = 0;
    if (variance.y > variance[seed]) seed = 1;
    if (variance.z > variance[seed]) seed = 2;
    if (variance[seed] <= 0.0)
    {
        outAxis = vec3(0.0);
        return 0.0;
    }

    vec3 v = vec3(0.0);
    v[seed] = 1.0;
    float eigenvalue = 0.0;
    for (int iteration = 0; iteration < 12; ++iteration)
    {
        vec3 next = vec3(cov[0] * v.x + cov[1] * v.y + cov[2] * v.z,
                         cov[1] * v.x + cov[3] * v.y + cov[4] * v.z,
                         cov[2] * v.x + cov[4] * v.y + cov[5] * v.z);
        float len = length(next);
        if (len <= 0.0)
        {
            outAxis = vec3(0.0);
            return 0.0;
        }
        eigenvalue = len;
        v = next / len;
    }
    outAxis = v;
    return eigenvalue;
}

ivec3 ToEndpoint(vec3 point)
{
    return clamp(ivec3(round(point)), ivec3(kInterpMin), ivec3(kInterpMax));
}

// PCA endpoint fit for one subset, oriented so `anchorTexel` sits at the E0 end —
// which satisfies the anchor-index constraint by construction rather than by a
// later endpoint swap, which would invalidate the delta encoding against
// endpoint 0.
void FitEndpoints(int mask, int anchorTexel, out ivec3 e0, out ivec3 e1)
{
    vec3 mean;
    float cov[6];
    int count = Moments(mask, mean, cov);

    vec3 axis;
    if (count == 0 || PrincipalAxis(cov, axis) <= 0.0)
    {
        e0 = ToEndpoint(mean);
        e1 = e0;
        return;
    }

    float tMin = 1e30;
    float tMax = -1e30;
    float tAnchor = 0.0;
    for (int t = 0; t < 16; ++t)
    {
        if ((mask & (1 << t)) == 0)
            continue;
        float projection = dot(vec3(g_Target[t]) - mean, axis);
        tMin = min(tMin, projection);
        tMax = max(tMax, projection);
        if (t == anchorTexel)
            tAnchor = projection;
    }
    if ((tAnchor - tMin) > (tMax - tAnchor))
    {
        float swap = tMin;
        tMin = tMax;
        tMax = swap;
    }

    e0 = ToEndpoint(mean + tMin * axis);
    e1 = ToEndpoint(mean + tMax * axis);
}

// ---- Candidate evaluation ---------------------------------------------------

// Per-texel index selection for one subset. The anchor is restricted to the
// legal (high-bit-clear) half of the range rather than clamped afterwards.
// Errors accumulate in float: the magnitudes reach ~2e11, far beyond int32, and
// float carries enough relative precision to rank candidates.
float SelectIndices(int mask, int anchorTexel, ivec3 u0, ivec3 u1, int weightBase, int weightCount)
{
    float total = 0.0;
    for (int t = 0; t < 16; ++t)
    {
        if ((mask & (1 << t)) == 0)
            continue;
        int limit = (t == anchorTexel) ? (weightCount / 2) : weightCount;
        float bestError = 1e30;
        int bestIndex = 0;
        for (int w = 0; w < limit; ++w)
        {
            int weight = kWeights[weightBase + w];
            vec3 d = vec3(ivec3(Interpolate(u0.x, u1.x, weight),
                                Interpolate(u0.y, u1.y, weight),
                                Interpolate(u0.z, u1.z, weight)) - g_Target[t]);
            float err = dot(d, d);
            if (err < bestError)
            {
                bestError = err;
                bestIndex = w;
            }
        }
        g_Indices[t] = bestIndex;
        total += bestError;
    }
    return total;
}

// Least-squares endpoint refit for one subset, holding the current indices fixed.
void RefitEndpoints(int mask, int weightBase, inout ivec3 e0, inout ivec3 e1)
{
    for (int c = 0; c < 3; ++c)
    {
        float a00 = 0.0;
        float a01 = 0.0;
        float a11 = 0.0;
        float rhs0 = 0.0;
        float rhs1 = 0.0;
        for (int t = 0; t < 16; ++t)
        {
            if ((mask & (1 << t)) == 0)
                continue;
            float w = float(kWeights[weightBase + g_Indices[t]]) / 64.0;
            float s = 1.0 - w;
            a00 += s * s;
            a01 += s * w;
            a11 += w * w;
            rhs0 += s * float(g_Target[t][c]);
            rhs1 += w * float(g_Target[t][c]);
        }
        float determinant = a00 * a11 - a01 * a01;
        if (abs(determinant) < 1e-6)
            continue; // every index identical — underdetermined; keep what we had
        e0[c] = clamp(int(round((rhs0 * a11 - rhs1 * a01) / determinant)), kInterpMin, kInterpMax);
        e1[c] = clamp(int(round((rhs1 * a00 - rhs0 * a01) / determinant)), kInterpMin, kInterpMax);
    }
}

// Quantize the current g_Fit0/g_Fit1 to the mode's precision into g_Q, pulling
// delta-coded endpoints into their storable window.
void QuantizeEndpoints(uint mode)
{
    int baseBits = ModeBaseBits(mode);
    int subsets = ModeSubsets(mode);
    for (int s = 0; s < subsets; ++s)
    {
        for (int c = 0; c < 3; ++c)
        {
            g_Q[s * 2 + 0][c] = Quantize(g_Fit0[s][c], baseBits);
            g_Q[s * 2 + 1][c] = Quantize(g_Fit1[s][c], baseBits);
        }
    }
    if (ModeHasDelta(mode) == 0)
        return;
    int endpointCount = subsets * 2;
    for (int e = 1; e < endpointCount; ++e)
    {
        for (int c = 0; c < 3; ++c)
            g_Q[e][c] = ClampToDeltaRange(g_Q[e][c], g_Q[0][c], ModeDeltaBits(mode, c), baseBits);
    }
}

// One quantize + index-selection pass over the current fit. Writes g_Q and
// g_Indices, returns the total squared error in interpolation space.
float ScoreCandidate(uint mode, int masks[2], int anchors[2], int weightBase, int weightCount)
{
    QuantizeEndpoints(mode);
    int baseBits = ModeBaseBits(mode);
    int subsets = ModeSubsets(mode);
    float total = 0.0;
    for (int s = 0; s < subsets; ++s)
    {
        ivec3 u0 = ivec3(Unquantize(g_Q[s * 2 + 0].x, baseBits),
                         Unquantize(g_Q[s * 2 + 0].y, baseBits),
                         Unquantize(g_Q[s * 2 + 0].z, baseBits));
        ivec3 u1 = ivec3(Unquantize(g_Q[s * 2 + 1].x, baseBits),
                         Unquantize(g_Q[s * 2 + 1].y, baseBits),
                         Unquantize(g_Q[s * 2 + 1].z, baseBits));
        total += SelectIndices(masks[s], anchors[s], u0, u1, weightBase, weightCount);
    }
    return total;
}

// Score one (mode, partition, fit) triple, refining while it helps. g_Q and
// g_Indices are left holding the winning encoding.
float EvaluateCandidate(uint mode, int masks[2], int anchors[2])
{
    bool oneSubset = ModeSubsets(mode) == 1;
    int weightBase = oneSubset ? kWeight4Base : kWeight3Base;
    int weightCount = oneSubset ? 16 : 8;
    int subsets = ModeSubsets(mode);

    float bestError = ScoreCandidate(mode, masks, anchors, weightBase, weightCount);

    ivec3 bestQ[4];
    int bestIndices[16];
    for (int i = 0; i < 4; ++i)
        bestQ[i] = g_Q[i];
    for (int i = 0; i < 16; ++i)
        bestIndices[i] = g_Indices[i];

    for (int iteration = 0; iteration < 2; ++iteration)
    {
        ivec3 savedFit0[2] = ivec3[2](g_Fit0[0], g_Fit0[1]);
        ivec3 savedFit1[2] = ivec3[2](g_Fit1[0], g_Fit1[1]);
        for (int s = 0; s < subsets; ++s)
            RefitEndpoints(masks[s], weightBase, g_Fit0[s], g_Fit1[s]);

        float error = ScoreCandidate(mode, masks, anchors, weightBase, weightCount);
        if (error >= bestError)
        {
            // Reject the step: restore the fit and the winning encoding.
            g_Fit0[0] = savedFit0[0]; g_Fit0[1] = savedFit0[1];
            g_Fit1[0] = savedFit1[0]; g_Fit1[1] = savedFit1[1];
            for (int i = 0; i < 4; ++i)
                g_Q[i] = bestQ[i];
            for (int i = 0; i < 16; ++i)
                g_Indices[i] = bestIndices[i];
            break;
        }
        bestError = error;
        for (int i = 0; i < 4; ++i)
            bestQ[i] = g_Q[i];
        for (int i = 0; i < 16; ++i)
            bestIndices[i] = g_Indices[i];
    }
    return bestError;
}

// Residual of the best-fit line through each subset, summed — ranks partitions
// without quantizing anything, which is what keeps the two-subset search cheap.
float PartitionResidual(uint shapeIndex)
{
    uint subsetBits = kPartitionSubsets[shapeIndex];
    int mask1 = int(subsetBits);
    int mask0 = int((~subsetBits) & 0xFFFFu);

    float residual = 0.0;
    for (int s = 0; s < 2; ++s)
    {
        int mask = (s == 0) ? mask0 : mask1;
        vec3 mean;
        float cov[6];
        if (Moments(mask, mean, cov) == 0)
            continue;
        vec3 axis;
        residual += max(0.0, (cov[0] + cov[3] + cov[5]) - PrincipalAxis(cov, axis));
    }
    return residual;
}

// ---- Bit packing ------------------------------------------------------------

uvec4 g_Packed;
int g_BitPos;

void PutBits(uint value, int bits)
{
    for (int i = 0; i < bits; ++i)
    {
        if (((value >> uint(i)) & 1u) != 0u)
            g_Packed[g_BitPos >> 5] |= (1u << uint(g_BitPos & 31));
        ++g_BitPos;
    }
}

// bcdec's read_bits_r reverses the bits it read, so writing them means reversing
// first (modes 12 and 13 only).
void PutBitsReversed(uint value, int bits)
{
    uint reversed = 0u;
    for (int i = 0; i < bits; ++i)
        reversed |= ((value >> uint(i)) & 1u) << uint(bits - 1 - i);
    PutBits(reversed, bits);
}

uvec4 PackBlock(uint mode, uint shapeIndex)
{
    g_Packed = uvec4(0u);
    g_BitPos = 0;
    PutBits(uint(ModeBitValue(mode)), ModeBitCount(mode));

    int baseBits = ModeBaseBits(mode);
    int subsets = ModeSubsets(mode);
    int endpointCount = subsets * 2;
    bool hasDelta = ModeHasDelta(mode) != 0;

    // Stored field values: endpoint 0 is the base, the rest are deltas for a
    // delta mode. Masking to the field width is also how a negative signed value
    // becomes its two's complement pattern.
    uint slots[13];
    for (int c = 0; c < 3; ++c)
    {
        uint baseMask = (baseBits >= 32) ? 0xFFFFFFFFu : ((1u << uint(baseBits)) - 1u);
        slots[c] = uint(g_Q[0][c]) & baseMask;
        for (int e = 1; e < endpointCount; ++e)
        {
            int fieldBits = hasDelta ? ModeDeltaBits(mode, c) : baseBits;
            uint fieldMask = (1u << uint(fieldBits)) - 1u;
            int stored = hasDelta ? (g_Q[e][c] - g_Q[0][c]) : g_Q[e][c];
            slots[e * 3 + c] = uint(stored) & fieldMask;
        }
    }
    for (int e = endpointCount; e < 4; ++e)
    {
        for (int c = 0; c < 3; ++c)
            slots[e * 3 + c] = 0u;
    }
    slots[12] = shapeIndex;

    uint fieldBegin = kFieldOffsets[mode];
    uint fieldEnd = kFieldOffsets[mode + 1u];
    for (uint f = fieldBegin; f < fieldEnd; ++f)
    {
        uint word = kFieldWords[f];
        int slot = int(word & 0xFu);
        int shift = int((word >> 4) & 0x3Fu);
        int bits = int((word >> 10) & 0x3Fu);
        bool reversed = ((word >> 16) & 1u) != 0u;
        uint value = (slots[slot] >> uint(shift)) & ((1u << uint(bits)) - 1u);
        if (reversed)
            PutBitsReversed(value, bits);
        else
            PutBits(value, bits);
    }

    // Indices in raster order; each anchor stores one bit fewer.
    if (subsets == 1)
    {
        PutBits(uint(g_Indices[0]), 3);
        for (int t = 1; t < 16; ++t)
            PutBits(uint(g_Indices[t]), 4);
    }
    else
    {
        int fixup = int(kPartitionFixup[shapeIndex]);
        for (int t = 0; t < 16; ++t)
        {
            bool anchor = (t == 0) || (t == fixup);
            PutBits(uint(g_Indices[t]), anchor ? 2 : 3);
        }
    }
    return g_Packed;
}

// ---- Entry point ------------------------------------------------------------

void main()
{
#ifdef OLO_BINDLESS
    OLO_HEAP_IMAGE(rgba32f, OLO_HEAP_IMAGE_RW, image2D, u_Source, 0);
    OLO_HEAP_IMAGE(rgba32ui, writeonly, uimage2D, u_Blocks, 1);
#endif

    ivec2 blockCoord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 blockGrid = imageSize(u_Blocks);
    if (blockCoord.x >= blockGrid.x || blockCoord.y >= blockGrid.y)
        return;
    ivec2 sourceSize = imageSize(u_Source);

    // Gather the 4x4 block, clamping to the edge for a partial block — the same
    // rule GatherBlockRGBFloat uses on the CPU.
    for (int ry = 0; ry < 4; ++ry)
    {
        for (int rx = 0; rx < 4; ++rx)
        {
            ivec2 texel = min(blockCoord * 4 + ivec2(rx, ry), sourceSize - ivec2(1));
            vec4 source = imageLoad(u_Source, texel);
            int t = ry * 4 + rx;
            g_Target[t] = ivec3(FloatToInterp(source.r), FloatToInterp(source.g), FloatToInterp(source.b));
        }
    }

    float bestError = 1e30;
    uint bestMode = 10u;
    uint bestPartition = 0u;
    ivec3 bestQ[4] = ivec3[4](ivec3(0), ivec3(0), ivec3(0), ivec3(0));
    int bestIndices[16];
    for (int i = 0; i < 16; ++i)
        bestIndices[i] = 0;

    // One-subset modes share a single fit over the whole block.
    {
        int masks[2] = int[2](0xFFFF, 0);
        int anchors[2] = int[2](0, 0);
        ivec3 fit0, fit1;
        FitEndpoints(0xFFFF, 0, fit0, fit1);
        for (uint mode = 0u; mode < 14u; ++mode)
        {
            if (ModeSubsets(mode) != 1)
                continue;
            g_Fit0[0] = fit0; g_Fit1[0] = fit1;
            g_Fit0[1] = fit0; g_Fit1[1] = fit1;
            float error = EvaluateCandidate(mode, masks, anchors);
            if (error < bestError)
            {
                bestError = error;
                bestMode = mode;
                bestPartition = 0u;
                for (int i = 0; i < 4; ++i)
                    bestQ[i] = g_Q[i];
                for (int i = 0; i < 16; ++i)
                    bestIndices[i] = g_Indices[i];
            }
        }
    }

    // Two-subset modes: rank the 32 shapes cheaply, then fully evaluate the best.
    int keptPartitions[kPartitionsEvaluated];
    float keptResiduals[kPartitionsEvaluated];
    for (int i = 0; i < kPartitionsEvaluated; ++i)
    {
        keptPartitions[i] = 0;
        keptResiduals[i] = 1e30;
    }
    for (uint shapeIndex = 0u; shapeIndex < 32u; ++shapeIndex)
    {
        float residual = PartitionResidual(shapeIndex);
        for (int slot = 0; slot < kPartitionsEvaluated; ++slot)
        {
            if (residual >= keptResiduals[slot])
                continue;
            for (int shift = kPartitionsEvaluated - 1; shift > slot; --shift)
            {
                keptResiduals[shift] = keptResiduals[shift - 1];
                keptPartitions[shift] = keptPartitions[shift - 1];
            }
            keptResiduals[slot] = residual;
            keptPartitions[slot] = int(shapeIndex);
            break;
        }
    }

    for (int k = 0; k < kPartitionsEvaluated; ++k)
    {
        uint shapeIndex = uint(keptPartitions[k]);
        uint subsetBits = kPartitionSubsets[shapeIndex];
        int masks[2] = int[2](int((~subsetBits) & 0xFFFFu), int(subsetBits));
        int anchors[2] = int[2](0, int(kPartitionFixup[shapeIndex]));

        ivec3 fit0[2], fit1[2];
        for (int s = 0; s < 2; ++s)
            FitEndpoints(masks[s], anchors[s], fit0[s], fit1[s]);

        for (uint mode = 0u; mode < 14u; ++mode)
        {
            if (ModeSubsets(mode) != 2)
                continue;
            g_Fit0[0] = fit0[0]; g_Fit0[1] = fit0[1];
            g_Fit1[0] = fit1[0]; g_Fit1[1] = fit1[1];
            float error = EvaluateCandidate(mode, masks, anchors);
            if (error < bestError)
            {
                bestError = error;
                bestMode = mode;
                bestPartition = shapeIndex;
                for (int i = 0; i < 4; ++i)
                    bestQ[i] = g_Q[i];
                for (int i = 0; i < 16; ++i)
                    bestIndices[i] = g_Indices[i];
            }
        }
    }

    for (int i = 0; i < 4; ++i)
        g_Q[i] = bestQ[i];
    for (int i = 0; i < 16; ++i)
        g_Indices[i] = bestIndices[i];

    imageStore(u_Blocks, blockCoord, PackBlock(bestMode, bestPartition));
}
