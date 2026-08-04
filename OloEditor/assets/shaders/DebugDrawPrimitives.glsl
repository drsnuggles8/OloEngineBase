// =============================================================================
// DebugDrawPrimitives.glsl — GPU-pushable shader debug draws, DRAW side (#725)
// =============================================================================
//
// One program, seven indirect draws. `ShaderDebugDrawPass` binds this once,
// then for each primitive type uploads `u_DebugPrimitiveType` and issues
// `glDrawArraysIndirect` against that type's channel — whose first 16 bytes ARE
// the DrawArraysIndirectCommand. The instance count was written by the same
// atomics that appended the entries, so the number of primitives never travels
// through the CPU.
//
// Expansion is two stages:
//
//   entry  ->  N line segments   (pure math, mirrored on the CPU in
//                                 Renderer/Debug/ShaderDebugDrawExpansion.cpp
//                                 and pinned by ShaderDebugDrawExpansionTest)
//   segment -> one screen-space quad (6 verts)
//
// Segments are expanded to SCREEN-SPACE QUADS rather than drawn with GL_LINES
// for two reasons: line width becomes a real, uniform-controlled quantity
// instead of a driver-dependent one (GL core profile only guarantees width 1),
// and the draw stays on GL_TRIANGLES — which is what
// `RendererAPI::DrawArraysIndirect` issues, so this feature needs no new RHI
// entry point and does not collide with the in-flight RHI work.
//
// The vertex-ID decode is:  segment = gl_VertexIndex / 6,  corner = % 6
//                           entry   = gl_InstanceIndex
//
// =============================================================================

#type vertex
#version 460 core

// Pulls in the entry structs and all seven channel declarations — the same
// declarations the push side sees, so the two cannot disagree about the layout.
// This is the one shader that legitimately wants every channel; a pushing shader
// names only the primitives it emits (see the header's opt-in note).
#define OLO_DEBUG_DRAW_ALL
#include "include/DebugDrawCommon.glsl"

// Segment counts. These four literals are read out of THIS FILE by
// ShaderDebugDrawContractTest and compared against
// ShaderDebugDrawContract::k* — keep the spelling stable.
#define OLO_DBG_CIRCLE_SEGMENTS 32u
#define OLO_DBG_SPHERE_RING_SEGMENTS 32u
#define OLO_DBG_CONE_RING_SEGMENTS 24u
#define OLO_DBG_CONE_SIDE_LINES 4u
#define OLO_DBG_VERTS_PER_SEGMENT 6u

layout(std140, binding = 57) uniform DebugDrawParams
{
    mat4 u_DebugViewProjection;            // world -> main clip
    mat4 u_DebugObserverInvViewProjection; // observer NDC -> world
    vec2 u_DebugViewportSize;              // pixels
    float u_DebugLineWidth;                // pixels
    uint u_DebugPrimitiveType;             // ShaderDebugDrawPrimitive
};

layout(location = 0) out vec3 v_Color;

// Orthonormal basis for a plane with the given normal. MUST match
// ShaderDebugDrawExpansion::OrthonormalBasis — a different-but-valid basis
// rotates every ring by an arbitrary angle, invisible on a circle and very
// visible on a cone's spokes.
void OloDebugBasis(vec3 n, out vec3 t, out vec3 b)
{
    vec3 nn = normalize(n);
    vec3 reference = (abs(nn.x) < 0.9) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    t = normalize(cross(reference, nn));
    b = cross(nn, t);
}

vec3 OloDebugRingPoint(vec3 center, vec3 t, vec3 b, float radius, uint i, uint count)
{
    float angle = 6.28318530717958647692 * (float(i) / float(count));
    return center + (t * (radius * cos(angle))) + (b * (radius * sin(angle)));
}

vec3 OloDebugAABBCorner(vec3 mn, vec3 mx, uint i)
{
    return vec3(((i & 1u) != 0u) ? mx.x : mn.x,
                ((i & 2u) != 0u) ? mx.y : mn.y,
                ((i & 4u) != 0u) ? mx.z : mn.z);
}

// The 12 edges of a corner-indexed box: every pair of indices differing in
// exactly one bit. Same order as ShaderDebugDrawExpansion::BoxEdges().
uvec2 OloDebugBoxEdge(uint i)
{
    uvec2 edges[12] = uvec2[12](uvec2(0u, 1u), uvec2(2u, 3u), uvec2(4u, 5u), uvec2(6u, 7u),
                                uvec2(0u, 2u), uvec2(1u, 3u), uvec2(4u, 6u), uvec2(5u, 7u),
                                uvec2(0u, 4u), uvec2(1u, 5u), uvec2(2u, 6u), uvec2(3u, 7u));
    return edges[i];
}

// Resolve one segment of one entry. Writes the two endpoints in the entry's own
// coordinate space, plus the entry's colour and space tag.
void OloDebugFetchSegment(uint primitive, uint entry, uint segment,
                          out vec3 a, out vec3 b, out vec3 color, out uint space)
{
    a = vec3(0.0);
    b = vec3(0.0);
    color = vec3(1.0);
    space = OLO_DEBUG_SPACE_WORLD;

    vec3 tangent;
    vec3 bitangent;

    if (primitive == 0u) // Line
    {
        OloDebugLineEntry e = g_OloDebugLines.Entries[entry];
        a = e.Start;
        b = e.End;
        color = e.Color;
        space = e.Space;
    }
    else if (primitive == 1u) // Circle
    {
        OloDebugCircleEntry e = g_OloDebugCircles.Entries[entry];
        OloDebugBasis(e.Normal, tangent, bitangent);
        a = OloDebugRingPoint(e.Center, tangent, bitangent, e.Radius, segment, OLO_DBG_CIRCLE_SEGMENTS);
        b = OloDebugRingPoint(e.Center, tangent, bitangent, e.Radius, segment + 1u, OLO_DBG_CIRCLE_SEGMENTS);
        color = e.Color;
        space = e.Space;
    }
    else if (primitive == 2u) // Rectangle
    {
        OloDebugRectangleEntry e = g_OloDebugRectangles.Entries[entry];
        vec3 c0 = e.Center - e.AxisU - e.AxisV;
        vec3 c1 = e.Center + e.AxisU - e.AxisV;
        vec3 c2 = e.Center + e.AxisU + e.AxisV;
        vec3 c3 = e.Center - e.AxisU + e.AxisV;
        vec3 corners[4] = vec3[4](c0, c1, c2, c3);
        a = corners[segment];
        b = corners[(segment + 1u) & 3u];
        color = e.Color;
        space = e.Space;
    }
    else if (primitive == 3u) // AABB
    {
        OloDebugAABBEntry e = g_OloDebugAABBs.Entries[entry];
        uvec2 edge = OloDebugBoxEdge(segment);
        a = OloDebugAABBCorner(e.Min, e.Max, edge.x);
        b = OloDebugAABBCorner(e.Min, e.Max, edge.y);
        color = e.Color;
        space = e.Space;
    }
    else if (primitive == 4u) // Box (8 explicit corners)
    {
        uvec2 edge = OloDebugBoxEdge(segment);
        a = g_OloDebugBoxes.Entries[entry].Corners[edge.x].xyz;
        b = g_OloDebugBoxes.Entries[entry].Corners[edge.y].xyz;
        color = g_OloDebugBoxes.Entries[entry].Color;
        space = g_OloDebugBoxes.Entries[entry].Space;
    }
    else if (primitive == 5u) // Cone
    {
        OloDebugConeEntry e = g_OloDebugCones.Entries[entry];
        vec3 baseCenter = e.Apex + e.Axis;
        OloDebugBasis(e.Axis, tangent, bitangent);
        if (segment < OLO_DBG_CONE_RING_SEGMENTS)
        {
            a = OloDebugRingPoint(baseCenter, tangent, bitangent, e.Radius, segment, OLO_DBG_CONE_RING_SEGMENTS);
            b = OloDebugRingPoint(baseCenter, tangent, bitangent, e.Radius, segment + 1u, OLO_DBG_CONE_RING_SEGMENTS);
        }
        else
        {
            uint spoke = segment - OLO_DBG_CONE_RING_SEGMENTS;
            uint ringIndex = (spoke * OLO_DBG_CONE_RING_SEGMENTS) / OLO_DBG_CONE_SIDE_LINES;
            a = e.Apex;
            b = OloDebugRingPoint(baseCenter, tangent, bitangent, e.Radius, ringIndex, OLO_DBG_CONE_RING_SEGMENTS);
        }
        color = e.Color;
        space = e.Space;
    }
    else // Sphere — three axis-aligned great circles, X then Y then Z
    {
        OloDebugSphereEntry e = g_OloDebugSpheres.Entries[entry];
        uint ring = segment / OLO_DBG_SPHERE_RING_SEGMENTS;
        // NOT named `step` — that is a GLSL built-in function, and shadowing it
        // is legal but a trap for anyone who later adds a step() call here.
        uint ringStep = segment % OLO_DBG_SPHERE_RING_SEGMENTS;
        vec3 normal = (ring == 0u) ? vec3(1.0, 0.0, 0.0) : ((ring == 1u) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        OloDebugBasis(normal, tangent, bitangent);
        a = OloDebugRingPoint(e.Center, tangent, bitangent, e.Radius, ringStep, OLO_DBG_SPHERE_RING_SEGMENTS);
        b = OloDebugRingPoint(e.Center, tangent, bitangent, e.Radius, ringStep + 1u, OLO_DBG_SPHERE_RING_SEGMENTS);
        color = e.Color;
        space = e.Space;
    }
}

// Position (in the entry's space) -> main-camera clip space.
vec4 OloDebugToClip(vec3 p, uint space)
{
    if (space == OLO_DEBUG_SPACE_MAIN_NDC)
    {
        // Already main-camera NDC: pass through. w = 1 makes the perspective
        // divide a no-op and keeps the screen-space expansion below exact.
        return vec4(p, 1.0);
    }
    if (space == OLO_DEBUG_SPACE_OBSERVER_NDC)
    {
        vec4 world = u_DebugObserverInvViewProjection * vec4(p, 1.0);
        world /= world.w;
        return u_DebugViewProjection * vec4(world.xyz, 1.0);
    }
    return u_DebugViewProjection * vec4(p, 1.0);
}

void main()
{
    uint segment = uint(gl_VertexIndex) / OLO_DBG_VERTS_PER_SEGMENT;
    uint corner = uint(gl_VertexIndex) % OLO_DBG_VERTS_PER_SEGMENT;
    uint entry = uint(gl_InstanceIndex);

    vec3 a;
    vec3 b;
    vec3 color;
    uint space;
    OloDebugFetchSegment(u_DebugPrimitiveType, entry, segment, a, b, color, space);
    v_Color = color;

    vec4 clipA = OloDebugToClip(a, space);
    vec4 clipB = OloDebugToClip(b, space);

    // Clip against the near plane (z >= -w) BEFORE the perspective divide.
    // Without this a segment with an endpoint behind the eye divides by a
    // negative w and the quad flips across the screen — the classic "debug line
    // shoots off to infinity when you walk past it" artefact.
    float da = clipA.z + clipA.w;
    float db = clipB.z + clipB.w;
    if (da < 0.0 && db < 0.0)
    {
        // Entirely behind the near plane. Emit a vertex the near plane clips
        // away rather than an arbitrary on-screen position.
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        return;
    }
    if (da < 0.0)
        clipA = mix(clipA, clipB, da / (da - db));
    else if (db < 0.0)
        clipB = mix(clipB, clipA, db / (db - da));

    vec2 halfViewport = 0.5 * u_DebugViewportSize;
    vec2 pixelA = (clipA.xy / clipA.w) * halfViewport;
    vec2 pixelB = (clipB.xy / clipB.w) * halfViewport;

    vec2 along = pixelB - pixelA;
    float alongLength = length(along);
    vec2 direction = (alongLength > 1e-6) ? (along / alongLength) : vec2(1.0, 0.0);
    vec2 normalOffset = vec2(-direction.y, direction.x) * (max(u_DebugLineWidth, 1.0) * 0.5);

    // Two triangles: (A-, B-, B+) and (A-, B+, A+).
    bool useEnd = (corner == 1u) || (corner == 2u) || (corner == 4u);
    float side = ((corner == 2u) || (corner == 4u) || (corner == 5u)) ? 1.0 : -1.0;

    vec4 baseClip = useEnd ? clipB : clipA;
    vec2 basePixel = useEnd ? pixelB : pixelA;
    vec2 offsetPixel = basePixel + (normalOffset * side);
    vec2 offsetNdc = offsetPixel / halfViewport;

    // Re-multiply by w so the hardware divide lands exactly on offsetNdc while
    // depth and perspective interpolation stay correct.
    gl_Position = vec4(offsetNdc * baseClip.w, baseClip.z, baseClip.w);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_Color;

// Single target. ShaderDebugDrawPass narrows the scene framebuffer's draw
// attachments to colour[0] for the duration of the draw, so the entity-ID,
// view-normal and velocity attachments keep the values the geometry passes
// wrote instead of receiving undefined data from an overlay that has none.
layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = vec4(v_Color, 1.0);
}
