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
