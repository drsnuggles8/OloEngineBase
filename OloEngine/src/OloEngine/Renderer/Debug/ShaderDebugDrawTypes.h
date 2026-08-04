#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>

// =============================================================================
// GPU-pushable shader debug draws — THE BUFFER CONTRACT (issue #725)
// =============================================================================
//
// This header is the single source of truth for the wire format shared by three
// parties that can never see each other's declarations:
//
//   1. the PUSH side — any GLSL stage that includes
//      `assets/shaders/include/DebugDrawCommon.glsl` and calls a
//      `OloDebugDraw*()` helper;
//   2. the DRAW side — `assets/shaders/DebugDrawPrimitives.glsl`, which expands
//      one channel per indirect draw into screen-space line quads;
//   3. the CPU side — `ShaderDebugDraw` (appender + per-frame prepare +
//      overflow readback) and `ShaderDebugDrawExpansion` (the CPU mirror of the
//      draw-side expansion that the parity test checks).
//
// Nothing enforces agreement between the three at compile time, so the sizes and
// offsets below are pinned by `ShaderDebugDrawContractTest` and the GLSL side is
// cross-checked against these constants by text comparison in the same test.
//
// -----------------------------------------------------------------------------
// One channel per primitive type
// -----------------------------------------------------------------------------
//
// Each primitive type owns one SSBO (`SSBO_DEBUG_DRAW_*`, bindings 46..52) laid
// out as a fixed 32-byte header followed by an entry array:
//
//     offset  0  uint VertexCount     \
//     offset  4  uint InstanceCount    |  a GL DrawArraysIndirectCommand,
//     offset  8  uint First            |  read straight off this buffer
//     offset 12  uint BaseInstance    /
//     offset 16  uint Capacity          entries the array can hold this frame
//     offset 20  uint RequestCount      UNCLAMPED append counter
//     offset 24  uint _pad0
//     offset 28  uint _pad1
//     offset 32  Entry Entries[Capacity]
//
// The indirect command living at offset 0 of the *same* buffer is deliberate:
// `RendererAPI::DrawArraysIndirect` binds the buffer as GL_DRAW_INDIRECT_BUFFER
// and reads the command at offset 0, so a channel needs no second allocation and
// the draw count is never round-tripped through the CPU.
//
// `VertexCount` is the per-instance vertex count of this primitive's line
// expansion (`ShaderDebugDrawContract::VertexCountPerInstance`) — constant per
// type, written by the CPU at prepare time. `InstanceCount` is the accepted
// entry count, so one instance == one primitive.
//
// -----------------------------------------------------------------------------
// Why there are TWO counters, and how the overflow flag falls out for free
// -----------------------------------------------------------------------------
//
// A push reserves its slot from `RequestCount`, which is never clamped:
//
//     uint slot = atomicAdd(RequestCount, 1u);
//     if (slot >= Capacity) return;          // dropped, but COUNTED
//     Entries[slot] = entry;
//     atomicAdd(InstanceCount, 1u);          // only accepted pushes draw
//
// so at end of frame `RequestCount` is the number of draws *asked for* and
// `InstanceCount == min(RequestCount, Capacity)` is the number *drawn*. Overflow
// is therefore exactly `RequestCount > Capacity`, with the excess telling you how
// much bigger the channel needs to be — no separate flag word, no extra atomic on
// the overflow path, and no way for the two to disagree.
//
// Incrementing `InstanceCount` only on the accepted path (rather than clamping
// `RequestCount` into it later) is what keeps the indirect draw in range: the
// GPU never sees an instance count that exceeds the entries actually written.
//
// -----------------------------------------------------------------------------
// Zero cost when disabled
// -----------------------------------------------------------------------------
//
// When the feature is off, `ShaderDebugDraw` leaves every channel at its
// header-only allocation with `Capacity == 0`. The push helpers open with a
// plain (non-atomic) `Capacity == 0u` test and return, so a shader that includes
// the header pays one scalar load per call site and nothing else: no atomics, no
// entry write. On the CPU side nothing is cleared, uploaded, read back or drawn,
// and the render pass declares no graph resources at all.
//
// The channels are still ALLOCATED and BOUND while disabled, and that is load
// bearing rather than sloppy: reading from an unbound SSBO is undefined in GL
// (the spec permits program termination), so the guard read needs a real buffer
// underneath it. 7 x 32 bytes is the price of the guard being safe.
//
// -----------------------------------------------------------------------------
// Coordinate spaces
// -----------------------------------------------------------------------------
//
// Every entry carries a `Space` tag; see `ShaderDebugDrawSpace`.
// =============================================================================

namespace OloEngine
{
    // @brief The seven primitive types, one append channel each.
    //
    // The enumerator order is load bearing: `ShaderDebugDraw` indexes its channel
    // array by it and derives each binding as `SSBO_DEBUG_DRAW_FIRST + index`,
    // and `DebugDrawPrimitives.glsl` switches on the same values. Pinned by
    // ShaderDebugDrawContractTest.
    enum class ShaderDebugDrawPrimitive : u32
    {
        Line = 0,
        Circle = 1,
        Rectangle = 2,
        AABB = 3,
        Box = 4,
        Cone = 5,
        Sphere = 6,
    };

    inline constexpr u32 kShaderDebugDrawPrimitiveCount = 7;

    // @brief The space an entry's positions are expressed in.
    //
    // World            — positions are world space; the draw applies the main
    //                    camera's view-projection. This is what a compute shader
    //                    reasoning about scene geometry wants.
    // MainCameraNDC    — positions are ALREADY main-camera normalised device
    //                    coordinates (x,y in [-1,1], z in [-1,1]); the draw
    //                    passes them through with w = 1. Use it to annotate
    //                    screen-space work (tile grids, cull rectangles) without
    //                    having to invert a projection in the pushing shader.
    // ObserverCameraNDC— positions are NDC of the *observer* camera: a second,
    //                    detached camera whose frustum you want to inspect from
    //                    the main view. The draw maps them observer-NDC → world
    //                    (via the observer's inverse view-projection) → main
    //                    clip. The observer camera itself is issue #726 and is
    //                    NOT built yet; until it is, `ShaderDebugDraw` uploads
    //                    the MAIN camera's inverse view-projection, which makes
    //                    this space round-trip exactly to MainCameraNDC rather
    //                    than produce garbage. The enumerator is live and
    //                    functional today, just degenerate.
    enum class ShaderDebugDrawSpace : u32
    {
        World = 0,
        MainCameraNDC = 1,
        ObserverCameraNDC = 2,
    };

    // -------------------------------------------------------------------------
    // Channel header — identical for all seven channels.
    // -------------------------------------------------------------------------
    struct ShaderDebugDrawChannelHeader
    {
        // --- DrawArraysIndirectCommand (GL reads these four at offset 0) ---
        u32 VertexCount = 0;   // verts per instance = this primitive's line expansion
        u32 InstanceCount = 0; // accepted entries; atomicAdd'd by the push helpers
        u32 First = 0;         // always 0
        u32 BaseInstance = 0;  // always 0

        u32 Capacity = 0;     // entries the array behind this header can hold
        u32 RequestCount = 0; // UNCLAMPED; overflow iff RequestCount > Capacity
        u32 _pad0 = 0;
        u32 _pad1 = 0;
    };
    static_assert(sizeof(ShaderDebugDrawChannelHeader) == 32,
                  "The channel header is the first 32 bytes of every debug-draw SSBO and is mirrored "
                  "verbatim by include/DebugDrawCommon.glsl — changing its size silently reinterprets "
                  "every entry array.");

    // -------------------------------------------------------------------------
    // Entry structs. Every one is std430-clean: `glm::vec3` occupies 12 bytes
    // with 16-byte alignment, so each is followed by a 4-byte scalar that lands
    // in the padding GLSL would insert anyway. Sizes pinned below.
    // -------------------------------------------------------------------------

    struct ShaderDebugDrawLine
    {
        glm::vec3 Start{ 0.0f };
        u32 Space = 0;
        glm::vec3 End{ 0.0f };
        f32 _pad0 = 0.0f;
        glm::vec3 Color{ 1.0f };
        f32 _pad1 = 0.0f;
    };
    static_assert(sizeof(ShaderDebugDrawLine) == 48);

    // A circle in the plane through `Center` with normal `Normal`. `Normal` need
    // not be unit length (the expansion normalises it) but must be non-zero.
    struct ShaderDebugDrawCircle
    {
        glm::vec3 Center{ 0.0f };
        u32 Space = 0;
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        f32 Radius = 1.0f;
        glm::vec3 Color{ 1.0f };
        f32 _pad0 = 0.0f;
    };
    static_assert(sizeof(ShaderDebugDrawCircle) == 48);

    // An arbitrarily oriented rectangle: the four corners are
    // `Center ± AxisU ± AxisV`, so both axes are HALF-extents and carry the
    // orientation. They are not required to be perpendicular — a sheared quad is
    // a legal (and occasionally useful) debug primitive.
    struct ShaderDebugDrawRectangle
    {
        glm::vec3 Center{ 0.0f };
        u32 Space = 0;
        glm::vec3 AxisU{ 1.0f, 0.0f, 0.0f };
        f32 _pad0 = 0.0f;
        glm::vec3 AxisV{ 0.0f, 1.0f, 0.0f };
        f32 _pad1 = 0.0f;
        glm::vec3 Color{ 1.0f };
        f32 _pad2 = 0.0f;
    };
    static_assert(sizeof(ShaderDebugDrawRectangle) == 64);

    // Axis-aligned in the entry's own coordinate space. `Min`/`Max` are not
    // re-ordered by the expansion — a swapped pair draws an inside-out box, which
    // is a useful thing to be able to SEE rather than have silently corrected.
    struct ShaderDebugDrawAABB
    {
        glm::vec3 Min{ 0.0f };
        u32 Space = 0;
        glm::vec3 Max{ 0.0f };
        f32 _pad0 = 0.0f;
        glm::vec3 Color{ 1.0f };
        f32 _pad1 = 0.0f;
    };
    static_assert(sizeof(ShaderDebugDrawAABB) == 48);

    // Eight EXPLICIT corners — the primitive for anything that is not an AABB and
    // not expressible as centre+extent+rotation either (a frustum slice, a
    // sheared cluster bound, an OBB you already have corners for). Corner index
    // bits are (bit0 = +U, bit1 = +V, bit2 = +W) relative to whatever basis the
    // producer used; the 12 edges are every pair differing in exactly one bit, so
    // the expansion never needs to know what that basis was.
    struct ShaderDebugDrawBox
    {
        std::array<glm::vec4, 8> Corners{}; // .w unused
        glm::vec3 Color{ 1.0f };
        u32 Space = 0;
    };
    static_assert(sizeof(ShaderDebugDrawBox) == 144);

    // A cone with its apex at `Apex` and its base circle centred at
    // `Apex + Axis`, of radius `Radius`. `Axis` therefore carries both direction
    // and height, which matches how a spot light's range/direction are already
    // stored and avoids a normalise-then-scale round trip in the pusher.
    struct ShaderDebugDrawCone
    {
        glm::vec3 Apex{ 0.0f };
        u32 Space = 0;
        glm::vec3 Axis{ 0.0f, -1.0f, 0.0f };
        f32 Radius = 1.0f;
        glm::vec3 Color{ 1.0f };
        f32 _pad0 = 0.0f;
    };
    static_assert(sizeof(ShaderDebugDrawCone) == 48);

    // Drawn as three axis-aligned great circles (a wire "gyroscope"), which reads
    // as a sphere from any angle without needing a camera-facing billboard and
    // therefore stays correct under the NDC coordinate spaces too.
    struct ShaderDebugDrawSphere
    {
        glm::vec3 Center{ 0.0f };
        f32 Radius = 1.0f;
        glm::vec3 Color{ 1.0f };
        u32 Space = 0;
    };
    static_assert(sizeof(ShaderDebugDrawSphere) == 32);

    // -------------------------------------------------------------------------
    // Expansion constants — how many line segments each primitive becomes.
    // -------------------------------------------------------------------------
    //
    // These numbers appear in three places: here, in `DebugDrawPrimitives.glsl`
    // (as the vertex-ID decode), and in `ShaderDebugDrawExpansion` (the CPU
    // mirror). `ShaderDebugDrawContractTest` reads the GLSL source and asserts the
    // literals match, because a mismatch is not a crash — the draw would simply
    // render part of a primitive, or read past the end of its segment table and
    // draw a degenerate quad, either of which looks like "the primitive is a bit
    // wrong" rather than "the constant drifted".
    namespace ShaderDebugDrawContract
    {
        inline constexpr u32 kCircleSegments = 32;     // circle ring
        inline constexpr u32 kSphereRingSegments = 32; // per great circle, x3
        inline constexpr u32 kConeRingSegments = 24;   // cone base ring
        inline constexpr u32 kConeSideLines = 4;       // apex -> base spokes
        inline constexpr u32 kVerticesPerSegment = 6;  // one screen-space quad

        // Segments each primitive expands to.
        [[nodiscard]] constexpr u32 SegmentCount(ShaderDebugDrawPrimitive primitive)
        {
            switch (primitive)
            {
                case ShaderDebugDrawPrimitive::Line:
                    return 1;
                case ShaderDebugDrawPrimitive::Circle:
                    return kCircleSegments;
                case ShaderDebugDrawPrimitive::Rectangle:
                    return 4;
                case ShaderDebugDrawPrimitive::AABB:
                    return 12;
                case ShaderDebugDrawPrimitive::Box:
                    return 12;
                case ShaderDebugDrawPrimitive::Cone:
                    return kConeRingSegments + kConeSideLines;
                case ShaderDebugDrawPrimitive::Sphere:
                    return 3 * kSphereRingSegments;
            }
            return 0;
        }

        // The DrawArraysIndirectCommand `count` field for this primitive.
        [[nodiscard]] constexpr u32 VertexCountPerInstance(ShaderDebugDrawPrimitive primitive)
        {
            return SegmentCount(primitive) * kVerticesPerSegment;
        }

        // Bytes one entry of this primitive occupies in its channel.
        [[nodiscard]] constexpr u32 EntryStride(ShaderDebugDrawPrimitive primitive)
        {
            switch (primitive)
            {
                case ShaderDebugDrawPrimitive::Line:
                    return sizeof(ShaderDebugDrawLine);
                case ShaderDebugDrawPrimitive::Circle:
                    return sizeof(ShaderDebugDrawCircle);
                case ShaderDebugDrawPrimitive::Rectangle:
                    return sizeof(ShaderDebugDrawRectangle);
                case ShaderDebugDrawPrimitive::AABB:
                    return sizeof(ShaderDebugDrawAABB);
                case ShaderDebugDrawPrimitive::Box:
                    return sizeof(ShaderDebugDrawBox);
                case ShaderDebugDrawPrimitive::Cone:
                    return sizeof(ShaderDebugDrawCone);
                case ShaderDebugDrawPrimitive::Sphere:
                    return sizeof(ShaderDebugDrawSphere);
            }
            return 0;
        }

        [[nodiscard]] constexpr const char* Name(ShaderDebugDrawPrimitive primitive)
        {
            switch (primitive)
            {
                case ShaderDebugDrawPrimitive::Line:
                    return "Line";
                case ShaderDebugDrawPrimitive::Circle:
                    return "Circle";
                case ShaderDebugDrawPrimitive::Rectangle:
                    return "Rectangle";
                case ShaderDebugDrawPrimitive::AABB:
                    return "AABB";
                case ShaderDebugDrawPrimitive::Box:
                    return "Box";
                case ShaderDebugDrawPrimitive::Cone:
                    return "Cone";
                case ShaderDebugDrawPrimitive::Sphere:
                    return "Sphere";
            }
            return "Unknown";
        }

        // Byte offset of the entry array — i.e. the header size.
        inline constexpr u32 kEntryArrayOffset = sizeof(ShaderDebugDrawChannelHeader);
    } // namespace ShaderDebugDrawContract

    // -------------------------------------------------------------------------
    // Render-side params UBO (binding UBO_DEBUG_DRAW = 57), std140.
    // -------------------------------------------------------------------------
    struct ShaderDebugDrawParamsUBO
    {
        glm::mat4 ViewProjection{ 1.0f };            // world -> main clip
        glm::mat4 ObserverInvViewProjection{ 1.0f }; // observer NDC -> world
        glm::vec2 ViewportSize{ 1.0f };              // pixels
        f32 LineWidth = 2.0f;                        // pixels
        u32 PrimitiveType = 0;                       // ShaderDebugDrawPrimitive
    };
    static_assert(sizeof(ShaderDebugDrawParamsUBO) % 16 == 0,
                  "std140 requires a 16-byte-multiple block size");
    static_assert(sizeof(ShaderDebugDrawParamsUBO) == 144);

    // -------------------------------------------------------------------------
    // Per-frame drain of the channel headers (see ShaderDebugDraw::GetStats()).
    // -------------------------------------------------------------------------
    struct ShaderDebugDrawChannelStats
    {
        u32 Capacity = 0;
        u32 Drawn = 0;     // == InstanceCount
        u32 Requested = 0; // == RequestCount
        u32 CpuPushes = 0; // of `Drawn`/`Requested`, how many came from the CPU

        [[nodiscard]] constexpr bool Overflowed() const
        {
            return Requested > Capacity;
        }
        [[nodiscard]] constexpr u32 Dropped() const
        {
            return Requested > Capacity ? Requested - Capacity : 0u;
        }
    };

    struct ShaderDebugDrawStats
    {
        std::array<ShaderDebugDrawChannelStats, kShaderDebugDrawPrimitiveCount> Channels{};
        bool StatsValid = false; // false until the first readback lands

        [[nodiscard]] bool AnyOverflow() const
        {
            for (const auto& channel : Channels)
            {
                if (channel.Overflowed())
                    return true;
            }
            return false;
        }
    };
} // namespace OloEngine
