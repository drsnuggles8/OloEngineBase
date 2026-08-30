#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GameBuildPipelineTest — unit test (headless, no GL, no active project).
//
// Pins the target-platform contract added for issue #891: the platform a
// build is produced for is now a declared GameBuildSettings field rather than
// an accident of whatever host happens to run the pipeline, every
// host-specific naming convention derives from it, and asking for a target
// this host cannot produce fails loudly — before any output directory is
// created — instead of copying a host executable into a folder shaped for a
// different OS.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Build/GameBuildSettings.h"

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace OloEngine;

namespace
{
    // The one platform this host's IsBuildTargetSupportedOnThisHost must
    // reject — there are only two enumerators today, so "not the host" is
    // unambiguous.
    BuildTargetPlatform NonHostPlatform()
    {
        return GetHostBuildPlatform() == BuildTargetPlatform::Windows
                   ? BuildTargetPlatform::Linux
                   : BuildTargetPlatform::Windows;
    }
} // namespace

TEST(GameBuildPipelineTest, DefaultSettingsTargetTheHostPlatform)
{
    GameBuildSettings settings;
    EXPECT_EQ(settings.TargetPlatform, GetHostBuildPlatform())
        << "a freshly constructed GameBuildSettings should default to the platform it's running on.";
}

TEST(GameBuildPipelineTest, OnlyTheHostPlatformIsSupported)
{
    // No cross-compilation toolchain exists — the host platform is the only
    // platform it can ever be true for.
    EXPECT_TRUE(IsBuildTargetSupportedOnThisHost(GetHostBuildPlatform()));

    EXPECT_FALSE(IsBuildTargetSupportedOnThisHost(NonHostPlatform()));
}

TEST(GameBuildPipelineTest, HostExecutableNamingIsPlatformSpecific)
{
    EXPECT_EQ(GetHostExecutableFileName("OloRuntime", BuildTargetPlatform::Windows), "OloRuntime.exe");
    EXPECT_EQ(GetHostExecutableFileName("OloRuntime", BuildTargetPlatform::Linux), "OloRuntime");
    EXPECT_EQ(GetHostExecutableFileName("MyGame", BuildTargetPlatform::Windows), "MyGame.exe");
    EXPECT_EQ(GetHostExecutableFileName("MyGame", BuildTargetPlatform::Linux), "MyGame");
}

TEST(GameBuildPipelineTest, CSharpScriptingIsWindowsOnly)
{
    // OloEngine-ScriptCore only builds under the Visual Studio generator
    // (CLAUDE.md) — so C# scripting cannot be shipped on a non-Windows target
    // regardless of what a build's other settings ask for.
    EXPECT_TRUE(IsScriptingAvailableOnPlatform(BuildTargetPlatform::Windows));
    EXPECT_FALSE(IsScriptingAvailableOnPlatform(BuildTargetPlatform::Linux));
}

TEST(GameBuildPipelineTest, WindowsStagesEveryDynamicLibraryBesideTheRuntime)
{
    const auto runtimeDir = OloEngine::Tests::TempDir("runtime-dependencies");
    const auto outputDir = OloEngine::Tests::TempDir("staged-dependencies");
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const std::array expectedNames{
        "avcodec-61.dll",
        "steam_api64.dll",
        "future-runtime-dependency.DLL",
    };
    for (const auto* name : expectedNames)
    {
        std::ofstream(runtimeDir / name) << name;
    }
    std::ofstream(runtimeDir / "OloRuntime.exe") << "runtime";
    std::ofstream(runtimeDir / "avcodec.lib") << "import library";

    sizet copiedCount = 0;
    std::string errorMessage;
    ASSERT_TRUE(StageRuntimeDependencyLibraries(
        BuildTargetPlatform::Windows, runtimeDir, outputDir, copiedCount, errorMessage))
        << errorMessage;

    EXPECT_EQ(copiedCount, expectedNames.size());
    for (const auto* name : expectedNames)
    {
        EXPECT_TRUE(std::filesystem::exists(outputDir / name)) << name;
    }
    EXPECT_FALSE(std::filesystem::exists(outputDir / "OloRuntime.exe"));
    EXPECT_FALSE(std::filesystem::exists(outputDir / "avcodec.lib"));
}

TEST(GameBuildPipelineTest, LooseRuntimeTexturesPreserveTheirProjectRelativePaths)
{
    const auto projectAssets = OloEngine::Tests::TempDir("project-assets");
    const auto outputAssets = OloEngine::Tests::TempDir("output-assets");
    std::filesystem::create_directories(projectAssets / "Textures" / "Menu");
    std::filesystem::create_directories(projectAssets / "Models" / "Boat");

    const std::array expectedPaths{
        std::filesystem::path{ "Textures/Menu/background.png" },
        std::filesystem::path{ "Models/Boat/albedo.JPG" },
    };
    for (const auto& path : expectedPaths)
    {
        std::ofstream(projectAssets / path) << path.generic_string();
    }
    std::ofstream(projectAssets / "Textures/Menu/readme.txt") << "not a texture";

    sizet copiedCount = 0;
    std::string errorMessage;
    ASSERT_TRUE(StageLooseRuntimeTextures(projectAssets, outputAssets, copiedCount, errorMessage))
        << errorMessage;

    EXPECT_EQ(copiedCount, expectedPaths.size());
    for (const auto& path : expectedPaths)
    {
        EXPECT_TRUE(std::filesystem::exists(outputAssets / path)) << path;
    }
    EXPECT_FALSE(std::filesystem::exists(outputAssets / "Textures/Menu/readme.txt"));
}

TEST(GameBuildPipelineTest, ShaderPackIsStagedWhenPresent)
{
    const auto editorAssets = OloEngine::Tests::TempDir("shaderpack-src");
    const auto outputAssets = OloEngine::Tests::TempDir("shaderpack-dst");
    std::filesystem::create_directories(editorAssets);

    const auto packSrc = editorAssets / "ShaderPack.osp";
    std::ofstream(packSrc) << "not a real pack, just bytes for the copy step";

    bool packStaged = false;
    std::string errorMessage;
    ASSERT_TRUE(StageShaderPack(packSrc, outputAssets, packStaged, errorMessage)) << errorMessage;

    EXPECT_TRUE(packStaged);
    EXPECT_TRUE(std::filesystem::exists(outputAssets / "ShaderPack.osp"));
}

TEST(GameBuildPipelineTest, ShaderPackStagingIsANoOpWhenAbsent)
{
    const auto editorAssets = OloEngine::Tests::TempDir("shaderpack-src-missing");
    const auto outputAssets = OloEngine::Tests::TempDir("shaderpack-dst-missing");
    std::filesystem::create_directories(editorAssets);
    // Deliberately no ShaderPack.osp written — a fresh worktree, or the CI
    // bake step never ran, is not a build failure (issue #908's plan
    // explicitly leans "explicit on request" for whether it exists).

    bool packStaged = false;
    std::string errorMessage;
    ASSERT_TRUE(StageShaderPack(editorAssets / "ShaderPack.osp", outputAssets, packStaged, errorMessage))
        << errorMessage;

    EXPECT_FALSE(packStaged);
    EXPECT_FALSE(std::filesystem::exists(outputAssets / "ShaderPack.osp"));
}

TEST(GameBuildPipelineTest, ToStringIsHumanReadable)
{
    EXPECT_STREQ(ToString(BuildTargetPlatform::Windows), "Windows");
    EXPECT_STREQ(ToString(BuildTargetPlatform::Linux), "Linux");
}

// The core "fail loudly" contract (#891 acceptance criterion 4). This check
// runs before ValidateProject, so it needs no active project — an unsupported
// target must be rejected on its own, without an output directory ever being
// created for it.
TEST(GameBuildPipelineTest, BuildRejectsATargetThisHostCannotProduce)
{
    const auto unsupportedPlatform = NonHostPlatform();

    GameBuildSettings settings;
    settings.GameName = "UnsupportedTargetTestGame";
    settings.TargetPlatform = unsupportedPlatform;
    settings.OutputDirectory = OloEngine::Tests::TempDir("unsupported-target-build");

    std::atomic<f32> progress{ 0.0f };
    GameBuildResult result = GameBuildPipeline::Build(settings, progress);

    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.ErrorMessage.find(ToString(unsupportedPlatform)), std::string::npos)
        << "the error should name the platform that was rejected: " << result.ErrorMessage;
    EXPECT_NE(result.ErrorMessage.find("cross-compilation"), std::string::npos)
        << "the error should explain WHY, not just fail silently opaque: " << result.ErrorMessage;

    // No output should have been produced for a build that never got past
    // the platform gate — the whole point is to avoid a folder that looks
    // fine and doesn't run. Check the filesystem directly, not just the
    // returned struct: the gate must reject before EVER creating the
    // <OutputDirectory>/<GameName> staging directory.
    EXPECT_TRUE(result.OutputPath.empty());
    EXPECT_FALSE(std::filesystem::exists(settings.OutputDirectory / settings.GameName))
        << "the platform gate must reject before creating any output directory.";
}

TEST(GameBuildPipelineTest, BuildAcceptsTheHostPlatformPastTheGate)
{
    // A supported target must clear the platform gate — the very next check
    // (no active project) is what should fail it instead, proving the gate
    // itself doesn't reject a legitimate build.
    GameBuildSettings settings;
    settings.GameName = "SupportedTargetTestGame";
    settings.TargetPlatform = GetHostBuildPlatform();

    std::atomic<f32> progress{ 0.0f };
    GameBuildResult result = GameBuildPipeline::Build(settings, progress);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorMessage.find("cross-compilation"), std::string::npos)
        << "a supported target must not be rejected by the platform gate: " << result.ErrorMessage;
}
