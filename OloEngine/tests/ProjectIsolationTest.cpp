// OLO_TEST_LAYER: meta
//
// ProjectIsolation.h: a test that leaves a project and an asset manager active
// must not hand them to the test after it. Each suite has two probes that both
// check what the probe before them left, then leave their own change behind,
// so whichever runs second checks the restore, in either order. A probe run on
// its own has nothing earlier to check. FoxCoatEvidenceTest leaked for real and
// kept its fox assets resident for the rest of the AMD nightly.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"

#include "TestTempDir.h"

#include <filesystem>

namespace
{
    using namespace OloEngine;

    struct ProbeRecord
    {
        bool Ran = false;
        bool HadProject = false;
        bool HadAssetManager = false;
        std::filesystem::path LeakedDirectory;
    };

    ProbeRecord& Record()
    {
        static ProbeRecord s_Record;
        return s_Record;
    }

    // The previous probe's project and asset manager must be gone, and what that
    // probe found active must be back. Then leave a project and an asset manager.
    void CheckThenLeakAProject()
    {
        ProbeRecord& record = Record();
        if (record.Ran)
        {
            EXPECT_EQ(Project::GetActive() != nullptr, record.HadProject);
            EXPECT_EQ(Project::HasAssetManager(), record.HadAssetManager);
            if (Project::GetActive())
            {
                EXPECT_NE(Project::GetActive()->GetDirectory(), record.LeakedDirectory)
                    << "the previous test's project is still active";
            }
        }

        record.HadProject = Project::GetActive() != nullptr;
        record.HadAssetManager = Project::HasAssetManager();
        record.LeakedDirectory = OloEngine::Tests::TempDir("leaked-project");
        Project::NewInMemory(record.LeakedDirectory, ProjectConfig{});
        Project::SetAssetManager(Ref<EditorAssetManager>::Create());
        record.Ran = true;
        ASSERT_TRUE(Project::GetActive());
        ASSERT_TRUE(Project::HasAssetManager());
    }
} // namespace

TEST(ProjectIsolationProbe, ALeavesAProjectAfterCheckingTheLastOneWasRemoved)
{
    CheckThenLeakAProject();
}

TEST(ProjectIsolationProbe, BLeavesAProjectAfterCheckingTheLastOneWasRemoved)
{
    CheckThenLeakAProject();
}

// The restore puts back the captured objects themselves. A suite that set up a
// project in SetUpTestSuite and holds it must still see that object after a test
// that swapped only the asset manager, not a rebuilt copy with the same directory.
class ProjectIsolationSuiteProbe : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        // Under this process's temp root: a released asset manager writes its
        // registry into the project directory, and a relative one would land in
        // the working directory, which is the repository.
        const std::filesystem::path directory = OloEngine::Tests::TempRoot() / "ProjectIsolationSuiteProbe";
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        ASSERT_FALSE(ec) << "cannot create " << directory.string() << ": " << ec.message();
        s_Project = Project::NewInMemory(directory, ProjectConfig{});
        s_AssetManager = Ref<EditorAssetManager>::Create();
        Project::SetAssetManager(s_AssetManager);
        s_Swapped = false;
    }
    static void TearDownTestSuite()
    {
        Project::Unload();
        s_AssetManager = nullptr;
        s_Project = nullptr;
    }

    // After an earlier probe swapped the asset manager, the suite's own objects
    // must be active again. Then swap it again for the next probe.
    static void CheckThenSwapTheAssetManager()
    {
        if (s_Swapped)
        {
            EXPECT_EQ(Project::GetActive().Raw(), s_Project.Raw()) << "the suite's project was replaced by a copy";
            ASSERT_TRUE(Project::HasAssetManager());
            EXPECT_EQ(Project::GetAssetManager().Raw(), s_AssetManager.Raw());
        }
        Project::SetAssetManager(Ref<EditorAssetManager>::Create());
        s_Swapped = true;
        ASSERT_NE(Project::GetAssetManager().Raw(), s_AssetManager.Raw());
    }

    inline static Ref<Project> s_Project;
    inline static Ref<AssetManagerBase> s_AssetManager;
    inline static bool s_Swapped = false;
};

TEST_F(ProjectIsolationSuiteProbe, ASwapsTheAssetManagerAfterCheckingTheSuitesWasRestored)
{
    CheckThenSwapTheAssetManager();
}

TEST_F(ProjectIsolationSuiteProbe, BSwapsTheAssetManagerAfterCheckingTheSuitesWasRestored)
{
    CheckThenSwapTheAssetManager();
}

// A test's asset manager writes its registry to whichever project is active
// when it is destroyed. The restore must destroy it while the test's own
// project is still active, or the registry lands in the project being put
// back -- in a real suite, a checked-in project's AssetRegistry.oar. The suite
// project here starts with no registry, and nothing of the suite's writes one
// before TearDownTestSuite, so a registry there is a test's.
class ProjectIsolationRegistryProbe : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        s_Directory = OloEngine::Tests::TempRoot() / "ProjectIsolationRegistryProbe";
        std::error_code ec;
        std::filesystem::remove_all(s_Directory, ec);
        std::filesystem::create_directories(s_Directory, ec);
        ASSERT_FALSE(ec) << "cannot create " << s_Directory.string() << ": " << ec.message();
        s_Project = Project::NewInMemory(s_Directory, ProjectConfig{});
        s_AssetManager = Ref<EditorAssetManager>::Create();
        Project::SetAssetManager(s_AssetManager);
        s_Switched = false;
    }
    static void TearDownTestSuite()
    {
        Project::Unload();
        s_AssetManager = nullptr;
        s_Project = nullptr;
    }

    static void CheckThenSwitchProjects()
    {
        if (s_Switched)
        {
            EXPECT_FALSE(std::filesystem::exists(s_Directory / "AssetRegistry.oar"))
                << "the previous test's asset manager wrote its registry into the restored project";
        }
        Project::NewInMemory(OloEngine::Tests::TempDir("registry-leak"), ProjectConfig{});
        Project::SetAssetManager(Ref<EditorAssetManager>::Create());
        s_Switched = true;
        ASSERT_NE(Project::GetActive().Raw(), s_Project.Raw());
    }

    inline static std::filesystem::path s_Directory;
    inline static Ref<Project> s_Project;
    inline static Ref<AssetManagerBase> s_AssetManager;
    inline static bool s_Switched = false;
};

TEST_F(ProjectIsolationRegistryProbe, ASwitchesProjectsAfterCheckingTheRestoredOneKeptItsRegistry)
{
    CheckThenSwitchProjects();
}

TEST_F(ProjectIsolationRegistryProbe, BSwitchesProjectsAfterCheckingTheRestoredOneKeptItsRegistry)
{
    CheckThenSwitchProjects();
}
