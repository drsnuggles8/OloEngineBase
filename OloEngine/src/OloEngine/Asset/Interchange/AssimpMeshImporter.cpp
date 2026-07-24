#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/AssimpMeshImporter.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Model.h"

#include <filesystem>

namespace OloEngine
{
    MeshImportResult AssimpMeshImporter::Import(const std::filesystem::path& path, const MeshImportOptions& options)
    {
        if (!std::filesystem::exists(path))
            return MeshImportResult::Failure("AssimpMeshImporter: file does not exist: " + path.string());

        // Preserve the exact old behaviour of MeshSourceSerializer::TryLoadData: build a Model
        // (which reads the .omesh geometry cache when warm and only re-imports the source for
        // materials) and combine its meshes into a single MeshSource. CreateCombinedMeshSource
        // deliberately does NOT Build() — the caller does — and marks the result pre-optimized.
        Model model(path.string(), TextureOverride{}, options.FlipUV);
        auto meshSource = model.CreateCombinedMeshSource();
        if (!meshSource || meshSource->GetVertices().IsEmpty())
            return MeshImportResult::Failure("AssimpMeshImporter: import produced no geometry: " + path.string());

        return MeshImportResult::Ok(std::move(meshSource));
    }
} // namespace OloEngine
