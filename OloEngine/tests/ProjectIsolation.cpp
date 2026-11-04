#include "OloEnginePCH.h"
#include "ProjectIsolation.h"

#include "OloEngine/Asset/AssetManager/AssetManagerBase.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Project/Project.h"

#include <gtest/gtest.h>

#include <string>

namespace OloEngine::Tests::ProjectIsolation
{
    namespace
    {
        struct Snapshot
        {
            Ref<Project> ActiveProject;
            Ref<AssetManagerBase> AssetManager;
        };

        [[nodiscard]] Snapshot Capture()
        {
            return { Project::GetActive(), Project::HasAssetManager() ? Project::GetAssetManager() : nullptr };
        }

        [[nodiscard]] bool Unchanged(const Snapshot& before)
        {
            const Snapshot now = Capture();
            return now.ActiveProject.Raw() == before.ActiveProject.Raw() &&
                   now.AssetManager.Raw() == before.AssetManager.Raw();
        }

        // Back to exactly `before`: the captured objects themselves, so a suite
        // that holds the project it set up in SetUpTestSuite still sees that one
        // when a test swapped only the asset manager.
        //
        // The test's asset manager must die while the test's project is still
        // active: EditorAssetManager::Shutdown writes its registry to whichever
        // project is active then. So nothing here may hold it past the swap, or
        // its registry overwrites the restored project's AssetRegistry.oar.
        // RestoreActive swaps the asset manager before the project.
        void Restore(const Snapshot& before, const std::string& owner)
        {
            {
                const Snapshot left = Capture();
                OLO_CORE_INFO("[test isolation] {} left project '{}' active{}; restoring the {} it found", owner,
                              left.ActiveProject ? left.ActiveProject->GetDirectory().string() : std::string("(none)"),
                              left.AssetManager ? " with its asset manager" : "",
                              before.ActiveProject ? "project" : "empty state");
            }
            // Drops the test's asset manager, and with it every asset only it held.
            Project::RestoreActive(before.ActiveProject, before.AssetManager);
        }

        class ProjectIsolationListener final : public ::testing::EmptyTestEventListener
        {
            void OnTestSuiteStart(const ::testing::TestSuite& /*suite*/) override
            {
                m_Suite = Capture();
            }
            void OnTestSuiteEnd(const ::testing::TestSuite& suite) override
            {
                if (!Unchanged(m_Suite))
                {
                    Restore(m_Suite, std::string("suite ") + suite.name());
                }
                m_Suite = {};
            }
            void OnTestStart(const ::testing::TestInfo& /*info*/) override
            {
                m_Test = Capture();
            }
            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                if (!Unchanged(m_Test))
                {
                    Restore(m_Test, std::string(info.test_suite_name()) + "." + info.name());
                }
                m_Test = {};
            }

            Snapshot m_Suite;
            Snapshot m_Test;
        };
    } // namespace

    void RegisterListener()
    {
        ::testing::UnitTest::GetInstance()->listeners().Append(new ProjectIsolationListener());
    }
} // namespace OloEngine::Tests::ProjectIsolation
