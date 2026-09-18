#ifndef OLO_FOLIAGE_INTERACTION_GLSL
#define OLO_FOLIAGE_INTERACTION_GLSL

// Local interaction bending (issue #1238). Included by FoliageWind.glsl, which
// is the ONE deformation producer every raster consumer calls — so colour,
// depth, shadow and velocity all get this term, and none of them can get a
// different one. FoliageParams.glsl must already be in scope.
//
// The state is not here. u_Interactions carries the CPU field's per-influence
// springs (FoliageInteraction.h); this file only evaluates the bend a given
// plant takes from them, so the shader is a pure function of the snapshot and
// re-evaluating it for the previous frame is exactly how the velocity lane
// stays honest.

// Mirrors kFoliageInteractionSlots in FoliageInteraction.h. The array in
// FoliageParams is four vec4 per slot, so the two numbers are locked together
// by ShaderUBOSizeConsistencyTest: a change on one side makes the reflected
// block outgrow (or undershoot) its C++ twin.
#define OLO_FOLIAGE_INTERACTION_SLOTS 16

// How far up the plant the bend reaches, and with what profile. Shared with
// foliageWindOffset — a plant that leans differently for wind and for a foot is
// two plants. Anchored at the base for every stiffness, which is the "plants
// remain rooted" half of the second acceptance criterion: h == 0 gives 0 in
// both branches of the mix, exactly, not approximately.
float foliageBendMask(vec3 vertex)
{
    float h = clamp(vertex.y, 0.0, 1.0);
    float stiffness = u_WindWeights.x;
    return mix(h, h * h / (1.0 + 4.0 * stiffness), stiffness);
}

// The horizontal push this plant's ROOT takes from the influence set.
// `snapshot` picks the frame: 0 = current, 1 = previous.
vec3 foliageInteractionPush(vec3 root, int snapshot)
{
    int count = min(int(u_InteractionParams.x + 0.5), OLO_FOLIAGE_INTERACTION_SLOTS);
    float response = u_InteractionParams.y;
    // An empty field contributes EXACTLY zero, not a small number. Every scene
    // that predates this feature has no influence source, so it takes this
    // branch on every vertex and renders bit-identically.
    if (count <= 0 || response <= 0.0)
        return vec3(0.0);

    vec2 push = vec2(0.0);
    for (int i = 0; i < count; ++i)
    {
        vec4 centerRadius = u_Interactions[i * 4 + 0];
        vec4 pushFalloff = u_Interactions[i * 4 + 1];
        vec4 prevCenterHeight = u_Interactions[i * 4 + 2];
        vec4 prevPush = u_Interactions[i * 4 + 3];

        float radius = centerRadius.w;
        if (radius <= 0.0)
            continue;

        vec3 center = snapshot == 0 ? centerRadius.xyz : prevCenterHeight.xyz;
        vec3 state = snapshot == 0 ? pushFalloff.xyz : prevPush.xyz;

        vec2 delta = root.xz - center.xz;
        float distance = length(delta);
        float normalized = distance / radius;
        if (normalized >= 1.0)
            continue;
        float radial = pow(1.0 - normalized, pushFalloff.w);

        // The influence is a vertical CYLINDER, not a sphere: an actor's origin
        // is at its feet and a blade's root is on the ground, so a sphere
        // centred on the actor either misses the grass it is standing in or
        // reaches a terrace above it. `prevCenterHeight.w` is how far above the
        // centre a root may still sit; one radius below covers a foot sunk into
        // uneven ground.
        float height = prevCenterHeight.w;
        float outside = max((center.y - radius) - root.y, root.y - (center.y + height));
        float vertical = 1.0 - smoothstep(0.0, max(0.25 * radius, 1e-4), outside);
        float weight = radial * vertical;
        if (weight <= 0.0)
            continue;

        // Two terms, both already spring-smoothed on the CPU: `state.xy` leans
        // the plant the way the actor is travelling, `state.z` presses it away
        // from the actor. A plant directly under the centre gets no radial
        // direction at all rather than a normalize() of zero.
        vec2 outward = distance > 1e-5 ? delta / distance : vec2(0.0);
        push += weight * (state.xy + state.z * outward);
    }

    push *= response;

    // THE BOUND, enforced where the sum happens. Any number of overlapping
    // influences still moves a plant at most u_InteractionParams.z, which is
    // what FoliageInteractionMaximumDisplacement pads every instance AABB by —
    // so a bent plant can never leave the box that decides whether it is drawn.
    float magnitude = length(push);
    if (magnitude > u_InteractionParams.z)
        push *= u_InteractionParams.z / magnitude;

    // Horizontal only, exactly like foliageWindOffset. The rooted mask is what
    // reads as a bend; adding an arc-shortening drop here and not in the wind
    // producer would make the two disagree about where the same plant's tip is.
    return vec3(push.x, 0.0, push.y);
}

// The push is a function of the plant's ROOT alone; only the rooted mask varies
// along the plant. Split so a caller that evaluates several vertices of the
// SAME plant — the normal Jacobian evaluates three — runs the influence loop
// once instead of once per vertex. At the 16-slot cap that is the difference
// between 16 and 48 iterations per vertex in the normal path.
vec3 foliageInteractionOffsetFromPush(vec3 push, vec3 vertex)
{
    return push * foliageBendMask(vertex);
}

vec3 foliageInteractionOffset(vec3 vertex, vec3 root, int snapshot)
{
    return foliageInteractionOffsetFromPush(foliageInteractionPush(root, snapshot), vertex);
}
#endif
