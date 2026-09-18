#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
void main() { gl_Position = vec4(a_Position, 1.0); }

#type fragment
#version 460 core
// The GPU half of issue #1238, run through the same harness as
// ShaderUnit_FoliageWind.glsl: the PRODUCTION include, with the UBO block
// replaced by mutable globals so one column can set up one case. Testing the
// real include rather than a copy is the point — a copy would drift.
layout(location = 0) out vec4 o_Result;
vec4 u_WindWeights = vec4(0.0);
vec4 u_InteractionParams = vec4(0.0);
vec4 u_Interactions[64];
#include "include/FoliageInteraction.glsl"

// One influence, written into slot `i`.
void put(int i, vec3 center, float radius, vec3 push, float falloff,
         vec3 prevCenter, float height, vec3 prevPush)
{
    u_Interactions[i * 4 + 0] = vec4(center, radius);
    u_Interactions[i * 4 + 1] = vec4(push, falloff);
    u_Interactions[i * 4 + 2] = vec4(prevCenter, height);
    u_Interactions[i * 4 + 3] = vec4(prevPush, 0.0);
}

void main()
{
    int column = int(gl_FragCoord.x);
    vec3 root = vec3(10.0, 0.0, 20.0);
    vec3 vertex = vec3(0.3, 1.0, 0.2);

    u_WindWeights = vec4(0.4, 0.0, 0.0, 0.0);
    // count, layer response, summed-push ceiling. The ceiling is what
    // FoliageInteractionMaximumDisplacement(response) produces on the CPU for
    // this response, and the test asserts against that same helper — so the two
    // sides of the clamp cannot drift apart.
    u_InteractionParams = vec4(1.0, 1.0, 1.0, 0.0);
    put(0, vec3(10.0, 0.0, 20.0), 4.0, vec3(0.5, 0.0, 1.0), 2.0,
        vec3(9.0, 0.0, 20.0), 2.0, vec3(0.2, 0.0, 0.5));

    // An EMPTY field contributes exactly zero. This is the regression guard for
    // every scene that has no influence source at all.
    if (column == 0)
        u_InteractionParams.x = 0.0;
    // A layer that does not respond contributes exactly zero too.
    if (column == 7)
        u_InteractionParams.y = 0.0;
    // Rooted: the base of the plant never moves, for any influence.
    if (column == 1)
        vertex.y = 0.0;
    // Outside the horizontal radius.
    if (column == 3)
        root.x += 100.0;
    // Below the cylinder — an actor on a terrace does not flatten the meadow
    // underneath it.
    if (column == 5)
        root.y -= 50.0;
    // Saturation: sixteen overlapping influences, all at full push, with a
    // response that would multiply far past the cap.
    if (column == 4)
    {
        u_InteractionParams = vec4(16.0, 1.0, 1.0, 0.0);
        for (int i = 0; i < 16; ++i)
            put(i, vec3(10.0, 0.0, 20.0), 4.0, vec3(2.0, 2.0, 2.0), 1.0,
                vec3(10.0, 0.0, 20.0), 2.0, vec3(2.0, 2.0, 2.0));
    }
    // The radial term pushes AWAY from the centre. Offset the plant so there is
    // a direction to check, and drop the directional term so only the radial
    // one is under test.
    if (column == 8)
    {
        root = vec3(12.0, 0.0, 20.0);
        put(0, vec3(10.0, 0.0, 20.0), 4.0, vec3(0.0, 0.0, 1.0), 1.0,
            vec3(10.0, 0.0, 20.0), 2.0, vec3(0.0, 0.0, 1.0));
    }

    vec3 current = foliageInteractionOffset(vertex, root, 0);
    // Column 6 asks whether the PREVIOUS snapshot is genuinely a different
    // evaluation — a velocity lane built from one snapshot reprojects a bent
    // plant from its rest pose and smears.
    if (column == 6)
    {
        o_Result = vec4(current - foliageInteractionOffset(vertex, root, 1), 1.0);
        return;
    }
    if (column == 8)
    {
        vec2 outward = normalize(root.xz - vec2(10.0, 20.0));
        o_Result = vec4(dot(current.xz, outward), current.y, 0.0, 1.0);
        return;
    }
    o_Result = vec4(current, 1.0);
}
