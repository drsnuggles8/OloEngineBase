#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderPack.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Helpers to create a dummy .osp file on disk for testing the reader
    struct FileHeader
    {
        char Magic[4] = { 'O', 'L', 'S', 'P' };
        u32 Version = SHADER_PACK_VERSION;
        u32 ShaderCount = 0;
        u32 Reserved = 0;
    };
    static_assert(sizeof(FileHeader) == 16);

    template<typename T>
    void WriteRaw(std::ofstream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
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

    // Create a minimal .osp with 1 shader (2 stages: vert + frag)
    std::filesystem::path CreateTestPack(const std::filesystem::path& dir, const std::string& shaderName)
    {
        std::filesystem::create_directories(dir);
        auto path = dir / "test.osp";

        // Fake SPIR-V data (just recognizable u32 patterns)
        std::vector<u32> fakeVulkanVert = { 0x07230203, 0x00010000, 1, 2, 3 }; // SPIR-V magic + dummy
        std::vector<u32> fakeOpenGLVert = { 0x07230203, 0x00010000, 10, 20, 30 };
        std::vector<u32> fakeVulkanFrag = { 0x07230203, 0x00010000, 4, 5, 6, 7 };
        std::vector<u32> fakeOpenGLFrag = { 0x07230203, 0x00010000, 40, 50, 60, 70 };

        std::ofstream out(path, std::ios::binary);

        // Header
        FileHeader header;
        header.ShaderCount = 1;
        WriteRaw(out, header);

        // We need to write the index first, then data, but we need data offsets.
        // Calculate index size: string(4 + name.size()) + stageCount(4) + 2 stages * (1 + 4*8)
        u64 indexSize = (sizeof(u32) + shaderName.size()) + sizeof(u32) + 2 * (sizeof(u8) + 4 * sizeof(u64));

        auto indexStart = out.tellp();
        // Write placeholder index
        std::vector<char> placeholder(static_cast<size_t>(indexSize), 0);
        out.write(placeholder.data(), static_cast<std::streamsize>(placeholder.size()));

        // Write Vulkan vert data
        u64 vulkanVertOffset = static_cast<u64>(out.tellp());
        u64 vulkanVertSize = fakeVulkanVert.size();
        out.write(reinterpret_cast<const char*>(fakeVulkanVert.data()),
                  static_cast<std::streamsize>(fakeVulkanVert.size() * sizeof(u32)));

        // Write OpenGL vert data
        u64 openGLVertOffset = static_cast<u64>(out.tellp());
        u64 openGLVertSize = fakeOpenGLVert.size();
        out.write(reinterpret_cast<const char*>(fakeOpenGLVert.data()),
                  static_cast<std::streamsize>(fakeOpenGLVert.size() * sizeof(u32)));

        // Write Vulkan frag data
        u64 vulkanFragOffset = static_cast<u64>(out.tellp());
        u64 vulkanFragSize = fakeVulkanFrag.size();
        out.write(reinterpret_cast<const char*>(fakeVulkanFrag.data()),
                  static_cast<std::streamsize>(fakeVulkanFrag.size() * sizeof(u32)));

        // Write OpenGL frag data
        u64 openGLFragOffset = static_cast<u64>(out.tellp());
        u64 openGLFragSize = fakeOpenGLFrag.size();
        out.write(reinterpret_cast<const char*>(fakeOpenGLFrag.data()),
                  static_cast<std::streamsize>(fakeOpenGLFrag.size() * sizeof(u32)));

        // Backfill index
        out.seekp(indexStart);
        WriteString(out, shaderName);
        u32 stageCount = 2;
        WriteRaw(out, stageCount);

        // Vert stage (u8=1)
        u8 vertStage = 1;
        WriteRaw(out, vertStage);
        WriteRaw(out, vulkanVertOffset);
        WriteRaw(out, vulkanVertSize);
        WriteRaw(out, openGLVertOffset);
        WriteRaw(out, openGLVertSize);

        // Frag stage (u8=2)
        u8 fragStage = 2;
        WriteRaw(out, fragStage);
        WriteRaw(out, vulkanFragOffset);
        WriteRaw(out, vulkanFragSize);
        WriteRaw(out, openGLFragOffset);
        WriteRaw(out, openGLFragSize);

        out.close();
        return path;
    }

    const std::string kTestShaderName = "assets/shaders/TestShader.glsl";

    class ShaderPackTest : public ::testing::Test
    {
      protected:
        std::filesystem::path m_TestDir;

        void SetUp() override
        {
            m_TestDir = std::filesystem::temp_directory_path() / "olo_shaderpack_test";
            std::filesystem::remove_all(m_TestDir);
            std::filesystem::create_directories(m_TestDir);
        }

        void TearDown() override
        {
            std::filesystem::remove_all(m_TestDir);
        }
    };
} // namespace

// =========================================================================
// Loading
// =========================================================================

TEST_F(ShaderPackTest, LoadValid)
{
    auto packPath = CreateTestPack(m_TestDir, kTestShaderName);
    ShaderPack pack(packPath);

    EXPECT_TRUE(pack.IsLoaded());
    EXPECT_EQ(pack.GetShaderCount(), 1u);
    EXPECT_EQ(pack.GetPath(), packPath);
}

TEST_F(ShaderPackTest, ContainsQuery)
{
    auto packPath = CreateTestPack(m_TestDir, kTestShaderName);
    ShaderPack pack(packPath);

    EXPECT_TRUE(pack.Contains(kTestShaderName));
    EXPECT_FALSE(pack.Contains("nonexistent/shader.glsl"));
}

TEST_F(ShaderPackTest, GetShaderNames)
{
    auto packPath = CreateTestPack(m_TestDir, kTestShaderName);
    ShaderPack pack(packPath);

    auto names = pack.GetShaderNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], kTestShaderName);
}

TEST_F(ShaderPackTest, LoadEntryValid)
{
    auto packPath = CreateTestPack(m_TestDir, kTestShaderName);
    ShaderPack pack(packPath);

    auto entry = pack.LoadEntry(kTestShaderName);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->Name, kTestShaderName);
    ASSERT_EQ(entry->Stages.size(), 2u);

    // Vert stage
    EXPECT_EQ(entry->Stages[0].Stage, 1u); // Vertex
    EXPECT_EQ(entry->Stages[0].VulkanSPIRV.size(), 5u);
    EXPECT_EQ(entry->Stages[0].VulkanSPIRV[0], 0x07230203u); // SPIR-V magic
    EXPECT_EQ(entry->Stages[0].VulkanSPIRV[2], 1u);
    EXPECT_EQ(entry->Stages[0].OpenGLSPIRV.size(), 5u);
    EXPECT_EQ(entry->Stages[0].OpenGLSPIRV[2], 10u);

    // Frag stage
    EXPECT_EQ(entry->Stages[1].Stage, 2u); // Fragment
    EXPECT_EQ(entry->Stages[1].VulkanSPIRV.size(), 6u);
    EXPECT_EQ(entry->Stages[1].VulkanSPIRV[2], 4u);
    EXPECT_EQ(entry->Stages[1].OpenGLSPIRV.size(), 6u);
    EXPECT_EQ(entry->Stages[1].OpenGLSPIRV[2], 40u);
}

TEST_F(ShaderPackTest, LoadEntryNotFound)
{
    auto packPath = CreateTestPack(m_TestDir, kTestShaderName);
    ShaderPack pack(packPath);

    auto entry = pack.LoadEntry("nonexistent.glsl");
    EXPECT_EQ(entry, nullptr);
}

// =========================================================================
// Error handling
// =========================================================================

TEST_F(ShaderPackTest, LoadNonexistentFile)
{
    ShaderPack pack(m_TestDir / "does_not_exist.osp");
    EXPECT_FALSE(pack.IsLoaded());
    EXPECT_EQ(pack.GetShaderCount(), 0u);
}

TEST_F(ShaderPackTest, LoadInvalidMagic)
{
    auto path = m_TestDir / "bad_magic.osp";
    {
        std::ofstream out(path, std::ios::binary);
        FileHeader header;
        header.Magic[0] = 'X'; // corrupt magic
        WriteRaw(out, header);
    }
    ShaderPack pack(path);
    EXPECT_FALSE(pack.IsLoaded());
}

TEST_F(ShaderPackTest, LoadWrongVersion)
{
    auto path = m_TestDir / "bad_version.osp";
    {
        std::ofstream out(path, std::ios::binary);
        FileHeader header;
        header.Version = 999;
        WriteRaw(out, header);
    }
    ShaderPack pack(path);
    EXPECT_FALSE(pack.IsLoaded());
}

TEST_F(ShaderPackTest, LoadTruncatedFile)
{
    auto path = m_TestDir / "truncated.osp";
    {
        std::ofstream out(path, std::ios::binary);
        FileHeader header;
        header.ShaderCount = 5; // Claims 5 shaders but file has no index
        WriteRaw(out, header);
    }
    ShaderPack pack(path);
    EXPECT_FALSE(pack.IsLoaded());
}

// =========================================================================
// Default / empty state
// =========================================================================

TEST_F(ShaderPackTest, DefaultConstructor)
{
    ShaderPack pack;
    EXPECT_FALSE(pack.IsLoaded());
    EXPECT_EQ(pack.GetShaderCount(), 0u);
    EXPECT_FALSE(pack.Contains("anything"));
    EXPECT_EQ(pack.LoadEntry("anything"), nullptr);
}

TEST_F(ShaderPackTest, EmptyPack)
{
    auto path = m_TestDir / "empty.osp";
    {
        std::ofstream out(path, std::ios::binary);
        FileHeader header;
        header.ShaderCount = 0;
        WriteRaw(out, header);
    }
    ShaderPack pack(path);
    EXPECT_TRUE(pack.IsLoaded());
    EXPECT_EQ(pack.GetShaderCount(), 0u);
    EXPECT_TRUE(pack.GetShaderNames().empty());
}
