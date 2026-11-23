#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
#include "TestTempDir.h"

#include "OloEngine/Project/ProjectSelection.h"

#include <gtest/gtest.h>
#include <fstream>

namespace OloEngine
{
    namespace fs = std::filesystem;

    class ProjectSelectionTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Root = Tests::TempDir("project-selection");
            fs::create_directories(m_Root / "Demo" / "CustomContent" / "Scenes");
            ASSERT_TRUE(std::ofstream(m_Root / "Demo" / "Demo.oloproj").good());
            ASSERT_TRUE(std::ofstream(m_Root / "Demo" / "CustomContent" / "Scenes" / "Intro.olo").good());
            ASSERT_TRUE(std::ofstream(m_Root / "Recent.oloproj").good());
        }

        fs::path m_Root;
    };

    TEST_F(ProjectSelectionTest, ExplicitProjectOverridesSceneAndRecent)
    {
        ProjectLaunchArguments args;
        args.Project = "Missing.oloproj";
        args.Scene = "Demo/CustomContent/Scenes/Intro.olo";
        const auto selection = ResolveProjectSelection(args, m_Root, "Recent.oloproj");
        EXPECT_EQ(selection.Path, m_Root / "Missing.oloproj");
        EXPECT_EQ(selection.Source, ProjectSelectionSource::Explicit);
    }

    TEST_F(ProjectSelectionTest, SceneInfersClosestDescriptorAboveCustomAssetRoot)
    {
        ProjectLaunchArguments args;
        args.Scene = "Demo/CustomContent/Scenes/Intro.olo";
        const auto selection = ResolveProjectSelection(args, m_Root, "Recent.oloproj");
        EXPECT_EQ(selection.Path, m_Root / "Demo" / "Demo.oloproj");
        EXPECT_EQ(selection.Source, ProjectSelectionSource::Scene);
    }

    TEST_F(ProjectSelectionTest, RecentProjectPrecedesSandboxFallback)
    {
        const auto selection = ResolveProjectSelection({}, m_Root, "Recent.oloproj");
        EXPECT_EQ(selection.Path, m_Root / "Recent.oloproj");
        EXPECT_EQ(selection.Source, ProjectSelectionSource::Recent);
    }

    TEST_F(ProjectSelectionTest, AmbiguousSceneDirectoryStopsBeforeAncestorAndRecentProject)
    {
        ASSERT_TRUE(std::ofstream(m_Root / "Demo" / "CustomContent" / "A.oloproj").good());
        ASSERT_TRUE(std::ofstream(m_Root / "Demo" / "CustomContent" / "B.oloproj").good());
        ProjectLaunchArguments args;
        args.Scene = "Demo/CustomContent/Scenes/Intro.olo";

        const auto selection = ResolveProjectSelection(args, m_Root, "Recent.oloproj");
        EXPECT_TRUE(selection.Path.empty());
        EXPECT_EQ(selection.Source, ProjectSelectionSource::Ambiguous);

        args.Project = "Demo/CustomContent/B.oloproj";
        EXPECT_EQ(ResolveProjectSelection(args, m_Root).Path, m_Root / args.Project);
    }

    TEST_F(ProjectSelectionTest, LegacyAssetsDirectoryCannotHideAmbiguity)
    {
        fs::create_directories(m_Root / "Demo" / "Assets" / "Scenes");
        ASSERT_TRUE(std::ofstream(m_Root / "Demo" / "Other.oloproj").good());
        ProjectLaunchArguments args;
        args.Scene = "Demo/Assets/Scenes/Intro.olo";
        const auto inferred = ResolveProjectSelection(args, m_Root, {}, true);
        EXPECT_TRUE(inferred.Path.empty());
        EXPECT_EQ(inferred.Source, ProjectSelectionSource::Ambiguous);

        args.Project = "Demo";
        const auto explicitDirectory = ResolveProjectSelection(args, m_Root, {}, true);
        EXPECT_TRUE(explicitDirectory.Path.empty());
        EXPECT_EQ(explicitDirectory.Source, ProjectSelectionSource::Ambiguous);
    }

    TEST_F(ProjectSelectionTest, MissingOrOmittedRecentProjectUsesSandboxFallback)
    {
        for (const fs::path& recent : { fs::path{}, fs::path("Moved.oloproj") })
        {
            const auto selection = ResolveProjectSelection({}, m_Root, recent);
            EXPECT_EQ(selection.Path, m_Root / "SandboxProject" / "Sandbox.oloproj");
            EXPECT_EQ(selection.Source, ProjectSelectionSource::SandboxFallback);
        }
    }

    TEST_F(ProjectSelectionTest, ServerSupportsBothLegacyDirectoriesAndDescriptors)
    {
        fs::create_directories(m_Root / "Legacy" / "Assets" / "Scenes");
        ProjectLaunchArguments args;
        args.Scene = "Legacy/Assets/Scenes/Server.olo";
        EXPECT_EQ(ResolveProjectSelection(args, m_Root, {}, true).Path, m_Root / "Legacy");

        args.Project = "Demo/";
        EXPECT_EQ(ResolveProjectSelection(args, m_Root, {}, true).Path, m_Root / "Demo" / "Demo.oloproj");
    }

    TEST(ProjectLaunchArgumentsTest, ExplicitFlagsOverridePositionalPaths)
    {
        char app[] = "app";
        char oldProject[] = "Old.OLOPROJ";
        char oldScene[] = "Old.OLO";
        char projectFlag[] = "--project";
        char project[] = "New Project.oloproj";
        char sceneFlag[] = "--scene";
        char scene[] = "New Scene.olo";
        char* argv[] = { app, oldProject, oldScene, projectFlag, project, sceneFlag, scene };
        const auto arguments = ParseProjectLaunchArguments(static_cast<int>(std::size(argv)), argv);
        EXPECT_EQ(arguments.Project, project);
        EXPECT_EQ(arguments.Scene, scene);
        EXPECT_TRUE(arguments.MissingValueFor.empty());
    }

    TEST(ProjectLaunchArgumentsTest, PositionalPathsAndMissingFlagValues)
    {
        char app[] = "app";
        char rhi[] = "--rhi";
        char vulkan[] = "vulkan";
        char project[] = "Demo.OLOPROJ";
        char scene[] = "Demo/Assets/Scenes/Intro.OLO";
        char projectFlag[] = "--project";
        char* argv[] = { app, rhi, vulkan, project, scene, projectFlag };
        const auto arguments = ParseProjectLaunchArguments(5, argv);
        EXPECT_EQ(arguments.Project, project);
        EXPECT_EQ(arguments.Scene, scene);
        EXPECT_EQ(ParseProjectLaunchArguments(6, argv).MissingValueFor, "--project");
    }
} // namespace OloEngine
