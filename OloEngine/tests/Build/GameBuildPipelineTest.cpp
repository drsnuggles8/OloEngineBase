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

#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Build/GameBuildSettings.h"

#include <atomic>
#include <filesystem>

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

    std::atomic<f32> progress{ 0.0f };
    GameBuildResult result = GameBuildPipeline::Build(settings, progress);

    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.ErrorMessage.find(ToString(unsupportedPlatform)), std::string::npos)
        << "the error should name the platform that was rejected: " << result.ErrorMessage;
    EXPECT_NE(result.ErrorMessage.find("cross-compilation"), std::string::npos)
        << "the error should explain WHY, not just fail silently opaque: " << result.ErrorMessage;

    // No output should have been produced for a build that never got past
    // the platform gate — the whole point is to avoid a folder that looks
    // fine and doesn't run.
    EXPECT_TRUE(result.OutputPath.empty());
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
