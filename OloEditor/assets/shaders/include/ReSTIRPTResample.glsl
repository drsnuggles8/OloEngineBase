#ifndef OLO_RESTIR_PT_RESAMPLE
#define OLO_RESTIR_PT_RESAMPLE
// One canonical identity and up to three mapped neighbour strategies. Support
// is evaluated by inverse shifting each candidate, including visibility and
// conditioning, even when that strategy's own random survivor was black.
void PTStream(inout PTPath selected, inout float total, PTPath candidate, float weight, inout OloPathSampler pathSampler)
{
    if (!(weight > 0.0) || !PTFinite(weight) || !PTFinite(total + weight))
        return;
    total += weight;
    if (oloPtGet1D(pathSampler) * total < weight)
        selected = candidate;
}
PTPath PTInitial(PTVertex receiver, uint pixel, inout OloPathSampler pathSampler)
{
    PTPath selected = PTEmpty();
    selected.Vertices[0] = receiver;
    selected.Lineage = uvec4(u_TlasAddressAndFrame.w, pixel, 0, OLO_RESTIR_PT_LAYOUT_BITS | 1u);
    float total = 0.0;
    float mean = 0.0;
    float m2 = 0.0;
    uint count = clamp(u_Counts.x, 1u, 64u);
    for (uint i = 0u; i < 64u; ++i)
    {
        if (i >= count)
            break;
        PTCount(2u);
        PTPath candidate = PTGenerate(receiver, pixel, pathSampler);
        float value = PTSelected(candidate) ? candidate.State.z * candidate.State.x : 0.0;
        if (!PTFinite(value))
        {
            PTCount(13u);
            value = 0.0;
        }
        float delta = value - mean;
        mean += delta / float(i + 1u);
        m2 += delta * (value - mean);
        PTStream(selected, total, candidate, value, pathSampler);
    }
    selected.State.y = float(count);
    selected.State.x = selected.State.z > 0.0 ? total / (float(count) * selected.State.z) : 0.0;
    // Empirical variance of the raw initial candidate luminances; not an
    // estimate of the variance after nonlinear reuse. One draw is insufficient.
    selected.Value.w = count > 1u ? max(m2 / float(count - 1u), 0.0) : 0.0;
    if (!PTFinite(selected.Value.w))
    {
        // A squared HDR sample may overflow even when its radiance is finite.
        // Keep diagnostic arithmetic finite and expose the failure explicitly.
        PTCount(13u);
        selected.Value.w = 0.0;
    }
    return selected;
}
// ONE PTShift CALLSITE, for the same reason PTShift has one PTMapOne callsite.
// Each inlined shift carries a full map's worth of PTPath temporaries in local
// memory. With a forward shift here and the inverse-support shifts in a
// separate MIS helper, the fragment shader needed 21984 B of local memory per
// thread; NVIDIA 621.14 backs at most 2^32 B across every resident thread
// (21845 B on an RTX 4090), creates the pipeline anyway, and loses the device
// on the first spill (#1807). Each strategy is four steps: phase 0 maps the
// neighbour to this receiver (strategy 0 uses the canonical path as is), and
// phases 1..3 inverse-shift that candidate under mappings 1, 2 and 4 for its
// MIS support. Order, arithmetic and sampler consumption match the nested form.
PTPath PTCombine(PTPath canonical, PTPath neighbour, uint mask, inout OloPathSampler pathSampler)
{
    PTPath selected = canonical;
    selected.State.x = 0.0;
    selected.State.z = 0.0;
    selected.Value.rgb = vec3(0);
    selected.Lineage.w = OLO_RESTIR_PT_LAYOUT_BITS | 1u;
    float total = 0.0;
    float confidence = canonical.State.y;
    // Mapping enumeration does not create additional source proposals.
    if ((mask & 7u) != 0u)
        confidence += neighbour.State.y;
    PTPath candidate = canonical;
    float jacobian = 1.0;
    float sourceWeight = 0.0;
    float supportTotal = 0.0;
    float supportOwn = 0.0;
    bool live = false;
    [[dont_unroll]] for (uint step = 0u; step < 16u; ++step)
    {
        uint strategy = step >> 2u;
        uint phase = step & 3u;
        uint bit = strategy == 0u ? 0u : (1u << (strategy - 1u));
        uint supportBit = phase == 0u ? 0u : (1u << (phase - 1u));
        if (phase == 0u)
        {
            candidate = canonical;
            jacobian = 1.0;
            sourceWeight = canonical.State.x;
            live = strategy == 0u ? PTSelected(canonical) : (PTSelected(neighbour) && (mask & bit) != 0u);
        }
        else if (!live || (mask & supportBit) == 0u)
        {
            supportBit = 0u;
        }
        bool needsShift = phase == 0u ? (live && strategy != 0u) : supportBit != 0u;
        if (needsShift)
        {
            // Inputs are selected first so the shift below is the only callsite.
            PTPath source = phase == 0u ? neighbour : candidate;
            PTVertex receiver = phase == 0u ? canonical.Vertices[0] : neighbour.Vertices[0];
            PTPath shifted;
            float shiftJ;
            bool mapped = PTShift(source, receiver, phase == 0u ? bit : supportBit, shifted, shiftJ);
            if (phase == 0u)
            {
                live = mapped;
                if (mapped)
                {
                    candidate = shifted;
                    jacobian = shiftJ;
                    sourceWeight = neighbour.State.x;
                }
            }
            else if (mapped)
            {
                float mass = neighbour.State.y * shifted.State.z * shiftJ;
                if (PTFinite(mass) && mass >= 0.0)
                {
                    supportTotal += mass;
                    if (supportBit == bit)
                        supportOwn = mass;
                }
            }
        }
        if (phase == 0u && live)
        {
            float canonicalMass = canonical.State.y * candidate.State.z;
            supportTotal = canonicalMass;
            supportOwn = bit == 0u ? canonicalMass : 0.0;
        }
        if (phase != 3u || !live)
            continue;
        float m = supportTotal > 0.0 && PTFinite(supportTotal) ? supportOwn / supportTotal : 0.0;
        float weight = m * candidate.State.z * sourceWeight * jacobian;
        if (weight > 0.0)
        {
            if (strategy != 0u)
            {
                PTCount(u_Counts.y == 1u ? 4u : 5u);
                PTCount(bit == 1u ? 6u : (bit == 2u ? 7u : 8u));
                candidate.Lineage.w = OLO_RESTIR_PT_LAYOUT_BITS | 3u | (candidate.Lineage.w & OLO_RESTIR_PT_LINEAGE_HISTORY) |
                                      (u_Counts.y == 1u ? OLO_RESTIR_PT_LINEAGE_HISTORY : 0u);
            }
            PTStream(selected, total, candidate, weight, pathSampler);
        }
    }
    selected.State.x = selected.State.z > 0.0 ? total / selected.State.z : 0.0;
    selected.State.y = min(confidence, max(u_Debug.z, 1.0));
    // The receiver belongs to THIS pixel even when the selected suffix came
    // from elsewhere. Its random coordinates remain those of the selected path.
    return selected;
}
#endif
