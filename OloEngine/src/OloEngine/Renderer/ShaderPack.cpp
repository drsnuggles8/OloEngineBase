#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace OloEngine
{
    // =========================================================================
    // Stage encoding (GLenum ↔ u8)
    // =========================================================================
    namespace
    {
        constexpr u8 StageToU8(unsigned int glStage)
        {
            switch (glStage)
            {
                case 0x8B31:
                    return 1; // GL_VERTEX_SHADER
                case 0x8B30:
                    return 2; // GL_FRAGMENT_SHADER
                case 0x8E88:
                    return 3; // GL_TESS_CONTROL_SHADER
                case 0x8E87:
                    return 4; // GL_TESS_EVALUATION_SHADER
                case 0x91B9:
                    return 5; // GL_COMPUTE_SHADER
                default:
                    return 0;
            }
        }

    } // namespace

    // =========================================================================
    // File format helpers
    // =========================================================================
    namespace
    {
        struct FileHeader
        {
            char Magic[4] = { 'O', 'L', 'S', 'P' };
            u32 Version = SHADER_PACK_VERSION;
            u32 ShaderCount = 0;
            // Padding to 16 bytes for alignment
            u32 Reserved = 0;
        };
        static_assert(sizeof(FileHeader) == 16);

        template<typename T>
        void WriteRaw(std::ofstream& out, const T& value)
        {
            out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template<typename T>
        bool ReadRaw(std::ifstream& in, T& value)
        {
            in.read(reinterpret_cast<char*>(&value), sizeof(T));
            return in.good();
        }

        void WriteString(std::ofstream& out, std::string_view str)
        {
            u32 len = static_cast<u32>(str.size());
            WriteRaw(out, len);
            if (len > 0)
            {
                out.write(str.data(), len);
            }
        }

        bool ReadString(std::ifstream& in, FString& str)
        {
            u32 len = 0;
            if (!ReadRaw(in, len))
            {
                return false;
            }
            // Sanity check
            if (len > 4096)
            {
                return false;
            }
            std::string bytes(len, '\0');
            if (len > 0)
            {
                in.read(bytes.data(), len);
            }
            if (!in.good())
            {
                return false;
            }
            str = FString(bytes);
            return true;
        }
    } // namespace

    // =========================================================================
    // ShaderPack — Load from disk
    // =========================================================================
    ShaderPack::ShaderPack(const std::filesystem::path& path)
        : m_Path(path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            OLO_CORE_ERROR("[ShaderPack] Failed to open: {}", path.string());
            return;
        }

        // Read and validate header
        FileHeader header{};
        if (!ReadRaw(in, header))
        {
            OLO_CORE_ERROR("[ShaderPack] Failed to read header: {}", path.string());
            return;
        }

        if (std::memcmp(header.Magic, "OLSP", 4) != 0)
        {
            OLO_CORE_ERROR("[ShaderPack] Invalid magic in: {}", path.string());
            return;
        }

        if (header.Version != SHADER_PACK_VERSION)
        {
            OLO_CORE_ERROR("[ShaderPack] Version mismatch (file={}, expected={}) in: {}",
                           header.Version, SHADER_PACK_VERSION, path.string());
            return;
        }

        // Read shader index
        for (u32 i = 0; i < header.ShaderCount; ++i)
        {
            IndexEntry entry;
            if (!ReadString(in, entry.Name))
            {
                OLO_CORE_ERROR("[ShaderPack] Failed to read shader name at index {}", i);
                return;
            }

            if (!ReadString(in, entry.ContentHash))
            {
                OLO_CORE_ERROR("[ShaderPack] Failed to read content hash for '{}'", entry.Name.ToView());
                return;
            }

            if (!ReadRaw(in, entry.StageCount))
            {
                OLO_CORE_ERROR("[ShaderPack] Failed to read stage count for '{}'", entry.Name.ToView());
                return;
            }

            if (entry.StageCount > static_cast<u32>(std::numeric_limits<i32>::max()))
            {
                OLO_CORE_ERROR("[ShaderPack] Stage count exceeds array capacity for '{}'", entry.Name.ToView());
                return;
            }
            entry.StageRefs.SetNum(static_cast<i32>(entry.StageCount));
            for (u32 s = 0; s < entry.StageCount; ++s)
            {
                auto& ref = entry.StageRefs[s];
                if (!ReadRaw(in, ref.Stage) ||
                    !ReadRaw(in, ref.VulkanOffset) ||
                    !ReadRaw(in, ref.VulkanSizeWords) ||
                    !ReadRaw(in, ref.OpenGLOffset) ||
                    !ReadRaw(in, ref.OpenGLSizeWords))
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read stage ref for '{}' stage {}", entry.Name.ToView(), s);
                    return;
                }
            }

            m_Index[entry.Name.ToStdString()] = std::move(entry);
        }

        m_Loaded = true;
        OLO_CORE_INFO("[ShaderPack] Loaded '{}' with {} shaders", path.string(), header.ShaderCount);
    }

    bool ShaderPack::Contains(const std::string& name) const
    {
        return m_Index.contains(name);
    }

    std::optional<std::string> ShaderPack::GetContentHash(const std::string& name) const
    {
        auto it = m_Index.find(name);
        if (it == m_Index.end())
        {
            return std::nullopt;
        }
        return it->second.ContentHash.ToStdString();
    }

    std::vector<std::string> ShaderPack::GetShaderNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Index.size());
        for (const auto& [name, entry] : m_Index)
        {
            names.push_back(name);
        }
        return names;
    }

    std::unique_ptr<ShaderPackEntry> ShaderPack::LoadEntry(const std::string& name) const
    {
        auto it = m_Index.find(name);
        if (it == m_Index.end())
        {
            return nullptr;
        }

        std::ifstream in(m_Path, std::ios::binary);
        if (!in)
        {
            OLO_CORE_ERROR("[ShaderPack] Failed to reopen '{}' for lazy load", m_Path.string());
            return nullptr;
        }

        auto entry = std::make_unique<ShaderPackEntry>();
        entry->Name = it->second.Name;
        entry->Stages.Reserve(static_cast<i32>(it->second.StageCount));

        for (const auto& ref : it->second.StageRefs)
        {
            if (ref.VulkanSizeWords > static_cast<u64>(std::numeric_limits<i32>::max()) ||
                ref.OpenGLSizeWords > static_cast<u64>(std::numeric_limits<i32>::max()))
            {
                OLO_CORE_ERROR("[ShaderPack] SPIR-V word count exceeds array capacity for '{}'", name);
                return nullptr;
            }
            ShaderPackStageData stage;
            stage.Stage = ref.Stage;

            // Read Vulkan SPIR-V
            in.seekg(static_cast<std::streamoff>(ref.VulkanOffset));
            stage.VulkanSPIRV.SetNum(static_cast<i32>(ref.VulkanSizeWords));
            if (ref.VulkanSizeWords > 0)
            {
                in.read(reinterpret_cast<char*>(stage.VulkanSPIRV.GetData()),
                        static_cast<std::streamsize>(ref.VulkanSizeWords * sizeof(u32)));
                if (!in.good())
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read Vulkan SPIR-V for '{}'", name);
                    return nullptr;
                }
            }

            // Read OpenGL SPIR-V
            in.seekg(static_cast<std::streamoff>(ref.OpenGLOffset));
            stage.OpenGLSPIRV.SetNum(static_cast<i32>(ref.OpenGLSizeWords));
            if (ref.OpenGLSizeWords > 0)
            {
                in.read(reinterpret_cast<char*>(stage.OpenGLSPIRV.GetData()),
                        static_cast<std::streamsize>(ref.OpenGLSizeWords * sizeof(u32)));
                if (!in.good())
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read OpenGL SPIR-V for '{}'", name);
                    return nullptr;
                }
            }

            entry->Stages.Add(std::move(stage));
        }

        return entry;
    }

    // =========================================================================
    // ShaderPack — Create from compiled shader libraries
    // =========================================================================
    bool ShaderPack::CreateFromLibraries(ShaderLibrary& lib2D, ShaderLibrary& lib3D, const std::filesystem::path& outputPath)
    {
        TArray<PackShaderInfo> shaders;

        auto collectShaders = [&shaders](ShaderLibrary& lib)
        {
            for (const auto& name : lib.GetAllShaderNames())
            {
                auto shader = lib.Get(name);
                if (!shader || !shader->IsReady())
                {
                    OLO_CORE_WARN("[ShaderPack] Skipping shader '{}' (not ready)", name);
                    continue;
                }

                const auto* glShader = static_cast<OpenGLShader*>(shader.get());

                // Hashed from the shader's OWN preprocessed source
                // (GetOriginalSourceCode) — the exact text that produced the
                // SPIR-V being packed below — rather than re-reading the file
                // from disk. A fresh disk re-read here would be a TOCTOU
                // window: if the file changed after this shader was compiled
                // but before "Build Shader Pack" ran, a re-read would hash
                // the NEW text while packing the OLD (in-memory) SPIR-V, so a
                // later runtime hash check against the (by-then also new)
                // on-disk file would falsely validate stale bytes.
                const std::string contentHash =
                    OpenGLShader::ComputeContentHashFromSources(glShader->GetOriginalSourceCode());
                if (contentHash.empty())
                {
                    OLO_CORE_WARN("[ShaderPack] Skipping shader '{}' (couldn't recompute its content hash)",
                                  shader->GetFilePath());
                    continue;
                }

                shaders.Add(PackShaderInfo{ shader->GetFilePath(), contentHash,
                                            &glShader->GetVulkanSPIRV(), &glShader->GetOpenGLSPIRV() });
            }
        };

        collectShaders(lib2D);
        collectShaders(lib3D);

        return WritePackFile(shaders, outputPath);
    }

    // =========================================================================
    // ShaderPack — Create from filepaths (headless, no GL context — issue #908)
    // =========================================================================
    bool ShaderPack::CreateFromFilepaths(const std::vector<std::string>& filepaths, const std::filesystem::path& outputPath)
    {
        if (filepaths.empty())
        {
            OLO_CORE_WARN("[ShaderPack] No shader filepaths to pack");
            return false;
        }

        // CPU-only: read, preprocess, shaderc, SPIRV-Cross. No GL call anywhere
        // in this call chain — see Shader::PrepareBatch / OpenGLShader::PrepareCPU.
        TArray<FString> ownedPaths;
        ownedPaths.Reserve(static_cast<i32>(filepaths.size()));
        for (const auto& path : filepaths)
        {
            ownedPaths.Emplace(path);
        }
        TArray<Ref<Shader>> prepared = Shader::PrepareBatch(std::span{ ownedPaths.GetData(), static_cast<sizet>(ownedPaths.Num()) }, nullptr);

        TArray<PackShaderInfo> shaders;
        shaders.Reserve(static_cast<i32>(filepaths.size()));

        for (sizet i = 0; i < filepaths.size(); ++i)
        {
            Ref<Shader> shader = (i < static_cast<sizet>(prepared.Num())) ? prepared[i] : nullptr;
            if (!shader)
            {
                OLO_CORE_WARN("[ShaderPack] Skipping '{}' (CPU prepare failed)", filepaths[i]);
                continue;
            }

            auto* glShader = static_cast<OpenGLShader*>(shader.get());
            if (glShader->GetVulkanSPIRV().empty())
            {
                OLO_CORE_WARN("[ShaderPack] Skipping '{}' (no compiled SPIR-V)", filepaths[i]);
                continue;
            }

            // Hashed from the shader's OWN preprocessed source, not a second
            // re-read of the file — same reasoning as CreateFromLibraries
            // above: a re-read here would be a second, independent disk read
            // of the same file Shader::PrepareBatch just read to produce
            // this SPIR-V, so a file that changed between the two reads
            // would pack SPIR-V from read #1 under a hash from read #2.
            const std::string contentHash =
                OpenGLShader::ComputeContentHashFromSources(glShader->GetOriginalSourceCode());
            if (contentHash.empty())
            {
                OLO_CORE_WARN("[ShaderPack] Skipping '{}' (couldn't recompute its content hash)", filepaths[i]);
                continue;
            }

            shaders.Add(PackShaderInfo{ filepaths[i], contentHash, &glShader->GetVulkanSPIRV(), &glShader->GetOpenGLSPIRV() });
        }

        return WritePackFile(shaders, outputPath);
    }

    std::vector<std::string> ShaderPack::CollectShaderFilepaths(const std::filesystem::path& shadersRoot)
    {
        std::vector<std::string> filepaths;

        if (!std::filesystem::exists(shadersRoot))
        {
            OLO_CORE_WARN("[ShaderPack] Shaders root does not exist: {}", shadersRoot.string());
            return filepaths;
        }

        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(shadersRoot, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".glsl")
            {
                continue;
            }

            // "include" (headers, no #type marker) and "tests" (never shipped)
            // are excluded by directory component, not by full-path substring —
            // a shader legitimately named e.g. "assets/shaders/tests_utils.glsl"
            // must not be swept out by a naive `.find("tests")`.
            const bool excluded = std::ranges::any_of(entry.path(),
                                                      [](const std::filesystem::path& component)
                                                      {
                                                          return component == "include" || component == "tests";
                                                      });
            if (excluded)
            {
                continue;
            }

            filepaths.push_back(entry.path().generic_string());
        }

        std::ranges::sort(filepaths);
        return filepaths;
    }

    // =========================================================================
    // ShaderPack — shared binary writer
    // =========================================================================
    bool ShaderPack::WritePackFile(const TArray<PackShaderInfo>& shaders, const std::filesystem::path& outputPath)
    {
        if (shaders.IsEmpty())
        {
            OLO_CORE_WARN("[ShaderPack] No shaders to pack");
            return false;
        }

        // Ensure parent directory exists
        if (auto parentDir = outputPath.parent_path(); !parentDir.empty())
        {
            std::filesystem::create_directories(parentDir);
        }

        std::ofstream out(outputPath, std::ios::binary);
        if (!out)
        {
            OLO_CORE_ERROR("[ShaderPack] Failed to create output file: {}", outputPath.string());
            return false;
        }

        // Write header
        FileHeader header{};
        header.ShaderCount = static_cast<u32>(shaders.Num());
        WriteRaw(out, header);

        // Phase 1: Write placeholder index (compute sizes but use dummy offsets)
        // We'll backfill the data offsets after writing all SPIR-V data.
        const auto indexStartPos = out.tellp();

        // Compute index size so we can calculate where data starts. For each
        // shader: u32(nameLen) + name + u32(hashLen) + hash + u32(stageCount)
        // + per-stage(u8 + 4*u64)
        u64 indexSize = 0;
        for (const auto& info : shaders)
        {
            indexSize += sizeof(u32) + info.Name.Len();        // name
            indexSize += sizeof(u32) + info.ContentHash.Len(); // content hash
            indexSize += sizeof(u32);                          // stageCount
            u32 stageCount = static_cast<u32>(info.VulkanSPIRV->size());
            indexSize += stageCount * (sizeof(u8) + 4 * sizeof(u64)); // per-stage refs
        }

        // Skip past index — we'll come back to write it
        out.seekp(static_cast<std::streamoff>(indexStartPos) + static_cast<std::streamoff>(indexSize));

        // Phase 2: Write SPIR-V data and record offsets
        struct StageOffset
        {
            u8 Stage = 0;
            u64 VulkanOffset = 0;
            u64 VulkanSizeWords = 0;
            u64 OpenGLOffset = 0;
            u64 OpenGLSizeWords = 0;
        };

        TArray<TArray<StageOffset>> allOffsets;
        allOffsets.Reserve(shaders.Num());

        for (const auto& info : shaders)
        {
            TArray<StageOffset> offsets;

            for (const auto& [glStage, vulkanData] : *info.VulkanSPIRV)
            {
                StageOffset so;
                so.Stage = StageToU8(glStage);

                // Write Vulkan SPIR-V
                so.VulkanOffset = static_cast<u64>(out.tellp());
                so.VulkanSizeWords = vulkanData.Num();
                if (!vulkanData.IsEmpty())
                {
                    out.write(reinterpret_cast<const char*>(vulkanData.GetData()),
                              static_cast<std::streamsize>(vulkanData.Num() * sizeof(u32)));
                }

                // Write OpenGL SPIR-V for same stage
                auto openGLIt = info.OpenGLSPIRV->find(glStage);
                so.OpenGLOffset = static_cast<u64>(out.tellp());
                if (openGLIt != info.OpenGLSPIRV->end() && !openGLIt->second.IsEmpty())
                {
                    so.OpenGLSizeWords = openGLIt->second.Num();
                    out.write(reinterpret_cast<const char*>(openGLIt->second.GetData()),
                              static_cast<std::streamsize>(openGLIt->second.Num() * sizeof(u32)));
                }
                else
                {
                    so.OpenGLSizeWords = 0;
                }

                offsets.Add(so);
            }

            allOffsets.Add(std::move(offsets));
        }

        // Phase 3: Backfill index with actual offsets
        out.seekp(indexStartPos);

        for (i32 i = 0; i < shaders.Num(); ++i)
        {
            const auto& info = shaders[i];
            const auto& offsets = allOffsets[i];

            WriteString(out, info.Name.ToView());
            WriteString(out, info.ContentHash.ToView());
            u32 stageCount = static_cast<u32>(offsets.Num());
            WriteRaw(out, stageCount);

            for (const auto& so : offsets)
            {
                WriteRaw(out, so.Stage);
                WriteRaw(out, so.VulkanOffset);
                WriteRaw(out, so.VulkanSizeWords);
                WriteRaw(out, so.OpenGLOffset);
                WriteRaw(out, so.OpenGLSizeWords);
            }
        }

        out.close();

        const auto fileSize = std::filesystem::file_size(outputPath);
        OLO_CORE_INFO("[ShaderPack] Created '{}' — {} shaders, {:.1f} KB",
                      outputPath.string(), shaders.Num(),
                      static_cast<f64>(fileSize) / 1024.0);

        return true;
    }
} // namespace OloEngine
