#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace OloEngine
{
    // Options threaded through a MeshImporter::Import call. Kept intentionally small —
    // most format-specific tuning belongs in the concrete importer. Add fields here only
    // when a knob is meaningful across formats (winding/scale/UV origin are candidates).
    struct MeshImportOptions
    {
        // Flip the V texture coordinate (some DCC exporters use a top-left UV origin).
        // Mirrors the pre-existing Model(path, override, flipUV) parameter so the Assimp
        // path stays behaviour-identical when routed through this interface.
        bool FlipUV = false;
    };

    // Outcome of a single import. On success, Source is a fully-populated MeshSource that
    // has NOT had Build() called on it (see the interface contract below); on failure,
    // Source is null and Error carries a human-readable reason for the log.
    struct MeshImportResult
    {
        Ref<MeshSource> Source;
        std::string Error;

        [[nodiscard]] bool Succeeded() const
        {
            return Source != nullptr;
        }

        static MeshImportResult Failure(std::string error)
        {
            MeshImportResult result;
            result.Error = std::move(error);
            return result;
        }

        static MeshImportResult Ok(Ref<MeshSource> source)
        {
            MeshImportResult result;
            result.Source = std::move(source);
            return result;
        }
    };

    // Abstract translator from a source geometry file (FBX / glTF / OBJ / USD / Alembic / …)
    // to the engine's unified MeshSource. This is the thin interchange seam issue #655 Tier 2
    // asks for: a MeshImporter fronting every parser so new formats plug in behind
    // MeshImporterRegistry without touching AssetSerializer/AssetImporter call sites.
    //
    // Contract every implementation MUST honour:
    //   * Produce vertices (position/normal/UV), indices, and one Submesh per material group,
    //     with Submesh::m_MaterialIndex indexing the returned MeshSource's imported-material
    //     array — exactly the shape Model::CreateCombinedMeshSource returns today.
    //   * Do NOT call MeshSource::Build(). GPU-buffer construction is the caller's job
    //     (MeshSourceSerializer builds; the asset-pack/headless paths deliberately do not),
    //     so an importer that Build()s would break headless serialization. Call
    //     SetPreOptimized(true) if the geometry is already meshopt-optimized (as the Assimp
    //     combined path does) so Build() skips re-optimization.
    //   * Be safe to call off the main thread (the async asset system loads on workers). Do
    //     no GL/GPU work; touch no shared mutable global state.
    class MeshImporter
    {
      public:
        virtual ~MeshImporter() = default;

        // Import the file at `path` into a MeshSource. Never throws across the boundary —
        // failures are reported via MeshImportResult::Error.
        [[nodiscard]] virtual MeshImportResult Import(const std::filesystem::path& path,
                                                      const MeshImportOptions& options = {}) = 0;

        // Short human-readable name used in logs (e.g. "Assimp", "OpenUSD", "Alembic").
        [[nodiscard]] virtual std::string_view GetName() const = 0;
    };
} // namespace OloEngine
