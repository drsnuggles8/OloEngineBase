#ifndef OLO_FOLIAGE_WIND_GLSL
#define OLO_FOLIAGE_WIND_GLSL

// Every raster consumer evaluates this producer for current and previous
// positions. The field snapshot travels in FoliageParams, including shadows.
// Coordinates of the root are absolute; offsets remain terrain local, matching
// the legacy contract. Bounds use the same length cap and coefficient sum.
vec3 foliageWindOffset(vec3 vertex, vec3 root, vec2 localRootXZ, float phase, float time)
{
    float h = clamp(vertex.y, 0.0, 1.0);
    float stiffness = u_WindWeights.x;
    float bend = mix(h, h * h / (1.0 + 4.0 * stiffness), stiffness);
    vec3 trunk;
    bool hierarchical = dot(u_WindWeights.xyz, vec3(1.0)) > 0.0;
    if (u_WindFlags.w > 0.5 && (hierarchical || u_ImpostorParams1.x <= 0.5))
    {
        float gust = 1.0 + u_WindGust.x * sin(time * u_WindGust.y * 6.2831853 +
                                            dot(root, u_WindDirection.xyz) * 0.05);
        vec3 velocity = u_WindDirection.xyz * u_WindDirection.w * gust;
        if (hierarchical) velocity *= min(1.0, 20.0 / max(length(velocity), 1e-6));
        trunk = velocity * u_WindStrength * bend * 0.1;
    }
    else
    {
        trunk = foliageLegacyWindOffset(localRootXZ, time, u_WindSpeed, u_WindStrength, bend);
    }
    if (dot(u_WindWeights.xyz, vec3(1.0)) > 0.0)
        trunk *= 0.8 + 0.2 * sin(time * u_WindSpeed * 0.8 + phase);
    // Macro gust phase is spatially coherent. Identity only modulates smaller
    // branch/leaf modes, so adjacent plants share gusts without moving in lockstep.
    float branchMask = h * h;
    float leafMask = branchMask * smoothstep(0.15, 0.7, length(vertex.xz));
    if (u_WindWeights.y > 0.0)
    {
        float branch = sin(time * u_WindSpeed * 1.7 + phase + vertex.y * 2.0);
        trunk += vec3(branch, 0.0, branch * 0.5) * (0.3 * u_WindStrength * u_WindWeights.y * branchMask);
    }
    if (u_WindWeights.z > 0.0)
    {
        float leaf = sin(time * u_WindSpeed * 8.3 + phase * 2.1 + dot(vertex, vec3(17.0, 11.0, 23.0)));
        trunk += vec3(leaf, leaf * 0.5, 0.0) * (0.13 * u_WindStrength * u_WindWeights.z * leafMask);
    }
    return trunk;
}

struct FoliageDeformation
{
    vec3 Current;
    vec3 Previous;
};

FoliageDeformation foliageDeform(vec3 rest, vec3 vertex, vec3 pivot, float phase)
{
    vec3 root = (u_Model * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    vec3 prevRoot = (u_PrevModel * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    FoliageDeformation result;
    bool legacyField = dot(u_WindWeights.xyz, vec3(1.0)) <= 0.0 && u_WindFlags.w > 0.5 && u_ImpostorParams1.x <= 0.5;
    float currentTime = legacyField ? u_WindClock.x : u_Time;
    float previousTime = legacyField ? u_WindClock.w : u_PrevTime;
    if (u_WindHistoryValid <= 0.5 || abs(u_Time - u_PrevTime) < 1e-6) previousTime = currentTime;
    result.Current = rest + foliageWindOffset(vertex, root, pivot.xz, phase, currentTime);
    result.Previous = rest + foliageWindOffset(vertex, prevRoot, pivot.xz, phase, previousTime);
    return result;
}

// The cofactor Jacobian transports authored mesh normals with the same bend.
// Legacy layers retain their original normal exactly. Shadows need no normals.
vec3 foliageWindNormal(vec3 normal, vec3 vertex, vec3 pivot, float phase,
                       mat3 restJacobian, vec3 offset)
{
    if (dot(u_WindWeights.xyz, vec3(1.0)) <= 0.0)
        return normalize(restJacobian * normal);
    vec3 root = (u_Model * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    const float epsilon = 0.001;
    mat3 jacobian = restJacobian;
    for (int axis = 0; axis < 3; ++axis)
    {
        vec3 step = vec3(0.0);
        step[axis] = epsilon;
        jacobian[axis] += (foliageWindOffset(vertex + step, root, pivot.xz, phase, u_Time) - offset) / epsilon;
    }
    mat3 cofactor = mat3(cross(jacobian[1], jacobian[2]), cross(jacobian[2], jacobian[0]),
                         cross(jacobian[0], jacobian[1]));
    vec3 bent = cofactor * normal;
    return dot(bent, bent) > 1e-12 ? normalize(bent) : normalize(restJacobian * normal);
}
#endif
