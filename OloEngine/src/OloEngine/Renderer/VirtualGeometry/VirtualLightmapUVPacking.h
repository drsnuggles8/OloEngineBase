#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // ── Where a virtual mesh's baked lightmap UV2 lives (issue #867) ─────────
    //
    // It has NO binding of its own, and that is not an oversight. The SSBO
    // namespace is full below Mesa's hard ceiling of 80
    // (docs/agent-rules/ssbo-binding-cap-is-80-on-mesa.md), and the one number
    // that looked reusable — SSBO_BONE_PULL (63) — cannot serve this path:
    // VulkanRendererAPI::AssembleRootData resolves 57/63 from the DRAW'S VAO
    // STREAMS and never from a published buffer, and the mesh-shader route
    // passes no VAO at all, so a real buffer published there is ignored and the
    // shader reads the frame arena's fixed 64 KiB null block instead.
    //
    // So the UV2 rides the cluster VERTEX ARENA (SSBO_VIRTUAL_VERTICES, 39) as a
    // packed tail region — the first route ShaderBindingLayout.h names when the
    // namespace is out of numbers ("ride an existing block"):
    //
    //     [ vertices:  SlotCount * SlotVertexCapacity elements ]
    //     [ uv2 tail:  SlotCount * SlotVertexCapacity / 4 elements ]
    //
    // FOUR uv2 pairs pack into one 32-byte VirtualGpuVertex element (8 floats),
    // so the tail costs +12.5% of the vertex arena — and only when a registered
    // mesh actually carries UV2. A scene with no baked virtual geometry
    // allocates exactly what it did before. The alternative, widening
    // VirtualGpuVertex to 48 bytes, would have cost +50% of an arena that is
    // hundreds of MB on a Sponza-scale scene, paid by every VG scene whether it
    // has a bake or not — the same trade baked-lightmap-pipeline.md §1 already
    // rejected for `Vertex`.
    //
    // ── Why the addressing needs no per-page fixup ───────────────────────────
    //
    // Two facts, and BOTH are load-bearing:
    //  1. VirtualMeshRegistry::LoadPage copies a page's vertices to
    //     `slotVertexBase * sizeof(VirtualGpuVertex)`, so every page starts at
    //     slot-local index 0.
    //  2. The registry rounds SlotVertexCapacity UP to a multiple of 4, so
    //     `slotVertexBase = slot * capacity` is always 4-aligned.
    //
    // Together those make the lane of a global index equal the lane of its
    // slot-local index, so the CPU can pack a page from 0 and the shader can go
    // straight from a global vertex index to (element, lane). Break either one
    // and this silently addresses the wrong texels.
    //
    // The GLSL twin is `oloVirtualLightmapUVElement` / `oloVirtualLightmapUVLane`
    // in OloEditor/assets/shaders/include/VirtualDrawInfo.glsl. Change one,
    // change both — the failure mode is not an error, it is sampling another
    // mesh's charts.

    // uv2 pairs packed into one 32-byte arena element.
    inline constexpr u32 kVirtualLightmapUVsPerElement = 4;

    // Elements needed to hold `vertexCount` uv2 pairs.
    [[nodiscard("returns the packed element count; it computes nothing else")]] inline constexpr u32
    VirtualLightmapUVElementCount(u32 vertexCount) noexcept
    {
        return (vertexCount + kVirtualLightmapUVsPerElement - 1) / kVirtualLightmapUVsPerElement;
    }

    // Element offset, RELATIVE to the tail's base element, holding this vertex.
    [[nodiscard("returns the element offset; it computes nothing else")]] inline constexpr u32
    VirtualLightmapUVElementOffset(u32 globalVertexIndex) noexcept
    {
        return globalVertexIndex >> 2;
    }

    // Write one uv2 into its lane of `element`.
    //
    // Lanes 0..3 are (PositionU.xy, PositionU.zw, NormalV.xy, NormalV.zw). The
    // field names belong to the vertex layout this region borrows; here the
    // element is eight raw floats and nothing else.
    inline void PackVirtualLightmapUV(VirtualGpuVertex& element, u32 globalVertexIndex,
                                      const glm::vec2& uv) noexcept
    {
        const u32 lane = globalVertexIndex & 3u;
        glm::vec4& pair = ((lane & 2u) == 0u) ? element.PositionU : element.NormalV;
        if ((lane & 1u) == 0u)
        {
            pair.x = uv.x;
            pair.y = uv.y;
        }
        else
        {
            pair.z = uv.x;
            pair.w = uv.y;
        }
    }

    // The C++ twin of the shader's read — used by tests and by any CPU-side
    // consumer that needs to read a packed uv2 back.
    [[nodiscard("returns the unpacked uv; it does not modify element")]] inline glm::vec2
    UnpackVirtualLightmapUV(const VirtualGpuVertex& element, u32 globalVertexIndex) noexcept
    {
        const u32 lane = globalVertexIndex & 3u;
        const glm::vec4& pair = ((lane & 2u) == 0u) ? element.PositionU : element.NormalV;
        return ((lane & 1u) == 0u) ? glm::vec2(pair.x, pair.y) : glm::vec2(pair.z, pair.w);
    }
} // namespace OloEngine
