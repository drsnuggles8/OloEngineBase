#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/AssimpMeshImporter.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Model.h"

#include <exception>
#include <filesystem>
#include <system_error>

namespace OloEngine
{
    MeshImportResult AssimpMeshImporter::Import(const std::filesystem::path& path, const MeshImportOptions& options)
    {
        // Non-throwing existence check — the throwing overload can throw on a filesystem
        // error (permissions, etc.), which would breach Import's no-throw boundary.
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return MeshImportResult::Failure("AssimpMeshImporter: file does not exist: " + path.string());

        // Preserve the exact old behaviour of MeshSourceSerializer::TryLoadData: build a Model
        // (which reads the .omesh geometry cache when warm and only re-imports the source for
        // materials) and combine its meshes into a single MeshSource. CreateCombinedMeshSource
        // deliberately does NOT Build() — the caller does — and marks the result pre-optimized.
        // Model construction / assimp import can throw (bad file, allocation); convert any
        // exception into a Failure result so Import stays no-throw for its callers.
        try
        {
            // FlipUV is passed straight through (no per-format origin XOR like the USD/Alembic
            // importers): the assimp import path already normalizes the UV origin, so the flag
            // here carries the "invert relative to the format's default" meaning directly. See
            // the MeshImportOptions::FlipUV note in MeshImporter.h.
            Model model(path.string(), TextureOverride{}, options.FlipUV);
            auto meshSource = model.CreateCombinedMeshSource();
            if (!meshSource || meshSource->GetVertices().IsEmpty())
                return MeshImportResult::Failure("AssimpMeshImporter: import produced no geometry: " + path.string());

            return MeshImportResult::Ok(std::move(meshSource));
        }
        catch (const std::exception& e)
        {
            return MeshImportResult::Failure(std::string("AssimpMeshImporter: import failed: ") + e.what());
        }
    }
} // namespace OloEngine
