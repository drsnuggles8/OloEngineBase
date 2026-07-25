#pragma once

#include "OloEngine/Asset/Interchange/MeshImporter.h"

namespace OloEngine
{
    // The first MeshImporter implementation and the registry's fallback: a thin wrapper over
    // the existing Renderer/Model Assimp path. Import() is exactly what
    // MeshSourceSerializer::TryLoadData used to do inline (Model + CreateCombinedMeshSource),
    // so routing FBX/glTF/OBJ/DAE/VRM/PLY through the interchange seam is behaviour-identical,
    // including the .omesh geometry cache Model reads on the warm path.
    class AssimpMeshImporter final : public MeshImporter
    {
      public:
        [[nodiscard]] MeshImportResult Import(const std::filesystem::path& path,
                                              const MeshImportOptions& options = {}) override;

        [[nodiscard]] std::string_view GetName() const override
        {
            return "Assimp";
        }
    };
} // namespace OloEngine
