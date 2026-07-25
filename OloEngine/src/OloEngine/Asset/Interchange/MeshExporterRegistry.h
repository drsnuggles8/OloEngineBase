#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Interchange/MeshExporter.h"

#include <filesystem>
#include <memory>
#include <mutex>
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

        // Returns an OWNERSHIP-RETAINING handle (not a raw pointer): the caller holds a shared
        // reference for as long as it uses the exporter, so a concurrent Register() replacing the
        // slot cannot free the exporter out from under an in-flight Export().
        [[nodiscard]] std::shared_ptr<MeshExporter> Find(std::string_view extension) const;

        // Resolve the exporter from the path's extension and run it.
        [[nodiscard]] MeshExportResult Export(const MeshSource& source,
                                              const std::filesystem::path& path,
                                              const MeshExportOptions& options = {}) const;

        [[nodiscard]] bool IsSupported(std::string_view extension) const;

        [[nodiscard]] std::vector<std::string> GetRegisteredExtensions() const;

      private:
        MeshExporterRegistry() = default;

        // Registers the built-in exporters exactly once. MUST be called with m_Mutex held.
        void EnsureBuiltinsRegistered() const;

        mutable std::mutex m_Mutex;
        // shared_ptr (not Scope) so Find() can hand out an ownership-retaining handle; guarded by
        // m_Mutex on every access.
        mutable std::unordered_map<std::string, std::shared_ptr<MeshExporter>> m_Exporters;
    };
} // namespace OloEngine
