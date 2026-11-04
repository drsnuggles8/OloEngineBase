// OLO_TEST_LAYER: meta
//
// ProjectIsolation.h: a test that leaves a project and an asset manager active
// must not hand them to the test after it. Two probes in one suite, run in
// declaration order: the first leaves both behind on purpose, the second
// checks it found what the first found. FoxCoatEvidenceTest did the first half
// for real and kept its fox assets resident for the rest of the AMD nightly.
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
} // namespace

TEST(ProjectIsolationProbe, ALeavesAProjectAndAnAssetManagerActive)
{
    ProbeRecord& record = Record();
    record.HadProject = Project::GetActive() != nullptr;
    record.HadAssetManager = Project::HasAssetManager();
    record.LeakedDirectory = OloEngine::Tests::TempDir("leaked-project");

    Project::NewInMemory(record.LeakedDirectory, ProjectConfig{});
    Project::SetAssetManager(Ref<EditorAssetManager>::Create());
    record.Ran = true;
    ASSERT_TRUE(Project::GetActive());
    ASSERT_TRUE(Project::HasAssetManager());
}

TEST(ProjectIsolationProbe, BFindsTheStateTheLeakingTestFound)
{
    const ProbeRecord& record = Record();
    if (!record.Ran)
    {
        GTEST_SKIP() << "checks the state ALeavesAProjectAndAnAssetManagerActive left; run them together";
    }
    EXPECT_EQ(Project::GetActive() != nullptr, record.HadProject);
    EXPECT_EQ(Project::HasAssetManager(), record.HadAssetManager);
    if (Project::GetActive())
    {
        EXPECT_NE(Project::GetActive()->GetDirectory(), record.LeakedDirectory)
            << "the previous test's project is still active";
    }
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
    }
    static void TearDownTestSuite()
    {
        Project::Unload();
        s_AssetManager = nullptr;
        s_Project = nullptr;
    }

    inline static Ref<Project> s_Project;
    inline static Ref<AssetManagerBase> s_AssetManager;
};

TEST_F(ProjectIsolationSuiteProbe, ASwapsOnlyTheAssetManager)
{
    Project::SetAssetManager(Ref<EditorAssetManager>::Create());
    ASSERT_NE(Project::GetAssetManager().Raw(), s_AssetManager.Raw());
}

TEST_F(ProjectIsolationSuiteProbe, BStillSeesTheSuitesOwnProjectAndAssetManager)
{
    EXPECT_EQ(Project::GetActive().Raw(), s_Project.Raw()) << "the suite's project was replaced by a copy";
    ASSERT_TRUE(Project::HasAssetManager());
    EXPECT_EQ(Project::GetAssetManager().Raw(), s_AssetManager.Raw());
}
