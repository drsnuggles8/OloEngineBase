#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class MeshSource;

    // The unwrap parameters shared by the BAKE and the runtime RESOLVE
    // (SceneLightmapRuntime::Resolve). They must be identical in both places:
    // the unwrap is deterministic, so a mesh whose UV2 stream was not persisted
    // (procedural primitives have no .omesh) is re-unwrapped at load and lands
    // bit-identically on the layout the bake rasterized — that is what makes
    // the resolve self-healing instead of permanently stale after a restart.
    inline constexpr u32 kLightmapUnwrapResolution = 512;
    inline constexpr u32 kLightmapUnwrapPadding = 4;

    struct LightmapUnwrapOptions
    {
        u32 Resolution = kLightmapUnwrapResolution; // xatlas pack resolution for the per-mesh atlas (pow2 recommended).
                                                    // 0 = let xatlas grow a single atlas sized from TexelsPerUnit
                                                    // (never multi-page); non-zero fixes the page size, and charts
                                                    // that overflow into a second page make Generate() fail.
        u32 Padding = kLightmapUnwrapPadding;       // chart padding in texels at that resolution
        f32 TexelsPerUnit = 0.0f;                   // 0 = let xatlas derive from Resolution
    };

    class LightmapUnwrap
    {
      public:
        // Generates a lightmap parameterization for `meshSource` IN PLACE:
        //  - runs xatlas over every submesh (one xatlas mesh per submesh, one shared atlas
        //    per MeshSource so charts of all submeshes pack together),
        //  - rebuilds m_Vertices / m_Indices with xatlas's seam-split vertices (new vertex
        //    = copy of original via xref, so Position/Normal/TexCoord are preserved
        //    bit-exactly; vertices referenced by no triangle are dropped),
        //  - updates every Submesh's BaseVertex/BaseIndex/VertexCount/IndexCount (indices
        //    stay GLOBAL into the rebuilt vertex array, matching the engine contract),
        //  - writes the normalized [0,1] UV2 stream via SetLightmapUVs (uv / actual atlas dims),
        //  - regenerates the position-merged shadow index buffer when one existed (it
        //    referenced the old vertex order), and leaves the source un-Built so the caller
        //    re-Builds GPU buffers (Build() is never called here — no GL required),
        //  - returns true on success; on ANY failure returns false and leaves the
        //    MeshSource COMPLETELY untouched (built into locals, committed at the end).
        //
        // Refuses (returns false, logs) meshes with: a skeleton or real bone influences,
        // morph targets, or a cooked virtual-mesh blob (their per-vertex/per-index data
        // would go stale) — and meshes that already HaveLightmapUVs (idempotence: returns
        // true, does nothing). A MeshSource with no submeshes is unwrapped as one implicit
        // whole-mesh range. Multiple atlas pages (charts did not fit Resolution) are a
        // failure for v1 — raise Resolution at the call site instead.
        //
        // Deterministic: the same input mesh + options produce identical output (xatlas
        // default ChartOptions/PackOptions, meshes added in submesh order).
        [[nodiscard]] static bool Generate(MeshSource& meshSource, const LightmapUnwrapOptions& options);
    };
} // namespace OloEngine
