#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <glad/gl.h>
#include <fstream>

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
                case 0x8B31: return 1; // GL_VERTEX_SHADER
                case 0x8B30: return 2; // GL_FRAGMENT_SHADER
                case 0x8E88: return 3; // GL_TESS_CONTROL_SHADER
                case 0x8E87: return 4; // GL_TESS_EVALUATION_SHADER
                case 0x91B9: return 5; // GL_COMPUTE_SHADER
                default: return 0;
            }
        }

        constexpr unsigned int U8ToStage(u8 packed)
        {
            switch (packed)
            {
                case 1: return 0x8B31; // GL_VERTEX_SHADER
                case 2: return 0x8B30; // GL_FRAGMENT_SHADER
                case 3: return 0x8E88; // GL_TESS_CONTROL_SHADER
                case 4: return 0x8E87; // GL_TESS_EVALUATION_SHADER
                case 5: return 0x91B9; // GL_COMPUTE_SHADER
                default: return 0;
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
            char Magic[4] = {'O', 'L', 'S', 'P'};
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

        void WriteU32Array(std::ofstream& out, const std::vector<u32>& data)
        {
            u64 count = data.size();
            WriteRaw(out, count);
            if (count > 0)
            {
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(count * sizeof(u32)));
            }
        }

        bool ReadU32Array(std::ifstream& in, std::vector<u32>& data)
        {
            u64 count = 0;
            if (!ReadRaw(in, count))
            {
                return false;
            }
            // Sanity check: reject absurdly large arrays
            constexpr u64 maxWords = 64 * 1024 * 1024; // 256 MB of SPIR-V
            if (count > maxWords)
            {
                return false;
            }
            data.resize(static_cast<size_t>(count));
            if (count > 0)
            {
                in.read(reinterpret_cast<char*>(data.data()),
                        static_cast<std::streamsize>(count * sizeof(u32)));
            }
            return in.good();
        }

        void WriteString(std::ofstream& out, const std::string& str)
        {
            u32 len = static_cast<u32>(str.size());
            WriteRaw(out, len);
            if (len > 0)
            {
                out.write(str.data(), len);
            }
        }

        bool ReadString(std::ifstream& in, std::string& str)
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
            str.resize(len);
            if (len > 0)
            {
                in.read(str.data(), len);
            }
            return in.good();
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

            if (!ReadRaw(in, entry.StageCount))
            {
                OLO_CORE_ERROR("[ShaderPack] Failed to read stage count for '{}'", entry.Name);
                return;
            }

            entry.StageRefs.resize(entry.StageCount);
            for (u32 s = 0; s < entry.StageCount; ++s)
            {
                auto& ref = entry.StageRefs[s];
                if (!ReadRaw(in, ref.Stage) ||
                    !ReadRaw(in, ref.VulkanOffset) ||
                    !ReadRaw(in, ref.VulkanSizeWords) ||
                    !ReadRaw(in, ref.OpenGLOffset) ||
                    !ReadRaw(in, ref.OpenGLSizeWords))
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read stage ref for '{}' stage {}", entry.Name, s);
                    return;
                }
            }

            m_Index[entry.Name] = std::move(entry);
        }

        m_Loaded = true;
        OLO_CORE_INFO("[ShaderPack] Loaded '{}' with {} shaders", path.string(), header.ShaderCount);
    }

    bool ShaderPack::Contains(const std::string& name) const
    {
        return m_Index.contains(name);
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
        entry->Stages.reserve(it->second.StageCount);

        for (const auto& ref : it->second.StageRefs)
        {
            ShaderPackStageData stage;
            stage.Stage = ref.Stage;

            // Read Vulkan SPIR-V
            in.seekg(static_cast<std::streamoff>(ref.VulkanOffset));
            stage.VulkanSPIRV.resize(static_cast<size_t>(ref.VulkanSizeWords));
            if (ref.VulkanSizeWords > 0)
            {
                in.read(reinterpret_cast<char*>(stage.VulkanSPIRV.data()),
                        static_cast<std::streamsize>(ref.VulkanSizeWords * sizeof(u32)));
                if (!in.good())
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read Vulkan SPIR-V for '{}'", name);
                    return nullptr;
                }
            }

            // Read OpenGL SPIR-V
            in.seekg(static_cast<std::streamoff>(ref.OpenGLOffset));
            stage.OpenGLSPIRV.resize(static_cast<size_t>(ref.OpenGLSizeWords));
            if (ref.OpenGLSizeWords > 0)
            {
                in.read(reinterpret_cast<char*>(stage.OpenGLSPIRV.data()),
                        static_cast<std::streamsize>(ref.OpenGLSizeWords * sizeof(u32)));
                if (!in.good())
                {
                    OLO_CORE_ERROR("[ShaderPack] Failed to read OpenGL SPIR-V for '{}'", name);
                    return nullptr;
                }
            }

            entry->Stages.push_back(std::move(stage));
        }

        return entry;
    }

    // =========================================================================
    // ShaderPack — Create from compiled shader libraries
    // =========================================================================
    bool ShaderPack::CreateFromLibraries(ShaderLibrary& lib2D, ShaderLibrary& lib3D, const std::filesystem::path& outputPath)
    {
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

        // Collect all shaders from both libraries
        struct ShaderInfo
        {
            std::string Name;
            const std::unordered_map<unsigned int, std::vector<u32>>* VulkanSPIRV = nullptr;
            const std::unordered_map<unsigned int, std::vector<u32>>* OpenGLSPIRV = nullptr;
        };

        std::vector<ShaderInfo> shaders;

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

                auto* glShader = static_cast<OpenGLShader*>(shader.get());
                shaders.push_back({
                    shader->GetFilePath(),
                    &glShader->GetVulkanSPIRV(),
                    &glShader->GetOpenGLSPIRV()
                });
            }
        };

        collectShaders(lib2D);
        collectShaders(lib3D);

        if (shaders.empty())
        {
            OLO_CORE_WARN("[ShaderPack] No shaders to pack");
            return false;
        }

        // Write header
        FileHeader header{};
        header.ShaderCount = static_cast<u32>(shaders.size());
        WriteRaw(out, header);

        // Phase 1: Write placeholder index (compute sizes but use dummy offsets)
        // We'll backfill the data offsets after writing all SPIR-V data.
        const auto indexStartPos = out.tellp();

        // Compute index size so we can calculate where data starts
        // For each shader: u32(nameLen) + name + u32(stageCount) + per-stage(u8 + 4*u64)
        u64 indexSize = 0;
        for (const auto& info : shaders)
        {
            indexSize += sizeof(u32) + info.Name.size(); // name
            indexSize += sizeof(u32);                     // stageCount
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

        struct ShaderOffsets
        {
            std::vector<StageOffset> Stages;
        };

        std::vector<ShaderOffsets> allOffsets;
        allOffsets.reserve(shaders.size());

        for (const auto& info : shaders)
        {
            ShaderOffsets offsets;

            for (const auto& [glStage, vulkanData] : *info.VulkanSPIRV)
            {
                StageOffset so;
                so.Stage = StageToU8(glStage);

                // Write Vulkan SPIR-V
                so.VulkanOffset = static_cast<u64>(out.tellp());
                so.VulkanSizeWords = vulkanData.size();
                if (!vulkanData.empty())
                {
                    out.write(reinterpret_cast<const char*>(vulkanData.data()),
                              static_cast<std::streamsize>(vulkanData.size() * sizeof(u32)));
                }

                // Write OpenGL SPIR-V for same stage
                auto openGLIt = info.OpenGLSPIRV->find(glStage);
                so.OpenGLOffset = static_cast<u64>(out.tellp());
                if (openGLIt != info.OpenGLSPIRV->end() && !openGLIt->second.empty())
                {
                    so.OpenGLSizeWords = openGLIt->second.size();
                    out.write(reinterpret_cast<const char*>(openGLIt->second.data()),
                              static_cast<std::streamsize>(openGLIt->second.size() * sizeof(u32)));
                }
                else
                {
                    so.OpenGLSizeWords = 0;
                }

                offsets.Stages.push_back(so);
            }

            allOffsets.push_back(std::move(offsets));
        }

        // Phase 3: Backfill index with actual offsets
        out.seekp(indexStartPos);

        for (size_t i = 0; i < shaders.size(); ++i)
        {
            const auto& info = shaders[i];
            const auto& offsets = allOffsets[i];

            WriteString(out, info.Name);
            u32 stageCount = static_cast<u32>(offsets.Stages.size());
            WriteRaw(out, stageCount);

            for (const auto& so : offsets.Stages)
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
                       outputPath.string(), shaders.size(),
                       static_cast<f64>(fileSize) / 1024.0);

        return true;
    }
} // namespace OloEngine
