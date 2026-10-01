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
#include "OloEngine/Build/LinuxDesktopLauncher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    void WritePngSignature(const std::filesystem::path& path)
    {
        constexpr std::array<unsigned char, 8> signature{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
    }

#ifdef OLO_PLATFORM_LINUX
    std::string ShellQuote(const std::filesystem::path& path)
    {
        std::string quoted{ "'" };
        for (const auto character : path.string())
        {
            if (character == '\'')
            {
                quoted += "'\"'\"'";
            }
            else
            {
                quoted += character;
            }
        }
        return quoted + "'";
    }
#endif
} // namespace

TEST(GameBuildPipelineTest, LinuxLauncherAndIconStayInsideAMovedPackage)
{
    const auto testRoot = OloEngine::Tests::TempDir("linux-launcher-relocation");
    const auto originalPackage = testRoot / "authoring output $`" / "Portable Game";
    const auto movedPackage = testRoot / "moved ä distribution %$`" / "Portable Game";
    std::filesystem::create_directories(originalPackage);

    const auto executable = originalPackage / "Portable Game";
    std::ofstream(executable) << "runtime";
    const auto selectedIcon = testRoot / "selected icon.png";
    WritePngSignature(selectedIcon);

    LinuxDesktopLauncherArtifacts artifacts;
    std::string errorMessage;
    ASSERT_TRUE(StageLinuxDesktopLauncher(executable, selectedIcon, "Portable Game", artifacts, errorMessage))
        << errorMessage;

    const auto desktopRelative = std::filesystem::relative(artifacts.DesktopEntryPath, originalPackage);
    const auto wrapperRelative = std::filesystem::relative(artifacts.WrapperScriptPath, originalPackage);
    const auto iconRelative = std::filesystem::relative(artifacts.PackagedIconPath, originalPackage);

    std::filesystem::create_directories(movedPackage.parent_path());
    std::filesystem::rename(originalPackage, movedPackage);

    const auto movedDesktop = movedPackage / desktopRelative;
    const auto movedWrapper = movedPackage / wrapperRelative;
    const auto movedIcon = movedPackage / iconRelative;
    ASSERT_TRUE(std::filesystem::exists(movedDesktop));
    ASSERT_TRUE(std::filesystem::exists(movedWrapper));
    ASSERT_TRUE(std::filesystem::exists(movedIcon));
    EXPECT_EQ(ReadTextFile(movedIcon), ReadTextFile(selectedIcon));

    const auto desktopText = ReadTextFile(movedDesktop);
    EXPECT_EQ(desktopText.find(originalPackage.string()), std::string::npos);
    EXPECT_NE(desktopText.find("Exec=/bin/sh -c"), std::string::npos);
    EXPECT_NE(desktopText.find(" olo-launcher %k"), std::string::npos);
    EXPECT_EQ(desktopText.find("Icon="), std::string::npos)
        << "A portable entry cannot use a stale absolute Icon= before its wrapper derives the moved path.";

    const auto wrapperText = ReadTextFile(movedWrapper);
    EXPECT_EQ(wrapperText.find(originalPackage.string()), std::string::npos);
    EXPECT_NE(wrapperText.find("launcher_dir"), std::string::npos);
    EXPECT_NE(wrapperText.find("Portable Game"), std::string::npos);
    EXPECT_NE(wrapperText.find("icons/game-icon.png"), std::string::npos);
    EXPECT_NE(wrapperText.find("temporary_entry"), std::string::npos);
}

TEST(GameBuildPipelineTest, LinuxLauncherRejectsMissingAndUnsupportedIcons)
{
    const auto testRoot = OloEngine::Tests::TempDir("linux-launcher-invalid-icon");
    const auto executable = testRoot / "Game";
    std::ofstream(executable) << "runtime";

    LinuxDesktopLauncherArtifacts artifacts;
    std::string errorMessage;
    EXPECT_FALSE(StageLinuxDesktopLauncher(executable, testRoot / "missing.png", "Game", artifacts, errorMessage));
    EXPECT_NE(errorMessage.find("not found"), std::string::npos);

    const auto unsupportedIcon = testRoot / "icon.ico";
    std::ofstream(unsupportedIcon) << "not a Linux icon";
    errorMessage.clear();
    EXPECT_FALSE(StageLinuxDesktopLauncher(executable, unsupportedIcon, "Game", artifacts, errorMessage));
    EXPECT_NE(errorMessage.find("PNG, SVG, or XPM"), std::string::npos);

    const auto corruptPng = testRoot / "corrupt.png";
    std::ofstream(corruptPng) << "not a PNG";
    errorMessage.clear();
    EXPECT_FALSE(StageLinuxDesktopLauncher(executable, corruptPng, "Game", artifacts, errorMessage));
    EXPECT_NE(errorMessage.find("does not match"), std::string::npos);
}

TEST(GameBuildPipelineTest, LinuxLauncherAllowsAnEmptyIconAndOverwritesPackagedIcons)
{
    const auto testRoot = OloEngine::Tests::TempDir("linux-launcher-regeneration");
    const auto executable = testRoot / "Game";
    std::ofstream(executable) << "runtime";

    LinuxDesktopLauncherArtifacts emptyArtifacts;
    std::string errorMessage;
    ASSERT_TRUE(StageLinuxDesktopLauncher(executable, {}, "Game", emptyArtifacts, errorMessage)) << errorMessage;
    EXPECT_TRUE(emptyArtifacts.PackagedIconPath.empty());
    EXPECT_EQ(ReadTextFile(emptyArtifacts.DesktopEntryPath).find("Icon="), std::string::npos);

    const auto icon = testRoot / "icon.PNG";
    WritePngSignature(icon);
    LinuxDesktopLauncherArtifacts firstArtifacts;
    ASSERT_TRUE(StageLinuxDesktopLauncher(executable, icon, "Game", firstArtifacts, errorMessage)) << errorMessage;
    EXPECT_EQ(firstArtifacts.PackagedIconPath.filename(), "game-icon.png");

    std::ofstream(icon, std::ios::binary | std::ios::app) << "replacement";
    LinuxDesktopLauncherArtifacts secondArtifacts;
    ASSERT_TRUE(StageLinuxDesktopLauncher(executable, icon, "Game", secondArtifacts, errorMessage)) << errorMessage;
    EXPECT_EQ(firstArtifacts.PackagedIconPath, secondArtifacts.PackagedIconPath);
    EXPECT_EQ(ReadTextFile(secondArtifacts.PackagedIconPath), ReadTextFile(icon));
}

TEST(GameBuildPipelineTest, LinuxLauncherExecutesFromARelocatedPackage)
{
#ifndef OLO_PLATFORM_LINUX
    GTEST_SKIP() << "Requires a Linux shell and filesystem permissions.";
#else
    const auto testRoot = OloEngine::Tests::TempDir("linux-launcher-execution");
    const auto sourcePackage = testRoot / "source package";
    const auto movedPackage = testRoot / "moved package";
    std::filesystem::create_directories(sourcePackage);

    const auto executable = sourcePackage / "Game";
    std::ofstream(executable) << "#!/bin/sh\nprintf launched > \"$0.launched\"\n";
    std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    const auto icon = testRoot / "icon.png";
    WritePngSignature(icon);

    LinuxDesktopLauncherArtifacts artifacts;
    std::string errorMessage;
    ASSERT_TRUE(StageLinuxDesktopLauncher(executable, icon, "Game", artifacts, errorMessage)) << errorMessage;
    std::filesystem::permissions(artifacts.DesktopEntryPath, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    std::filesystem::permissions(artifacts.WrapperScriptPath, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    std::filesystem::rename(sourcePackage, movedPackage);

    const auto movedDesktop = movedPackage / artifacts.DesktopEntryPath.filename();
    const auto movedIcon = movedPackage / "icons/game-icon.png";
    const auto bootstrap = "/bin/sh -c 'exec \"$(dirname -- \"$1\")/OloGameLauncher.sh\"' olo-launcher " + ShellQuote(movedDesktop);
    ASSERT_EQ(std::system(bootstrap.c_str()), 0);
    EXPECT_TRUE(std::filesystem::exists(movedPackage / "Game.launched"));
    EXPECT_NE(ReadTextFile(movedDesktop).find("Icon=" + movedIcon.string()), std::string::npos);
    EXPECT_NE(std::filesystem::status(movedDesktop).permissions() & std::filesystem::perms::owner_exec,
              std::filesystem::perms::none);

    if (std::system("command -v desktop-file-validate >/dev/null 2>&1") == 0)
    {
        EXPECT_EQ(std::system(("desktop-file-validate " + ShellQuote(movedDesktop)).c_str()), 0);
    }
#endif
}

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
    // The engine links the Mono runtime only on Windows, so C# scripting cannot
    // be shipped on a non-Windows target regardless of what a build's other
    // settings ask for.
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

// Issue #1392: a foliage layer opens its MeshPath from disk, not from the pack,
// so a packaged game without the .obj (and the .mtl and textures it pulls in)
// drew every authored plant as a flat card. The staging follows a scene's path
// scalars and each staged file's own references, and nothing else.
TEST(GameBuildPipelineTest, SceneReferencedContentStagesEveryFileAScenePathOpens)
{
    namespace fs = std::filesystem;
    // The project sits one level down so its "../" reference lands in this
    // case's own scratch directory.
    const auto project = OloEngine::Tests::TempDir("scene-refs") / "project";
    const auto engine = OloEngine::Tests::TempDir("scene-refs-engine");
    const auto output = OloEngine::Tests::TempDir("scene-refs-output");
    fs::create_directories(project / "Assets/Scenes");
    fs::create_directories(project / "Assets/Models/Plant/Textures");
    fs::create_directories(project / "Assets/Models/Rock");
    fs::create_directories(project / "Assets/Models/Unused");
    fs::create_directories(project / "Assets/Skies/Day");
    fs::create_directories(engine / "assets/textures");

    std::ofstream(project / "Assets/Models/Plant/plant.obj") << "# plant\nmtllib plant.mtl bark.mtl\nv 0 0 0\n";
    std::ofstream(project / "Assets/Models/Plant/bark.mtl") << "newmtl bark\nrefl -type sphere -mm 0 1 Textures/chrome.png\n";
    WritePngSignature(project / "Assets/Models/Plant/Textures/chrome.png");
    // A one-part working-directory value and the library beside it.
    std::ofstream(engine / "tree.obj") << "mtllib tree.mtl\nv 0 0 0\n";
    std::ofstream(engine / "tree.mtl") << "newmtl tree\n";
    std::ofstream(project / "Assets/Models/Plant/plant.mtl")
        << "newmtl leaves\nmap_Kd -s 1 1 1 Textures/leaves big.png\nmap_Bump -bm 0.5 Textures/leaves_n.png\n";
    WritePngSignature(project / "Assets/Models/Plant/Textures/leaves big.png");
    // Resolved against the working directory, like ResolveContentPath does.
    fs::create_directories(engine / "Resources/Icons");
    WritePngSignature(engine / "Resources/Icons/marker.png");
    // One path, two files: "Assets/" and "assets/" are one directory in a
    // Windows package.
    fs::create_directories(project / "Assets/Shared");
    fs::create_directories(engine / "assets/shared");
    std::ofstream(project / "Assets/Shared/Tex.png") << "project";
    std::ofstream(engine / "assets/shared/tex.png") << "engine";
    WritePngSignature(project / "Assets/Models/Plant/Textures/leaves_n.png");
    std::ofstream(project / "Assets/Models/Plant/plant.obj.oloimport") << "FlipUV: false\n";
    std::ofstream(project / "Assets/Models/Rock/rock.gltf")
        << R"({"buffers":[{"uri":"rock%20data.bin"},{"uri":"data:application/octet-stream;base64,AAAA"}],)"
        << R"("images":[{"uri":"rock_albedo.png"}]})";
    std::ofstream(project / "Assets/Models/Rock/rock data.bin") << "bytes";
    WritePngSignature(project / "Assets/Models/Rock/rock_albedo.png");
    std::ofstream(project / "Assets/Models/Unused/unused.obj") << "v 0 0 0\n";
    fs::create_directories(project / "Assets/Audio");
    std::ofstream(project / "Assets/Audio/Wind.ogg") << "ogg";
    WritePngSignature(project / "Assets/Skies/Day/right.png");
    WritePngSignature(project / "Assets/Skies/Day/left.png");
    WritePngSignature(engine / "assets/textures/leaf_normal.png");

    const fs::path scene = project / "Assets/Scenes/Meadow.olo";
    std::ofstream(scene) << "Scene: Meadow.olo\n"
                            "Entities:\n"
                            "  - Entity: 1\n"
                            "    TagComponent:\n"
                            "      Tag: Assets is not a path\n"
                            "    FoliageComponent:\n"
                            "      Layers:\n"
                            "        - Name: Grass\n"
                            "          MeshPath: Assets/Models/Plant/plant.obj\n"
                            "          NormalMapPath: assets/textures/leaf_normal.png\n"
                            "          RoughnessMapPath: assets/textures/missing.png\n"
                            "        - Name: Rocks\n"
                            "          MeshPath: Assets/Models/Rock/rock.gltf\n"
                            "          AlbedoPath: ../outside/texture.png\n"
                            "          ThicknessMapPath: Assets/../../escape.png\n"
                            "    EnvironmentMapComponent:\n"
                            "      FilePath: Assets/Skies/Day\n"
                            "  - Entity: 2\n"
                            "    AudioSourceComponent:\n"
                            "      Filepath: Audio/Wind.ogg\n"
                            "  - Entity: 3\n"
                            "    FoliageComponent:\n"
                            "      Layers:\n"
                            "        - AlbedoPath: ../outside/texture.png\n"
                            "    SpriteRendererComponent:\n"
                            "      TexturePath: Resources/Icons/marker.png\n"
                            "  - Entity: 4\n"
                            "    MaterialA: Assets/Shared/Tex.png\n"
                            "    MaterialB: assets/shared/tex.png\n"
                            "    LooseModel: tree.obj\n";
    fs::create_directories(project.parent_path() / "outside");
    WritePngSignature(project.parent_path() / "outside/texture.png");

    sizet copiedCount = 0;
    std::vector<std::string> unresolved;
    std::string errorMessage;
    ASSERT_TRUE(StageSceneReferencedContent({ scene }, project, "Assets", engine, output, copiedCount, unresolved,
                                            errorMessage))
        << errorMessage;

    const std::array staged{
        fs::path{ "Assets/Models/Plant/plant.obj" },
        fs::path{ "Assets/Models/Plant/plant.mtl" },
        fs::path{ "Assets/Models/Plant/bark.mtl" },           // the second name on the mtllib line
        fs::path{ "Assets/Models/Plant/Textures/chrome.png" }, // after "-type sphere -mm 0 1"
        fs::path{ "Assets/Models/Plant/Textures/leaves big.png" }, // after "-s 1 1 1", spaces and all
        fs::path{ "Assets/Models/Plant/Textures/leaves_n.png" },
        fs::path{ "Assets/Models/Plant/plant.obj.oloimport" },
        fs::path{ "Assets/Models/Rock/rock.gltf" },
        fs::path{ "Assets/Models/Rock/rock data.bin" },
        fs::path{ "Assets/Models/Rock/rock_albedo.png" },
        fs::path{ "Assets/Skies/Day/left.png" },
        fs::path{ "Assets/Skies/Day/right.png" },
        fs::path{ "assets/textures/leaf_normal.png" },
        // Asset-relative: an audio source's Filepath is read back as
        // <AssetDirectory>/<value>.
        fs::path{ "Assets/Audio/Wind.ogg" },
        fs::path{ "Resources/Icons/marker.png" },
        fs::path{ "tree.obj" },
        fs::path{ "tree.mtl" },
    };
    for (const auto& path : staged)
    {
        EXPECT_TRUE(fs::is_regular_file(output / path)) << path.generic_string() << " was not staged";
    }
    // ...plus exactly one of the two colliding files.
    EXPECT_EQ(copiedCount, staged.size() + 1u);
    EXPECT_TRUE(fs::is_regular_file(output / "Assets/Shared/Tex.png"));
    EXPECT_FALSE(fs::exists(output / "Assets/Models/Unused/unused.obj")) << "an unreferenced model was staged";
    EXPECT_FALSE(fs::exists(output / "Assets/Scenes")) << "the scenes ship through CopySceneFiles, not here";

    // Missing, outside the project, escaping the asset root, and colliding:
    // each named once (the ../ reference appears twice), none a build failure.
    ASSERT_EQ(unresolved.size(), 4u);
    const auto mentions = [&](const std::string& needle)
    {
        return std::ranges::any_of(unresolved, [&](const std::string& line)
                                   { return line.find(needle) != std::string::npos; });
    };
    EXPECT_TRUE(mentions("assets/textures/missing.png"));
    EXPECT_TRUE(mentions("../outside/texture.png"));
    EXPECT_TRUE(mentions("escape.png"));
    EXPECT_TRUE(mentions("collides with"));
    for (const auto& line : unresolved)
    {
        EXPECT_EQ(line.rfind("Meadow.olo: ", 0), 0u) << "an unresolved reference must name its scene: " << line;
    }
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
    // The gate is passed, so the pipeline creates <OutputDirectory>/<GameName> before the
    // no-project check fails it. Without an explicit OutputDirectory that lands in the
    // working directory (OloEditor/ under ctest) and stays there as untracked junk.
    settings.OutputDirectory = OloEngine::Tests::TempDir("supported-target-build");

    std::atomic<f32> progress{ 0.0f };
    GameBuildResult result = GameBuildPipeline::Build(settings, progress);

    EXPECT_FALSE(result.Success);
    EXPECT_EQ(result.ErrorMessage.find("cross-compilation"), std::string::npos)
        << "a supported target must not be rejected by the platform gate: " << result.ErrorMessage;
}
