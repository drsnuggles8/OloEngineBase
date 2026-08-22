#pragma once

#include "OloEngine/Renderer/MeshSource.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine
{
    // Options for a mesh export. Small on purpose; add cross-format knobs only.
    struct MeshExportOptions
    {
        // When the target container supports both text and binary forms (glTF: .gltf vs .glb),
        // the registry picks the exporter/format from the path extension, so this is only a
        // hint for exporters that can't infer it. Left here for future formats.
        bool Binary = false;
    };

    struct MeshExportResult
    {
        bool Success = false;
        std::string Error;

        static MeshExportResult Ok()
        {
            return MeshExportResult{ true, {} };
        }
        static MeshExportResult Failure(std::string error)
        {
            return MeshExportResult{ false, std::move(error) };
        }
    };

    // Abstract writer from the engine's MeshSource to an on-disk interchange file. The export
    // axis is deliberately separate from MeshImporter (issue #655): importing and
    // exporting have different call sites and formats, so overloading one interface for both
    // would muddy each. glTF export (the first and only requested exporter) is implemented on
    // top of the already-vendored Assimp Exporter, so no new dependency is needed.
    class MeshExporter
    {
      public:
        virtual ~MeshExporter() = default;

        // Write `source` to `path`. Never throws across the boundary — failures are reported
        // via MeshExportResult::Error. The MeshSource need not have been Build()'d; only CPU
        // geometry (vertices/indices/submeshes) + imported materials are read.
        [[nodiscard]] virtual MeshExportResult Export(const MeshSource& source,
                                                      const std::filesystem::path& path,
                                                      const MeshExportOptions& options = {}) = 0;

        [[nodiscard]] virtual std::string_view GetName() const = 0;
    };
} // namespace OloEngine
