#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Interchange/MeshExporter.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Process-wide registry mapping a lower-cased extension (no leading dot) to the
    // MeshExporter that writes it. Built-ins register lazily (std::call_once), same pattern
    // as MeshImporterRegistry: AssimpMeshExporter claims gltf/glb. Unlike the import side
    // there is no fallback — an unknown export target is an explicit error.
    class MeshExporterRegistry
    {
      public:
        static MeshExporterRegistry& Get();

        void Register(std::string_view extension, Scope<MeshExporter> exporter);

        [[nodiscard]] MeshExporter* Find(std::string_view extension) const;

        // Resolve the exporter from the path's extension and run it.
        [[nodiscard]] MeshExportResult Export(const MeshSource& source,
                                              const std::filesystem::path& path,
                                              const MeshExportOptions& options = {}) const;

        [[nodiscard]] bool IsSupported(std::string_view extension) const;

        [[nodiscard]] std::vector<std::string> GetRegisteredExtensions() const;

      private:
        MeshExporterRegistry() = default;

        void EnsureBuiltinsRegistered() const;

        mutable std::unordered_map<std::string, Scope<MeshExporter>> m_Exporters;
    };
} // namespace OloEngine
