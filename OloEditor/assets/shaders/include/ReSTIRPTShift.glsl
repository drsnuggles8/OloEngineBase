#ifndef OLO_RESTIR_PT_SHIFT
#define OLO_RESTIR_PT_SHIFT
// Mirrors PathShift.h and ReservoirCore.h; shader parity tests pin these.
const float PTMinimumConnectionDistance = 0.05;
const float PTMinimumShiftCosine = 1.0e-4;
bool PTFinitePath(PTPath p)
{
    if (p.Metadata.x > 3u || p.Metadata.y > 1u || any(isnan(p.State)) || any(isinf(p.State)) ||
        any(lessThan(p.State.xyz, vec3(0))) || any(isnan(p.Endpoint.PositionKind)) ||
        any(isinf(p.Endpoint.PositionKind)) || any(isnan(p.Endpoint.RadianceDensity)) ||
        any(isinf(p.Endpoint.RadianceDensity)))
        return false;
    for (uint k = 0u; k < 4u; ++k)
    {
        if (k > p.Metadata.x)
            break;
        PTVertex v = p.Vertices[k];
        if (any(isnan(v.PositionRoughness)) || any(isinf(v.PositionRoughness)) ||
            any(isnan(v.GeometricNormalMetallic)) || any(isinf(v.GeometricNormalMetallic)) ||
            any(isnan(v.ShadingNormalClosure)) || any(isinf(v.ShadingNormalClosure)) ||
            any(isnan(v.Randoms)) || any(isinf(v.Randoms)) || !PTFinite3(v.Albedo.rgb) || !PTFinite3(v.Incoming.xyz))
            return false;
        if (abs(dot(v.Incoming.xyz, v.Incoming.xyz) - 1.0) > 0.0002 ||
            abs(dot(v.GeometricNormalMetallic.xyz, v.GeometricNormalMetallic.xyz) - 1.0) > 0.0002)
            return false;
    }
    return true;
}
bool PTAccumulate(float factor, inout float logJ, inout float logAbs)
{
    if (!(factor > 0.0) || !PTFinite(factor))
        return false;
    float term = log(factor);
    logJ += term;
    logAbs += abs(term);
    if (!PTFinite(logJ) || logAbs > log(8.0))
    {
        PTCount(11u);
        return false;
    }
    return true;
}
bool PTRoughChart(PTVertex v)
{
    return v.Randoms.w < 0.5 || v.PositionRoughness.w >= u_Params.w;
}
bool PTCompatibleReceiver(PTVertex a, PTVertex b)
{
    return dot(a.GeometricNormalMetallic.xyz, b.GeometricNormalMetallic.xyz) > 0.5 &&
           abs(a.PositionRoughness.w - b.PositionRoughness.w) < 0.35;
}
bool PTConnection(PTPath source, PTPath destination, uint k, out float jacobian, out PTVertex changed, out PTVertex held)
{
    jacobian = 0.0;
    changed = destination.Vertices[k];
    held = PTEmptyVertex();
    PTVertex original = source.Vertices[k];
    if (!PTRoughChart(original) || !PTRoughChart(changed) || !PTCompatibleReceiver(original, changed))
        return false;
    // Invert the outgoing direction after moving this receiver. The residual
    // stays fixed only for reconnection, never for random replay.
    if (k < source.Metadata.x)
    {
        held = source.Vertices[k + 1u];
        if ((k + 1u < source.Metadata.x || source.Metadata.y == 1u) && !PTRoughChart(held))
            return false;
        // Solid angles belong to the actual sampling origins. On this signed
        // front-sided chart the normal offset is constant, independent of l.
        vec3 srcDelta = held.PositionRoughness.xyz - PTOrigin(original, original.GeometricNormalMetallic.xyz);
        vec3 dstDelta = held.PositionRoughness.xyz - PTOrigin(changed, changed.GeometricNormalMetallic.xyz);
        float srcDistance = length(srcDelta);
        float dstDistance = length(dstDelta);
        float minimum = PTMinimumConnectionDistance;
        if (srcDistance <= minimum || dstDistance <= minimum)
            return false;
        vec3 ls = srcDelta / srcDistance;
        vec3 ld = dstDelta / dstDistance;
        float cs = dot(held.GeometricNormalMetallic.xyz, -ls);
        float cd = dot(held.GeometricNormalMetallic.xyz, -ld);
        if (cs <= PTMinimumShiftCosine || cd <= PTMinimumShiftCosine || dot(original.GeometricNormalMetallic.xyz, ls) <= PTMinimumShiftCosine ||
            dot(changed.GeometricNormalMetallic.xyz, ld) <= PTMinimumShiftCosine)
            return false;
        if (!PTVisible(original, held.PositionRoughness.xyz) || !PTVisible(changed, held.PositionRoughness.xyz))
            return false;
        if (!PTInverseDirection(changed, ld, PTResidual(original)))
            return false;
        held.Incoming.xyz = -ld;
        if (k + 1u < source.Metadata.x || source.Metadata.y == 1u)
        {
            if (!PTInverseDirection(held, PTDirection(source, k + 1u), PTResidual(source.Vertices[k + 1u])))
                return false;
        }
        jacobian = (cd / (dstDistance * dstDistance)) / (cs / (srcDistance * srcDistance));
        return true;
    }
    // A terminal environment direction is held in solid angle. A terminal
    // triangle drawn by BSDF instead has the same projected-area connection.
    if (source.Metadata.y == 0u)
        return false;
    vec3 ls = PTDirection(source, k);
    vec3 ld = ls;
    if (source.Endpoint.PositionKind.w > 1.5)
    {
        vec3 srcDelta = source.Endpoint.PositionKind.xyz - PTOrigin(original, original.GeometricNormalMetallic.xyz);
        vec3 dstDelta = source.Endpoint.PositionKind.xyz - PTOrigin(changed, changed.GeometricNormalMetallic.xyz);
        float ds = length(srcDelta);
        float dd = length(dstDelta);
        if (min(ds, dd) <= PTMinimumConnectionDistance)
            return false;
        ld = dstDelta / dd;
        float cs = dot(source.Endpoint.NormalTwoSided.xyz, -ls);
        float cd = dot(source.Endpoint.NormalTwoSided.xyz, -ld);
        // Reconnection is deliberately front-sided even for two-sided emission.
        if (min(cs, cd) <= PTMinimumShiftCosine)
            return false;
        jacobian = cd * ds * ds / (cs * dd * dd);
        if (!PTVisible(original, source.Endpoint.PositionKind.xyz) || !PTVisible(changed, source.Endpoint.PositionKind.xyz))
            return false;
    }
    else
    {
        jacobian = 1.0;
        if (!PTVisible(original, PTOrigin(original, ls) + ls * u_Params.z) ||
            !PTVisible(changed, PTOrigin(changed, ld) + ld * u_Params.z))
            return false;
    }
    if (dot(original.GeometricNormalMetallic.xyz, ls) <= PTMinimumShiftCosine || dot(changed.GeometricNormalMetallic.xyz, ld) <= PTMinimumShiftCosine)
        return false;
    return PTInverseDirection(changed, ld, PTResidual(original));
}
// Mapping identifiers are mask values 1 reconnect, 2 replay, 4 hybrid.
// The finite map routine is used in both directions and selects its own first
// connection. The wrapper checks that the reverse selected the same index.
bool PTMapOne(PTPath source, PTVertex receiver, uint mapping, out PTPath destination, out float logJ, out uint connection)
{
    destination = source;
    destination.Vertices[0] = receiver;
    destination.Vertices[0].Randoms = source.Vertices[0].Randoms;
    logJ = 0.0;
    float logAbs = 0.0;
    connection = 4u;
    bool connected = false;
    if (source.Metadata.x > 3u || source.Metadata.y > 1u)
        return false;
    for (uint k = 0u; k < 4u; ++k)
    {
        if (k > source.Metadata.x)
            break;
        if (mapping != 2u)
        {
            PTVertex changed, held;
            float factor;
            if (PTConnection(source, destination, k, factor, changed, held))
            {
                if (!PTAccumulate(factor, logJ, logAbs))
                    return false;
                destination.Vertices[k] = changed;
                if (k < source.Metadata.x)
                    destination.Vertices[k + 1u] = held;
                connected = true;
                connection = k;
                break;
            }
            if (mapping == 1u)
                return false;
        }
        if (k == source.Metadata.x && source.Metadata.y == 0u)
            break;
        PTVertex v = destination.Vertices[k];
        vec3 l;
        if (!PTForward(v, l))
            return false;
        float cs = PTChartDensity(source.Vertices[k], PTDirection(source, k));
        float cd = PTChartDensity(v, l);
        if (!PTAccumulate(cs / cd, logJ, logAbs))
            return false;
        if (k < source.Metadata.x)
        {
            OloRtHit hit;
            if (!PTTrace(v, l, hit) || hit.Unshadeable || hit.SphereLight >= 0)
                return false;
            destination.Vertices[k + 1u] = PTFromHit(hit, -l);
            destination.Vertices[k + 1u].Randoms = source.Vertices[k + 1u].Randoms;
        }
    }
    if (!connected)
    {
        if (source.Metadata.y == 0u)
        {
            if (!PTNEE(destination, false))
                return false;
        }
        else
        {
            if (!PTBSDFEndpoint(destination))
                return false;
            if (abs(destination.Endpoint.PositionKind.w - source.Endpoint.PositionKind.w) > 0.1)
                return false;
        }
    }
    else if (source.Metadata.y == 0u)
    {
        // Fixed NEE area coordinates, but punctual attenuation and endpoint
        // visibility are always reevaluated at the shifted last receiver.
        if (!PTNEE(destination, false))
            return false;
    }
    float proposal;
    if (!PTEvaluatePath(destination, proposal))
        return false;
    destination.State.w = logJ;
    destination.Lineage.z = mapping;
    return true;
}
bool PTEquivalent(PTPath a, PTPath b)
{
    if (any(notEqual(a.Metadata, b.Metadata)))
        return false;
    float positionTolerance = max(0.0001, 0.1 * u_Params.y);
    for (uint k = 0u; k < 4u; ++k)
    {
        if (k > a.Metadata.x)
            break;
        if (any(notEqual(a.Vertices[k].Identity, b.Vertices[k].Identity)))
            return false;
        if (length(a.Vertices[k].PositionRoughness.xyz - b.Vertices[k].PositionRoughness.xyz) > positionTolerance)
            return false;
        if (length(a.Vertices[k].Incoming.xyz - b.Vertices[k].Incoming.xyz) > 0.0002 ||
            length(a.Vertices[k].GeometricNormalMetallic.xyz - b.Vertices[k].GeometricNormalMetallic.xyz) > 0.0002)
            return false;
        if (abs(a.Vertices[k].Randoms.w - b.Vertices[k].Randoms.w) > 0.1)
            return false;
        if (k < a.Metadata.x || a.Metadata.y == 1u)
        {
            if (length(a.Vertices[k].Randoms.xyz - b.Vertices[k].Randoms.xyz) > 0.002)
                return false;
            if (length(PTDirection(a, k) - PTDirection(b, k)) > 0.0002)
                return false;
        }
    }
    return abs(a.Endpoint.PositionKind.w - b.Endpoint.PositionKind.w) < 0.1 &&
           all(equal(a.Endpoint.Identity, b.Endpoint.Identity)) &&
           length(a.Endpoint.PositionKind.xyz - b.Endpoint.PositionKind.xyz) < positionTolerance;
}
bool PTShift(PTPath source, PTVertex receiver, uint mapping, out PTPath destination, out float jacobian)
{
    jacobian = 0.0;
    destination = source;
    if (!PTLayoutCompatible(source))
    {
        PTCount(9u);
        return false;
    }
    if (!PTFinitePath(source) || !PTFinite3(receiver.PositionRoughness.xyz) || !PTFinite3(receiver.Incoming.xyz))
    {
        PTCount(13u);
        return false;
    }
    if (source.Metadata.z != u_Counts.w || source.Metadata.w != u_Reuse.w)
    {
        PTCount(9u);
        return false;
    }
    float logJ = 0.0;
    uint connection = 4u;
    PTPath current = source;
    PTVertex targetReceiver = receiver;
    // One static mapping callsite avoids duplicating the full ray-query and
    // closure graph. Execution remains forward first, then independent inverse.
    [[dont_unroll]] for (uint direction = 0u; direction < 2u; ++direction)
    {
        PTPath mapped;
        float mappedLog;
        uint mappedConnection;
        if (!PTMapOne(current, targetReceiver, mapping, mapped, mappedLog, mappedConnection))
        {
            PTCount(direction == 0u ? 9u : 10u);
            return false;
        }
        if (direction == 0u)
        {
            destination = mapped;
            logJ = mappedLog;
            connection = mappedConnection;
            current = mapped;
            targetReceiver = source.Vertices[0];
        }
        else if (connection != mappedConnection || !PTEquivalent(source, mapped) || abs(logJ + mappedLog) > 0.002)
        {
            PTCount(10u);
            return false;
        }
    }
    jacobian = exp(logJ);
    if (!PTFinite(jacobian) || !(jacobian > 0.0))
        return false;
    // Test perturbations only. The ordinary shader never defines either
    // symbol. Deliberately corrupt the estimator AFTER validating the real
    // map, inverse, visibility and conditioning, so a negative control does
    // not accidentally compare two different admission policies.
#ifdef OLO_RESTIR_PT_TEST_OMIT_JACOBIAN
    jacobian = 1.0;
#endif
#ifdef OLO_RESTIR_PT_TEST_OMIT_MAPPING
    // Deliberately omit the validated mapping while retaining its actual J.
    // For reconnection, keeping the suffix at a NEW receiver would still
    // perform the essential mapping. Instead retain the original receiver,
    // angular coordinates and source target as well: none is transformed.
    // Replay/hybrid retain the old suffix but reevaluate at the new receiver,
    // exposing the missing prefix replay rather than merely copying colour.
    destination = source;
    if (mapping != 1u)
    {
        destination.Vertices[0] = receiver;
        destination.Vertices[0].Randoms = source.Vertices[0].Randoms;
        float ignoredProposal;
        if (!PTEvaluatePath(destination, ignoredProposal))
            return false;
    }
    destination.Lineage.z = mapping;
    destination.State.w = logJ;
#endif
    return true;
}
#endif
