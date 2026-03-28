#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Shader;
    class ShaderLibrary;

    // Binary file format version for .osp (OloEngine Shader Pack) files
    constexpr u32 SHADER_PACK_VERSION = 1;

    // Per-stage SPIR-V data stored in a shader pack
    struct ShaderPackStageData
    {
        u8 Stage = 0; // GL_VERTEX_SHADER mapped to 1=Vert, 2=Frag, 3=TessCtrl, 4=TessEval, 5=Compute
        std::vector<u32> VulkanSPIRV;
        std::vector<u32> OpenGLSPIRV;
    };

    // All data for a single shader program in the pack
    struct ShaderPackEntry
    {
        std::string Name;
        std::vector<ShaderPackStageData> Stages;
    };

    class ShaderPack
    {
      public:
        ShaderPack() = default;

        // Load a shader pack from disk
        explicit ShaderPack(const std::filesystem::path& path);

        // Query
        [[nodiscard]] bool IsLoaded() const
        {
            return m_Loaded;
        }
        [[nodiscard]] bool Contains(const std::string& name) const;
        [[nodiscard]] const std::filesystem::path& GetPath() const
        {
            return m_Path;
        }
        [[nodiscard]] u32 GetShaderCount() const
        {
            return static_cast<u32>(m_Index.size());
        }
        [[nodiscard]] std::vector<std::string> GetShaderNames() const;

        // Load a single shader's SPIR-V data from the pack (lazy read from disk).
        // Returns nullptr if the shader is not in the pack.
        [[nodiscard]] std::unique_ptr<ShaderPackEntry> LoadEntry(const std::string& name) const;

        // Build a shader pack from all compiled shaders in the given libraries.
        // Both Renderer2D and Renderer3D shader libraries are included.
        static bool CreateFromLibraries(ShaderLibrary& lib2D, ShaderLibrary& lib3D, const std::filesystem::path& outputPath);

      private:
        // On-disk index entry (where to find each shader's data in the file)
        struct IndexEntry
        {
            std::string Name;
            u32 StageCount = 0;

            struct StageRef
            {
                u8 Stage = 0;
                u64 VulkanOffset = 0;
                u64 VulkanSizeWords = 0;
                u64 OpenGLOffset = 0;
                u64 OpenGLSizeWords = 0;
            };

            std::vector<StageRef> StageRefs;
        };

        bool m_Loaded = false;
        std::filesystem::path m_Path;
        std::unordered_map<std::string, IndexEntry> m_Index;
    };
} // namespace OloEngine
