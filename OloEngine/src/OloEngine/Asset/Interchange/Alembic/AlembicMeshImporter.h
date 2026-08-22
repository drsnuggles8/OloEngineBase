#pragma once

// The Alembic (.abc) importer only exists when the OLO_WITH_ALEMBIC CMake option compiled the
// Alembic + Imath dependency in. When it's off this header is empty and the registry never
// references the class, so the engine builds with zero Alembic footprint.
#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Asset/Interchange/MeshImporter.h"

namespace OloEngine
{
    // Imports Alembic geometry caches (.abc, Ogawa backend) into a MeshSource (issue #655).
    // Reads IPolyMesh / ISubD prims' rest-pose (sample 0) positions, normals, and UVs,
    // fan-triangulates, and accumulates each prim's world transform from its IXform parents.
    //
    // Scope (documented in the PR): static rest-pose geometry only. Alembic
    // carries no standardized PBR material (its material side-channel is DCC-specific shader
    // dictionaries), so every submesh gets the engine-default material — callers assign their
    // own. Multi-sample vertex animation (schema.getNumSamples() > 1) is detected and logged but
    // baked playback is a follow-up.
    class AlembicMeshImporter final : public MeshImporter
    {
      public:
        [[nodiscard]] MeshImportResult Import(const std::filesystem::path& path,
                                              const MeshImportOptions& options = {}) override;

        [[nodiscard]] std::string_view GetName() const override
        {
            return "Alembic";
        }
    };
} // namespace OloEngine

#endif // OLO_WITH_ALEMBIC
