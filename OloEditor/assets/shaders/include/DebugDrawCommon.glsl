// =============================================================================
// DebugDrawCommon.glsl — GPU-pushable shader debug draws, PUSH side (issue #725)
// =============================================================================
//
// Include this from ANY stage (vertex / fragment / compute), NAMING THE CHANNELS
// YOU WANT, and call the matching OloDebugDraw*() helper to emit debug geometry
// that appears in the viewport in the same frame:
//
//     #define OLO_DEBUG_DRAW_AABB
//     #include "include/DebugDrawCommon.glsl"
//     ...
//     OloDebugDrawAABB(worldMin, worldMax, vec3(1.0, 0.0, 0.0), OLO_DEBUG_SPACE_WORLD);
//
// The channels are OPT-IN per primitive because each one is a storage block, and
// storage blocks are a scarce per-stage resource: GL only guarantees 8 per stage,
// and NVIDIA's compute limit is 16 — which `VirtualClusterCull.comp`, at 9 blocks
// of its own, would blow through if this header declared all seven
// unconditionally. Declare the one or two you actually push. `OLO_DEBUG_DRAW_ALL`
// turns on every channel and exists for the draw-side shader, which by
// definition needs them all.
//
// Recognised: OLO_DEBUG_DRAW_LINE / _CIRCLE / _RECTANGLE / _AABB / _BOX / _CONE
//             / _SPHERE / _ALL. Naming none declares nothing, so a stale include
//             costs literally zero.
//
// The full buffer contract lives in
// OloEngine/src/OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h. What matters
// here:
//
// * Each primitive has its OWN append channel (SSBO 46..52). Every channel
//   opens with the same 32-byte header — a GL DrawArraysIndirectCommand
//   followed by Capacity + RequestCount — so the draw count never travels
//   through the CPU.
//
// * A push reserves its slot from the UNCLAMPED RequestCount and only bumps
//   InstanceCount (== the indirect draw's instance count) once it has actually
//   written an entry. So the entries the GPU draws are always in range, and
//   `RequestCount > Capacity` is exactly "you overflowed and N draws were
//   dropped" — which the engine reads back and reports rather than swallowing.
//
// * ZERO COST WHEN DISABLED. Every helper opens with a plain, non-atomic
//   `Capacity == 0u` test and returns. When the feature is off the engine leaves
//   every channel header-only with Capacity 0, so a push site costs one scalar
//   load and nothing else — no atomic, no write, no bandwidth. The channels stay
//   ALLOCATED and BOUND while disabled precisely so that guard read is defined;
//   reading an unbound SSBO is not.
//
// * Positions are interpreted per the entry's `space` tag; see the
//   OLO_DEBUG_SPACE_* constants. World is what a shader reasoning about scene
//   geometry wants; the NDC spaces let you annotate screen-space work without
//   inverting a projection in the pushing shader.
//
// * Push volume is YOUR problem. A per-cluster or per-pixel push will overflow
//   a 4096-entry channel instantly — which is reported rather than silent, but
//   it is still not a visualization. Gate high-frequency pushes on something
//   (a selected entity, a screen region, an every-Nth-index test).
// =============================================================================

#ifndef OLO_DEBUG_DRAW_COMMON_GLSL
#define OLO_DEBUG_DRAW_COMMON_GLSL

#ifdef OLO_DEBUG_DRAW_ALL
#define OLO_DEBUG_DRAW_LINE
#define OLO_DEBUG_DRAW_CIRCLE
#define OLO_DEBUG_DRAW_RECTANGLE
#define OLO_DEBUG_DRAW_AABB
#define OLO_DEBUG_DRAW_BOX
#define OLO_DEBUG_DRAW_CONE
#define OLO_DEBUG_DRAW_SPHERE
#endif

// Coordinate-space tags — must match OloEngine::ShaderDebugDrawSpace.
#define OLO_DEBUG_SPACE_WORLD 0u
#define OLO_DEBUG_SPACE_MAIN_NDC 1u
#define OLO_DEBUG_SPACE_OBSERVER_NDC 2u

// -----------------------------------------------------------------------------
// Line — SSBO 46
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_LINE
struct OloDebugLineEntry
{
    vec3 Start;
    uint Space;
    vec3 End;
    float _pad0;
    vec3 Color;
    float _pad1;
};

layout(std430, binding = 46) buffer OloDebugDrawLineChannel
{
    // These four ARE the DrawArraysIndirectCommand the engine hands to
    // glDrawArraysIndirect at offset 0 of this same buffer.
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugLineEntry Entries[];
} g_OloDebugLines;

// The shape every helper below repeats, and the ORDER of the two atomics is the
// contract: reserve from the unclamped RequestCount (so overflow stays
// countable), bail if the slot is past capacity (so the write stays in bounds),
// write, and only then bump InstanceCount (so the indirect draw never covers an
// unwritten slot).
void OloDebugDrawLine(vec3 start, vec3 end, vec3 color, uint space)
{
    if (g_OloDebugLines.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugLines.RequestCount, 1u);
    if (slot >= g_OloDebugLines.Capacity)
        return;
    g_OloDebugLines.Entries[slot].Start = start;
    g_OloDebugLines.Entries[slot].Space = space;
    g_OloDebugLines.Entries[slot].End = end;
    g_OloDebugLines.Entries[slot].Color = color;
    atomicAdd(g_OloDebugLines.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_LINE

// -----------------------------------------------------------------------------
// Circle — SSBO 47
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_CIRCLE
struct OloDebugCircleEntry
{
    vec3 Center;
    uint Space;
    vec3 Normal;
    float Radius;
    vec3 Color;
    float _pad0;
};

layout(std430, binding = 47) buffer OloDebugDrawCircleChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugCircleEntry Entries[];
} g_OloDebugCircles;

void OloDebugDrawCircle(vec3 center, vec3 normal, float radius, vec3 color, uint space)
{
    if (g_OloDebugCircles.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugCircles.RequestCount, 1u);
    if (slot >= g_OloDebugCircles.Capacity)
        return;
    g_OloDebugCircles.Entries[slot].Center = center;
    g_OloDebugCircles.Entries[slot].Space = space;
    g_OloDebugCircles.Entries[slot].Normal = normal;
    g_OloDebugCircles.Entries[slot].Radius = radius;
    g_OloDebugCircles.Entries[slot].Color = color;
    atomicAdd(g_OloDebugCircles.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_CIRCLE

// -----------------------------------------------------------------------------
// Rectangle — SSBO 48. Corners are Center +- AxisU +- AxisV, so both axes are
// HALF-extents and carry the orientation; they need not be perpendicular.
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_RECTANGLE
struct OloDebugRectangleEntry
{
    vec3 Center;
    uint Space;
    vec3 AxisU;
    float _pad0;
    vec3 AxisV;
    float _pad1;
    vec3 Color;
    float _pad2;
};

layout(std430, binding = 48) buffer OloDebugDrawRectangleChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugRectangleEntry Entries[];
} g_OloDebugRectangles;

void OloDebugDrawRectangle(vec3 center, vec3 axisU, vec3 axisV, vec3 color, uint space)
{
    if (g_OloDebugRectangles.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugRectangles.RequestCount, 1u);
    if (slot >= g_OloDebugRectangles.Capacity)
        return;
    g_OloDebugRectangles.Entries[slot].Center = center;
    g_OloDebugRectangles.Entries[slot].Space = space;
    g_OloDebugRectangles.Entries[slot].AxisU = axisU;
    g_OloDebugRectangles.Entries[slot].AxisV = axisV;
    g_OloDebugRectangles.Entries[slot].Color = color;
    atomicAdd(g_OloDebugRectangles.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_RECTANGLE

// -----------------------------------------------------------------------------
// AABB — SSBO 49. Axis-aligned in the entry's own space. Min/Max are NOT
// re-ordered by the draw: a swapped pair draws an inside-out box, which is a
// useful thing to be able to see rather than have silently corrected.
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_AABB
struct OloDebugAABBEntry
{
    vec3 Min;
    uint Space;
    vec3 Max;
    float _pad0;
    vec3 Color;
    float _pad1;
};

layout(std430, binding = 49) buffer OloDebugDrawAABBChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugAABBEntry Entries[];
} g_OloDebugAABBs;

void OloDebugDrawAABB(vec3 minCorner, vec3 maxCorner, vec3 color, uint space)
{
    if (g_OloDebugAABBs.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugAABBs.RequestCount, 1u);
    if (slot >= g_OloDebugAABBs.Capacity)
        return;
    g_OloDebugAABBs.Entries[slot].Min = minCorner;
    g_OloDebugAABBs.Entries[slot].Space = space;
    g_OloDebugAABBs.Entries[slot].Max = maxCorner;
    g_OloDebugAABBs.Entries[slot].Color = color;
    atomicAdd(g_OloDebugAABBs.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_AABB

// -----------------------------------------------------------------------------
// Box — SSBO 50. Eight EXPLICIT corners, for anything that is neither an AABB
// nor expressible as centre+extent+rotation (a frustum slice, a sheared cluster
// bound). Corner index bits are (bit0 = +U, bit1 = +V, bit2 = +W) in whatever
// basis you used; the draw connects every pair differing in exactly one bit, so
// the basis never has to be communicated.
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_BOX
struct OloDebugBoxEntry
{
    vec4 Corners[8];
    vec3 Color;
    uint Space;
};

layout(std430, binding = 50) buffer OloDebugDrawBoxChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugBoxEntry Entries[];
} g_OloDebugBoxes;

void OloDebugDrawBox(vec3 corners[8], vec3 color, uint space)
{
    if (g_OloDebugBoxes.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugBoxes.RequestCount, 1u);
    if (slot >= g_OloDebugBoxes.Capacity)
        return;
    for (int i = 0; i < 8; ++i)
        g_OloDebugBoxes.Entries[slot].Corners[i] = vec4(corners[i], 1.0);
    g_OloDebugBoxes.Entries[slot].Color = color;
    g_OloDebugBoxes.Entries[slot].Space = space;
    atomicAdd(g_OloDebugBoxes.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_BOX

// -----------------------------------------------------------------------------
// Cone — SSBO 51. Apex at `apex`, base circle centred at `apex + axis`, so the
// axis carries both direction and height — matching how a spot light's
// range/direction are already stored.
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_CONE
struct OloDebugConeEntry
{
    vec3 Apex;
    uint Space;
    vec3 Axis;
    float Radius;
    vec3 Color;
    float _pad0;
};

layout(std430, binding = 51) buffer OloDebugDrawConeChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugConeEntry Entries[];
} g_OloDebugCones;

void OloDebugDrawCone(vec3 apex, vec3 axis, float radius, vec3 color, uint space)
{
    if (g_OloDebugCones.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugCones.RequestCount, 1u);
    if (slot >= g_OloDebugCones.Capacity)
        return;
    g_OloDebugCones.Entries[slot].Apex = apex;
    g_OloDebugCones.Entries[slot].Space = space;
    g_OloDebugCones.Entries[slot].Axis = axis;
    g_OloDebugCones.Entries[slot].Radius = radius;
    g_OloDebugCones.Entries[slot].Color = color;
    atomicAdd(g_OloDebugCones.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_CONE

// -----------------------------------------------------------------------------
// Sphere — SSBO 52. Drawn as three axis-aligned great circles, so it reads as a
// sphere from any angle without a camera-facing billboard (which would go wrong
// under the NDC coordinate spaces).
// -----------------------------------------------------------------------------
#ifdef OLO_DEBUG_DRAW_SPHERE
struct OloDebugSphereEntry
{
    vec3 Center;
    float Radius;
    vec3 Color;
    uint Space;
};

layout(std430, binding = 52) buffer OloDebugDrawSphereChannel
{
    uint VertexCount;
    uint InstanceCount;
    uint First;
    uint BaseInstance;
    uint Capacity;
    uint RequestCount;
    uint _headerPad0;
    uint _headerPad1;
    OloDebugSphereEntry Entries[];
} g_OloDebugSpheres;

void OloDebugDrawSphere(vec3 center, float radius, vec3 color, uint space)
{
    if (g_OloDebugSpheres.Capacity == 0u)
        return;
    uint slot = atomicAdd(g_OloDebugSpheres.RequestCount, 1u);
    if (slot >= g_OloDebugSpheres.Capacity)
        return;
    g_OloDebugSpheres.Entries[slot].Center = center;
    g_OloDebugSpheres.Entries[slot].Radius = radius;
    g_OloDebugSpheres.Entries[slot].Color = color;
    g_OloDebugSpheres.Entries[slot].Space = space;
    atomicAdd(g_OloDebugSpheres.InstanceCount, 1u);
}
#endif // OLO_DEBUG_DRAW_SPHERE

#endif // OLO_DEBUG_DRAW_COMMON_GLSL
