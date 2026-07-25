#pragma once

// The OpenUSD importer only exists when the OLO_WITH_USD CMake option compiled the OpenUSD +
// oneTBB dependency in. When off, this header is empty and the registry never references it,
// so the default engine build has zero USD footprint (USD is a large, opt-in dependency —
// see docs/agent-rules for the ExternalProject vendoring story).
#if defined(OLO_WITH_USD)

#include "OloEngine/Asset/Interchange/MeshImporter.h"

namespace OloEngine
{
    // Imports OpenUSD stages (.usd/.usda/.usdc/.usdz) into a MeshSource (issue #655 Tier 1).
    // Traverses every UsdGeomMesh, triangulates it honoring the prim's orientation (winding),
    // resolves per-corner normals + primvars:st UVs by their declared interpolation, composes
    // each prim's world transform (UsdGeomXformCache) with the stage up-axis + metersPerUnit,
    // and maps the bound UsdPreviewSurface material's factors to the engine PBR material.
    //
    // First-slice scope (documented in the PR): static-mesh import (default time sample), one
    // submesh per mesh prim with the mesh-level bound material. Stage/layer composition beyond
    // what UsdStage::Open resolves, GeomSubset per-face-group materials, skinning/animation,
    // point instancers, and usdz-embedded texture extraction are follow-ups.
    class UsdMeshImporter final : public MeshImporter
    {
      public:
        [[nodiscard]] MeshImportResult Import(const std::filesystem::path& path,
                                              const MeshImportOptions& options = {}) override;

        [[nodiscard]] std::string_view GetName() const override
        {
            return "OpenUSD";
        }
    };
} // namespace OloEngine

#endif // OLO_WITH_USD
