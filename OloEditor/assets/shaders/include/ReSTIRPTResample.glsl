#ifndef OLO_RESTIR_PT_RESAMPLE
#define OLO_RESTIR_PT_RESAMPLE
// One canonical identity and up to three mapped neighbour strategies. Support
// is evaluated by inverse shifting each candidate, including visibility and
// conditioning, even when that strategy's own random survivor was black.
float PTMIS(PTPath candidate, PTPath canonical, PTPath neighbour, uint ownMapping, uint mask)
{
    float canonicalMass = canonical.State.y * candidate.State.z;
    float total = canonicalMass;
    float own = ownMapping == 0u ? canonicalMass : 0.0;
    for (uint bit = 1u; bit <= 4u; bit *= 2u)
    {
        if ((mask & bit) == 0u)
            continue;
        PTPath inverse;
        float inverseJ;
        if (!PTShift(candidate, neighbour.Vertices[0], bit, inverse, inverseJ))
            continue;
        float mass = neighbour.State.y * inverse.State.z * inverseJ;
        if (!PTFinite(mass) || mass < 0.0)
            continue;
        total += mass;
        if (bit == ownMapping)
            own = mass;
    }
    return total > 0.0 && PTFinite(total) ? own / total : 0.0;
}
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
    // Preserve canonical/reconnect/replay/hybrid order with one static MIS
    // callsite. Each support query contains a full forward-and-inverse trace.
    [[dont_unroll]] for (uint strategy = 0u; strategy < 4u; ++strategy)
    {
        uint bit = strategy == 0u ? 0u : (1u << (strategy - 1u));
        PTPath candidate = canonical;
        float jacobian = 1.0;
        float sourceWeight = canonical.State.x;
        if (strategy == 0u)
        {
            if (!PTSelected(canonical))
                continue;
        }
        else
        {
            if (!PTSelected(neighbour) || (mask & bit) == 0u)
                continue;
            if (!PTShift(neighbour, canonical.Vertices[0], bit, candidate, jacobian))
                continue;
            sourceWeight = neighbour.State.x;
        }
        float m = PTMIS(candidate, canonical, neighbour, bit, mask);
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
