// =============================================================================
// ShaderPackContentHashTest.cpp
//
// Pins the invalidation contract added for issue #908: a ShaderPack entry is
// keyed by a content hash (OpenGLShader::ComputeContentHash — the same
// ingredients, preprocessed source + Vulkan-tier compiler options, that the
// #906 per-stage SPIR-V cache already hashes on) rather than by name alone.
// A pack whose baked hash no longer matches the shader's current on-disk
// source must be a MISS — served SPIR-V compiled from a different source
// than what's on disk right now would be silently, permanently wrong.
//
// Entirely GL-free: ShaderPack::CreateFromFilepaths uses only the CPU-side
// prepare path (Shader::PrepareBatch — read, preprocess, shaderc,
// SPIRV-Cross), and ShaderLibrary::PrepareParallel's pack lookup makes no GL
// call either (materializing the GL program is deferred to
// FinalizeParallel(), which this test never calls). Runs on headless CI.
//
// OLO_TEST_LAYER: shaderpipe
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <algorithm>
#include <fstream>
#include <string>

using namespace OloEngine;

namespace
{
    constexpr const char* kMinimalShaderSource = R"glsl(
#type vertex
#version 450 core
layout(location = 0) in vec3 a_Position;
void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = vec4(1.0, 0.0, 1.0, 1.0);
}
)glsl";

    void WriteFile(const std::filesystem::path& path, std::string_view contents)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }
} // namespace

TEST(ShaderPackContentHashTest, ComputeContentHashIsStableAndSensitiveToContent)
{
    const auto dir = OloEngine::Tests::TempDir("shaderpack-hash-compute");
    const auto shaderPath = dir / "HashProbe.glsl";
    WriteFile(shaderPath, kMinimalShaderSource);

    const std::string filepathStr = shaderPath.generic_string();
    const std::string hashA = OpenGLShader::ComputeContentHash(filepathStr);
    const std::string hashB = OpenGLShader::ComputeContentHash(filepathStr);
    ASSERT_FALSE(hashA.empty());
    EXPECT_EQ(hashA, hashB) << "hashing the same unchanged file twice must be reproducible";

    WriteFile(shaderPath, std::string(kMinimalShaderSource) + "\n// a trailing change\n");
    const std::string hashC = OpenGLShader::ComputeContentHash(filepathStr);
    EXPECT_NE(hashA, hashC) << "any source change must change the hash";
}

TEST(ShaderPackContentHashTest, MatchingHashIsServedFromThePack)
{
    const auto dir = OloEngine::Tests::TempDir("shaderpack-hash-hit");
    const auto shaderPath = dir / "HitProbe.glsl";
    WriteFile(shaderPath, kMinimalShaderSource);
    const std::string filepathStr = shaderPath.generic_string();

    const auto packPath = dir / "Test.osp";
    ASSERT_TRUE(ShaderPack::CreateFromFilepaths({ filepathStr }, packPath));

    ShaderLibrary lib;
    lib.LoadShaderPack(packPath);
    ASSERT_TRUE(lib.HasShaderPack());

    auto batch = lib.PrepareParallel({ filepathStr });
    ASSERT_EQ(batch.m_IsPackLoaded.size(), 1u);
    EXPECT_TRUE(batch.m_IsPackLoaded[0]) << "an unmutated source must hit the pack it was baked from";
}

TEST(ShaderPackContentHashTest, MutatedSourceMissesAndFallsBackToCompile)
{
    const auto dir = OloEngine::Tests::TempDir("shaderpack-hash-miss");
    const auto shaderPath = dir / "MissProbe.glsl";
    WriteFile(shaderPath, kMinimalShaderSource);
    const std::string filepathStr = shaderPath.generic_string();

    const auto packPath = dir / "Test.osp";
    ASSERT_TRUE(ShaderPack::CreateFromFilepaths({ filepathStr }, packPath));

    // Mutate AFTER the pack was baked — the pack now describes stale bytes.
    WriteFile(shaderPath, std::string(kMinimalShaderSource) + "\n// mutated after bake\n");

    ShaderLibrary lib;
    lib.LoadShaderPack(packPath);

    auto batch = lib.PrepareParallel({ filepathStr });
    ASSERT_EQ(batch.m_IsPackLoaded.size(), 1u);
    EXPECT_FALSE(batch.m_IsPackLoaded[0]) << "a mutated source must miss the now-stale pack entry";
    ASSERT_EQ(batch.m_Prepared.size(), 1u);
    EXPECT_NE(batch.m_Prepared[0], nullptr) << "a miss must still fall back to a CPU compile, not drop the shader";
}

TEST(ShaderPackContentHashTest, CollectShaderFilepathsSkipsIncludeAndTestsDirectories)
{
    const auto root = OloEngine::Tests::TempDir("shaderpack-collect");
    std::filesystem::create_directories(root / "include");
    std::filesystem::create_directories(root / "tests");
    std::filesystem::create_directories(root / "compute");

    WriteFile(root / "Main.glsl", kMinimalShaderSource);
    WriteFile(root / "compute" / "Compute.glsl", kMinimalShaderSource);
    WriteFile(root / "include" / "Common.glsl", "// header, no #type marker\n");
    WriteFile(root / "tests" / "TestOnly.glsl", kMinimalShaderSource);
    WriteFile(root / "NotAShader.txt", "ignore me");

    const auto found = ShaderPack::CollectShaderFilepaths(root);

    auto contains = [&found](std::string_view suffix)
    {
        return std::ranges::any_of(found, [&](const std::string& p)
                                   { return p.ends_with(suffix); });
    };

    EXPECT_TRUE(contains("Main.glsl"));
    EXPECT_TRUE(contains("compute/Compute.glsl"));
    EXPECT_FALSE(contains("Common.glsl")) << "include/ headers must be excluded";
    EXPECT_FALSE(contains("TestOnly.glsl")) << "tests/ content must be excluded";
    EXPECT_EQ(found.size(), 2u);
}

TEST(ShaderPackContentHashTest, PrepareBatchProducesADestructibleShaderWithNoGLContext)
{
    // Regression pin for the bug this file's other tests originally hit:
    // OpenGLShader::~OpenGLShader() used to unconditionally enqueue a GL
    // program deletion (FrameResourceManager::SubmitForDeletion), which runs
    // its lambda SYNCHRONOUSLY whenever the manager was never Init()'d — an
    // assumption that "not yet initialized" still means "a GL context is
    // live" that holds for every other caller but not this one: a purely
    // headless bake process where GLAD's function pointers were never
    // loaded, crashing on an unloaded glDeleteProgram. Fixed by skipping
    // that whole block when m_RendererID == 0 (nothing was ever created).
    const auto dir = OloEngine::Tests::TempDir("shaderpack-hash-preparebatch");
    const auto shaderPath = dir / "PBProbe.glsl";
    WriteFile(shaderPath, kMinimalShaderSource);
    const std::string filepathStr = shaderPath.generic_string();

    auto prepared = Shader::PrepareBatch({ filepathStr }, nullptr);
    ASSERT_EQ(prepared.size(), 1u);
    ASSERT_NE(prepared[0], nullptr);
    // `prepared` destructs here — the crash, when present, happened after
    // this test body returned, inside the resulting ~OpenGLShader() call.
}
