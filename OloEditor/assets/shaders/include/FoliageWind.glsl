#ifndef OLO_FOLIAGE_WIND_GLSL
#define OLO_FOLIAGE_WIND_GLSL

// Local interaction bending (issue #1238) composes INSIDE foliageDeform below,
// so every consumer of this producer — colour, depth, shadow, velocity, and the
// ray-traced vegetation snapshot — gets wind and interaction as one
// displacement rather than two that could be applied in different places.
#include "FoliageInteraction.glsl"

// Every raster consumer evaluates this producer for current and previous
// positions. The field snapshot travels in FoliageParams, including shadows.
// Coordinates of the root are absolute; offsets remain terrain local, matching
// the legacy contract. Bounds use the same length cap and coefficient sum.
vec3 foliageWindOffset(vec3 vertex, vec3 root, vec2 localRootXZ, float phase, float time)
{
    float h = clamp(vertex.y, 0.0, 1.0);
    // The anchored bend profile now lives in FoliageInteraction.glsl, so a foot
    // and a gust lean the same plant along the same curve.
    float bend = foliageBendMask(vertex);
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

// WHICH CLOCK THIS LAYER ANIMATES ON. A legacy layer (no hierarchical weights)
// under an enabled wind field rides u_WindClock.x; everything else rides
// u_Time. Extracted because foliageWindNormal's finite difference has to
// evaluate the wind at the SAME instant the displacement it differences against
// was evaluated at — differencing two clocks and dividing by epsilon = 0.001
// scales the gap by a thousand and the transported normal becomes garbage.
//
// Latent until issue #1238: foliageWindNormal was unreachable for legacy layers,
// so u_Time was always the right answer there. Interaction bending reaches them,
// which is what made the two expressions have to agree.
float foliageAnimationClock()
{
    bool legacyField = dot(u_WindWeights.xyz, vec3(1.0)) <= 0.0 && u_WindFlags.w > 0.5 &&
                       u_ImpostorParams1.x <= 0.5;
    return legacyField ? u_WindClock.x : u_Time;
}

FoliageDeformation foliageDeform(vec3 rest, vec3 vertex, vec3 pivot, float phase)
{
    vec3 root = (u_Model * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    vec3 prevRoot = (u_PrevModel * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    FoliageDeformation result;
    bool legacyField = dot(u_WindWeights.xyz, vec3(1.0)) <= 0.0 && u_WindFlags.w > 0.5 && u_ImpostorParams1.x <= 0.5;
    float currentTime = foliageAnimationClock();
    float previousTime = legacyField ? u_WindClock.w : u_PrevTime;
    if (u_WindHistoryValid <= 0.5 || abs(u_Time - u_PrevTime) < 1e-6) previousTime = currentTime;
    result.Current = rest + foliageWindOffset(vertex, root, pivot.xz, phase, currentTime) +
                     foliageInteractionOffset(vertex, root, 0);
    result.Previous = rest + foliageWindOffset(vertex, prevRoot, pivot.xz, phase, previousTime) +
                      foliageInteractionOffset(vertex, prevRoot, 1);
    return result;
}

// The cofactor Jacobian transports authored mesh normals with the same bend.
// Legacy layers retain their original normal exactly. Shadows need no normals.
vec3 foliageWindNormal(vec3 normal, vec3 vertex, vec3 pivot, float phase,
                       mat3 restJacobian, vec3 offset)
{
    // Legacy layers (no hierarchical weights) still transport their authored
    // normal unchanged, EXCEPT where an interaction is bending them — a plant
    // pushed flat under a foot and shaded as if upright is the same defect the
    // cofactor transport exists to prevent, and issue #1238's layers are not
    // required to opt into hierarchical wind first.
    bool interacting = u_InteractionParams.x >= 0.5 && u_InteractionParams.y > 0.0;
    if (dot(u_WindWeights.xyz, vec3(1.0)) <= 0.0 && !interacting)
        return normalize(restJacobian * normal);
    vec3 root = (u_Model * vec4(pivot, 1.0)).xyz + u_WindFlags.xyz;
    const float epsilon = 0.001;
    mat3 jacobian = restJacobian;
    // Hoisted: the influence loop depends on the ROOT, which is the same for all
    // three perturbations. Only the rooted mask below varies with the vertex.
    vec3 interaction = foliageInteractionPush(root, 0);
    for (int axis = 0; axis < 3; ++axis)
    {
        vec3 step = vec3(0.0);
        step[axis] = epsilon;
        jacobian[axis] += (foliageWindOffset(vertex + step, root, pivot.xz, phase, foliageAnimationClock()) +
                           foliageInteractionOffsetFromPush(interaction, vertex + step) - offset) / epsilon;
    }
    mat3 cofactor = mat3(cross(jacobian[1], jacobian[2]), cross(jacobian[2], jacobian[0]),
                         cross(jacobian[0], jacobian[1]));
    vec3 bent = cofactor * normal;
    return dot(bent, bent) > 1e-12 ? normalize(bent) : normalize(restJacobian * normal);
}
#endif
