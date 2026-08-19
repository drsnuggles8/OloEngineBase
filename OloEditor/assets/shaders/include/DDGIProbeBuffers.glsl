// =============================================================================
// DDGIProbeBuffers.glsl — the DDGI sparsity + scheduling buffers (issue #707)
//
// Include AFTER include/DDGICommon.glsl (this file reads the DDGI volume UBO).
//
// TWO storage bindings serve all of upgrade 2 (sparsity), upgrade 3 (variable
// update rate) and the measurement the issue's acceptance criteria ask for:
//
//   binding 79 — DDGIProbeAux, one record per probe across ALL cascades.
//   binding 80 — DDGIStats, a handful of per-frame counters.
//
// NOTHING here is read back inside DDGIProbeUpdatePass::Execute. The aux buffer
// is written by the GPU and read by the GPU; the CPU touches it only through
// the explicit diagnostics entry point (DDGIProbeUpdatePass::
// ReadbackProbeDiagnostics), which the editor panel, the MCP tools and the
// tests call — never the frame. That is acceptance criterion 3 of #707.
// =============================================================================

#ifndef DDGI_PROBE_BUFFERS_GLSL
#define DDGI_PROBE_BUFFERS_GLSL

// C++ twin: DDGIProbeUpdatePass::ProbeAuxRecord. std430, 32 bytes.
struct DDGIProbeAuxRecord
{
    uint LastRequestFrame;   // frame counter atomicMax'd by ANY requester; 0 = never
    uint ScreenRequestFrame; // frame counter atomicMax'd by SCREEN requesters only
    uint State;              // DDGI::ProbeState as written by DDGI_Relocate.comp
    uint BounceHitCount;     // #751 diagnostic: cached frontface hits considered
    uint Flags;              // bit 0 = has been captured at least once at this lattice point
    float BounceWeightSum;   // #751 diagnostic: sum of ddgiCascadeWeight over those hits
    float _ddgiAuxPad0;
    float _ddgiAuxPad1;
};

// ScreenRequestFrame exists to make the indirection depth EXACTLY ONE, which is
// what issue #707 specifies and what bounds the cost. DDGI_RequestProbe.comp
// walks only probes that a screen pixel requested; a probe that became live via
// that hop is relit, but its own hit points are never walked, so the request set
// cannot grow transitively over frames into the dense grid again.

const uint DDGI_PROBE_FLAG_CAPTURED = 1u;

// Integer twins of the float DDGI_PROBE_* constants in DDGICommon.glsl (the
// probe-data TEXTURE stores the state as a float; the aux buffer as a uint).
const uint DDGI_PROBE_STATE_UNCAPTURED = 0u;
const uint DDGI_PROBE_STATE_ACTIVE = 1u;
const uint DDGI_PROBE_STATE_INACTIVE = 2u;

layout(std430, binding = 79) buffer DDGIProbeAuxBuffer
{
    DDGIProbeAuxRecord b_ProbeAux[];
};

// C++ twin: DDGIProbeUpdatePass::ProbeStats.
layout(std430, binding = 80) buffer DDGIStatsBuffer
{
    uint b_StatLiveProbes;      // probes whose request is still inside the lifetime window
    uint b_StatActiveProbes;    // probes classified Active (captured, not inside geometry)
    uint b_StatRelitProbes;     // probes the relight compute actually shaded this frame
    uint b_StatCapturedProbes;  // probes ever captured at their current lattice point
    uint b_StatBlendedProbes;   // probes the irradiance blend actually wrote this frame
    uint b_StatUncapturedLive;  // live probes still waiting for a first capture
    uint _ddgiStatPad0;
    uint _ddgiStatPad1;
};

// -----------------------------------------------------------------------------
// Requests. A probe is "requested" by a shaded screen pixel, by another live
// probe's cached hit point (the ONE indirection the issue specifies), or by the
// camera-neighbourhood seed. atomicMax is the whole synchronisation story: the
// value is monotone in the frame counter, so racing writers cannot lose a
// newer request, and no clear pass is needed between frames.
// -----------------------------------------------------------------------------

void ddgiRequestProbe(int probeIndex, uint frameIndex, bool fromScreen)
{
    if (probeIndex < 0)
    {
        return;
    }
    atomicMax(b_ProbeAux[probeIndex].LastRequestFrame, frameIndex);
    if (fromScreen)
    {
        atomicMax(b_ProbeAux[probeIndex].ScreenRequestFrame, frameIndex);
    }
}

// True for a probe a shaded screen pixel (or the camera seed, which stands in
// for the pixels a not-yet-rendered frame has not produced) asked for THIS
// frame's lifetime window. The gate on the one-indirection hop.
bool ddgiProbeWasScreenRequested(int probeIndex, uint frameIndex)
{
    return ddgiIsProbeLive(b_ProbeAux[probeIndex].ScreenRequestFrame, frameIndex,
                           uint(max(u_DDGIRequestLifetime, 0)));
}

// Request the 8 probes that the trilinear gather would read for `relPos` in
// `level`. Positions are RENDER-ORIGIN-RELATIVE, matching the UBO.
void ddgiRequestCascadeCorners(int level, vec3 relPos, uint frameIndex, bool fromScreen)
{
    ivec3 dims = max(u_DDGIGridDimensions.xyz, ivec3(1));
    ivec3 latMin = u_DDGICascadeLattice[level].xyz;
    vec3 latF = (relPos - u_DDGICascadeOrigin[level].xyz) / max(u_DDGICascadeSpacing[level].xyz, vec3(1e-6));
    ivec3 base = clamp(ivec3(floor(latF)), latMin, latMin + max(dims - ivec3(2), ivec3(0)));
    for (int i = 0; i < 8; ++i)
    {
        ivec3 corner = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 lattice = min(base + corner, latMin + dims - ivec3(1));
        ddgiRequestProbe(ddgiCascadedProbeIndex(level, ddgiStorageCoordForLattice(lattice)), frameIndex, fromScreen);
    }
}

// Request everything the gather at `relPos` could read: the finest cascade that
// owns the position, plus the next coarser one when the position is inside the
// blend band (where ddgiGatherIrradiance genuinely reads both). Requesting only
// the finest would leave the coarser cascade's band probes stale and turn the
// blend band into a visible seam — the exact failure the handover warns about.
void ddgiRequestProbesAtPosition(vec3 relPos, uint frameIndex, bool fromScreen)
{
    int cascadeCount = clamp(u_DDGICascadeCount, 1, DDGI_MAX_CASCADES);
    for (int level = 0; level < cascadeCount; ++level)
    {
        float w = ddgiCascadeWeight(relPos, level, u_DDGICascadeBlendBand, 0.0);
        if (w <= 0.0)
        {
            continue;
        }
        ddgiRequestCascadeCorners(level, relPos, frameIndex, fromScreen);
        if (w < 1.0 && (level + 1) < cascadeCount)
        {
            ddgiRequestCascadeCorners(level + 1, relPos, frameIndex, fromScreen);
        }
        return;
    }
}

// -----------------------------------------------------------------------------
// Liveness + scheduling. Both are GROUP-UNIFORM in every consumer: a compute
// dispatch maps one work group to one probe, so these decide whether the whole
// group returns. That is deliberate — an early return that is not group-uniform
// in front of a barrier is a hang (docs/agent-rules/gpu-scan-compaction.md).
// -----------------------------------------------------------------------------

bool ddgiProbeIsLive(int probeIndex, uint frameIndex)
{
    if (u_DDGISparsityEnabled == 0)
    {
        return true;
    }
    return ddgiIsProbeLive(b_ProbeAux[probeIndex].LastRequestFrame, frameIndex, uint(max(u_DDGIRequestLifetime, 0)));
}

bool ddgiProbeIsCaptured(int probeIndex)
{
    return (b_ProbeAux[probeIndex].Flags & DDGI_PROBE_FLAG_CAPTURED) != 0u;
}

// The full "does this probe do work this frame" test shared by the relight and
// irradiance-blend computes, so the two can never disagree about which probes
// are current — a probe blended without being relit would EMA toward a stale
// radiance cache, which reads as a probe that slowly drifts dark.
bool ddgiProbeUpdatesNow(int probeIndex, uint frameIndex)
{
    if (!ddgiProbeIsCaptured(probeIndex))
    {
        return false;
    }
    if (b_ProbeAux[probeIndex].State != DDGI_PROBE_STATE_ACTIVE)
    {
        return false;
    }
    if (!ddgiProbeIsLive(probeIndex, frameIndex))
    {
        return false;
    }
    return ddgiProbeUpdatesThisFrame(probeIndex, u_DDGIFrameIndex, u_DDGIUpdateRateDivisor);
}

#endif // DDGI_PROBE_BUFFERS_GLSL
