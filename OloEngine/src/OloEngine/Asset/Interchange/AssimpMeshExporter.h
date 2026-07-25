#pragma once

#include "OloEngine/Asset/Interchange/MeshExporter.h"

namespace OloEngine
{
    // glTF 2.0 exporter built on the already-vendored Assimp Exporter (format ids "gltf2" /
    // "glb2"), so no new dependency is added (issue #655 Tier 3). Builds a transient aiScene
    // from the MeshSource — one aiMesh per submesh so per-submesh material bindings survive —
    // maps each imported PBR material to an aiMaterial (base-color/metallic/roughness/emissive
    // factors + texture URIs), and writes .gltf (text) or .glb (binary) chosen by extension.
    class AssimpMeshExporter final : public MeshExporter
    {
      public:
        [[nodiscard]] MeshExportResult Export(const MeshSource& source,
                                              const std::filesystem::path& path,
                                              const MeshExportOptions& options = {}) override;

        [[nodiscard]] std::string_view GetName() const override
        {
            return "Assimp-glTF";
        }
    };
} // namespace OloEngine
