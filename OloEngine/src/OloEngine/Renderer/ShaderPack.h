#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Shader;
    class ShaderLibrary;

    // Binary file format version for .osp (OloEngine Shader Pack) files.
    //
    // Version 2 (issue #908) adds a per-entry content hash so a pack entry can
    // be VALIDATED against the shader source it is meant to serve rather than
    // trusted on name alone — a v1 pack (name-keyed, no hash) is a strict
    // subset of what a v2 reader expects, so the version check below rejects
    // it outright instead of half-reading a shorter index.
    constexpr u32 SHADER_PACK_VERSION = 2;

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

        // The content hash this pack stored for `name` at BAKE time (issue
        // #908) — nullopt if the shader isn't in the pack at all. Compare
        // against OpenGLShader::ComputeContentHash(name) computed from the
        // CURRENT on-disk source before calling LoadEntry: a mismatch means
        // the pack is stale for this entry and must be treated as a miss, not
        // served. Deliberately not done inside LoadEntry itself — computing
        // the current hash means reading and preprocessing the live shader
        // file, which is the caller's (ShaderLibrary's) job, not the pack's;
        // the pack only reports what it baked.
        [[nodiscard]] std::optional<std::string> GetContentHash(const std::string& name) const;

        // Load a single shader's SPIR-V data from the pack (lazy read from disk).
        // Returns nullptr if the shader is not in the pack.
        [[nodiscard]] std::unique_ptr<ShaderPackEntry> LoadEntry(const std::string& name) const;

        // Build a shader pack from all compiled shaders in the given libraries.
        // Both Renderer2D and Renderer3D shader libraries are included. Requires
        // every shader to already be GL-linked (IsReady()) — this is the
        // editor's "Build Shader Pack" menu command, run inside a live context.
        static bool CreateFromLibraries(ShaderLibrary& lib2D, ShaderLibrary& lib3D, const std::filesystem::path& outputPath);

        // Build a shader pack directly from a list of shader filepaths, using
        // ONLY the CPU-side prepare path (Shader::PrepareBatch: read, preprocess,
        // shaderc, SPIRV-Cross) — no GL context is created, touched, or required
        // (issue #908's invalidation-contract spike: verified PrepareBatch never
        // calls into GL). This is the headless CI producer; `filepaths` must be
        // the SAME strings the runtime will later call ShaderLibrary::Load()
        // with (e.g. "assets/shaders/Water.glsl"), since that string is the
        // pack's lookup key. A filepath whose CPU prepare fails is skipped with
        // a warning rather than failing the whole bake — one broken shader
        // should not withhold the pack from every other shader that compiled
        // fine (the runtime falls back to compiling that one shader from
        // source, same as any other pack miss).
        static bool CreateFromFilepaths(const std::vector<std::string>& filepaths, const std::filesystem::path& outputPath);

        // Recursively collect every `.glsl` file under `shadersRoot`, skipping
        // `include/` (headers with no `#type` marker — PreProcess would reject
        // them) and `tests/` (test-only content, never shipped). Returns paths
        // relative to the CURRENT directory in the same "assets/shaders/…" form
        // ShaderLibrary::Load() is called with, sorted for a reproducible bake.
        [[nodiscard]] static std::vector<std::string> CollectShaderFilepaths(const std::filesystem::path& shadersRoot);

      private:
        // On-disk index entry (where to find each shader's data in the file)
        struct IndexEntry
        {
            std::string Name;
            std::string ContentHash; // issue #908 — see GetContentHash()
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

        // One shader's data, in the shape the on-disk writer needs — shared by
        // CreateFromLibraries (already-linked shaders) and CreateFromFilepaths
        // (CPU-prepared-only shaders), so the binary-layout logic itself is
        // written and tested exactly once.
        struct PackShaderInfo
        {
            std::string Name;
            std::string ContentHash;
            const std::unordered_map<unsigned int, std::vector<u32>>* VulkanSPIRV = nullptr;
            const std::unordered_map<unsigned int, std::vector<u32>>* OpenGLSPIRV = nullptr;
        };
        static bool WritePackFile(const std::vector<PackShaderInfo>& shaders, const std::filesystem::path& outputPath);

        bool m_Loaded = false;
        std::filesystem::path m_Path;
        std::unordered_map<std::string, IndexEntry> m_Index;
    };
} // namespace OloEngine
