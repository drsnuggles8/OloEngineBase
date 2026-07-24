#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Interchange/MeshImporter.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Process-wide registry that maps a lower-cased file extension (no leading dot) to the
    // MeshImporter that handles it, plus a single fallback importer used when no extension
    // match exists. This is the dispatch half of issue #655's interchange abstraction:
    // MeshSourceSerializer::TryLoadData resolves a translator here by extension instead of
    // hard-coding the Assimp Model path.
    //
    // Built-ins register lazily on first use (std::call_once, mirroring AssetExtensions):
    //   * AssimpMeshImporter is the FALLBACK (and claims the classic Assimp extensions), so
    //     every extension that reached Assimp before still does — behaviour-preserving.
    //   * UsdMeshImporter / AlembicMeshImporter claim their own extensions, compiled in only
    //     when OLO_WITH_USD / OLO_WITH_ALEMBIC are defined.
    //
    // Thread-safety: registration completes inside call_once before any lookup returns, and
    // the maps are read-only afterwards, so concurrent Import() calls from the async asset
    // workers are safe without per-call locking.
    class MeshImporterRegistry
    {
      public:
        static MeshImporterRegistry& Get();

        // Register `importer` for a single normalized extension (case-insensitive, dot optional).
        // A later registration for the same extension replaces the earlier one (USD/Alembic
        // deliberately override the Assimp fallback for their formats).
        void Register(std::string_view extension, Scope<MeshImporter> importer);

        // Register the importer used when no per-extension entry matches. Assimp claims this.
        void SetFallback(Scope<MeshImporter> importer);

        // Resolve the importer for `extension` (per-extension entry, else the fallback, else
        // nullptr). Extension may include or omit the leading dot and any case.
        [[nodiscard]] MeshImporter* Find(std::string_view extension) const;

        // Convenience: resolve the importer from the path's extension and run it. Returns a
        // failure result (never throws) when no importer can handle the extension.
        [[nodiscard]] MeshImportResult Import(const std::filesystem::path& path,
                                              const MeshImportOptions& options = {}) const;

        // True when Find() would return a non-null importer for this extension.
        [[nodiscard]] bool IsSupported(std::string_view extension) const;

        // Sorted list of explicitly-registered extensions (excludes the fallback catch-all).
        [[nodiscard]] std::vector<std::string> GetRegisteredExtensions() const;

      private:
        MeshImporterRegistry() = default;

        void EnsureBuiltinsRegistered() const;

        // mutable so the const query methods can trigger lazy built-in registration.
        mutable std::unordered_map<std::string, Scope<MeshImporter>> m_Importers;
        mutable Scope<MeshImporter> m_Fallback;
    };
} // namespace OloEngine
