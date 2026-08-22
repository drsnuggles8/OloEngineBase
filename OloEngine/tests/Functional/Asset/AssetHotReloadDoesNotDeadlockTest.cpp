#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// AssetHotReloadDoesNotDeadlockTest — Functional Test.
//
// Cross-subsystem seam under test:
//   filewatch hot-reload → EditorAssetManager::ReloadData → AssetRegistry
//   metadata refresh → SerializeAssetRegistry. That tail is where the editor
//   wedged twice in one day (issues #439 and #863): ReloadData held the
//   non-recursive m_RegistryMutex, SerializeAssetRegistry took it again, and
//   the game thread parked in ParkingLot::Wait forever — no assert, no log
//   line, no CPU. Both reports were live-editor findings; nothing headless had
//   ever called ReloadData, which is why the same line could be rediscovered
//   through a second trigger after the first fix shipped.
//
// This is the missing headless coverage, and it asserts two things that a
// careless fix breaks in opposite directions:
//
//   1. ReloadData RETURNS — it does not park the calling thread.
//   2. Hot-reload still HAPPENS — the new file contents are actually live
//      afterwards. Silently dropping the reload also makes (1) pass.
//
// Reaching the deadlocking tail at all needs an asset that is both tracked (in
// the registry) and successfully loaded; miss either and ReloadData returns
// early, before the line under test, and the case asserts nothing while looking
// green. The probe asset below is authored by the test so both hold by
// construction, and so the reload is observable rather than merely successful.
//
// A ScriptFile is used on purpose: ScriptFileSerializer is CPU-only (YAML text
// → ScriptFileAsset, no GPU resources), so the whole path runs with no GL
// context and this is a normal CI citizen rather than one more workstation-only
// SKIP.
//
// On the liveness assertion: the regression this guards is a PERMANENT park,
// not a slow path, so a generous upper bound separates pass from fail with no
// sensitivity at either end (a healthy reload of a 3-line file is sub-
// millisecond). ReloadData runs on a worker only so that a regression fails
// THIS case instead of hanging the whole suite — the lock discipline under test
// is a property of ReloadData itself and is thread-independent, so nothing
// about the assertion depends on which thread makes the call.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Comfortably above a healthy reload of a 3-line text asset, comfortably
    // below any CI job timeout. The failure it detects never completes at all.
    constexpr auto kReloadLivenessBudget = std::chrono::seconds(30);

    // .cs maps to AssetType::ScriptFile; ScriptFileSerializer reads the file as
    // YAML and wants a `ScriptFile` node, so the probe carries one. Authored by
    // the test rather than staged from SandboxProject, so the reload's effect is
    // something this test chose and can therefore assert on.
    constexpr const char* kProbeRelativePath = "Assets/Scripts/Source/HotReloadProbe.cs";

    std::string ProbeContents(const std::string& className)
    {
        return "ScriptFile:\n  ClassNamespace: Sandbox\n  ClassName: " + className + "\n";
    }

    void WriteProbe(const std::filesystem::path& path, const std::string& className)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << "could not write the probe asset at " << path.string();
        out << ProbeContents(className);
    }
} // namespace

class AssetHotReloadDoesNotDeadlockTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // No staged SandboxProject assets — the probe is authored below, after
        // the temp project directory exists.
        EnableAssetManager({});
    }
};

TEST_F(AssetHotReloadDoesNotDeadlockTest, ReloadDataOfALoadedAssetCompletesAndPublishesTheNewContents)
{
    ASSERT_TRUE(Project::GetActive())
        << "Project::Load failed — no active project after EnableAssetManager.";

    auto manager = Project::GetAssetManager().As<EditorAssetManager>();
    ASSERT_TRUE(manager)
        << "Project::GetAssetManager returned null or non-EditorAssetManager — "
           "harness wiring is broken.";

    const auto probePath = StagedAssetAbsolutePath(kProbeRelativePath);
    WriteProbe(probePath, "BeforeReload");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    // Tracked: without a registry entry ReloadData bails on invalid metadata.
    const AssetHandle handle = manager->ImportAsset(probePath);
    ASSERT_NE(static_cast<u64>(handle), 0ULL)
        << "ImportAsset returned a zero handle for " << kProbeRelativePath
        << " — the .cs extension is no longer mapped to ScriptFile, so ReloadData "
           "would return early and this case would assert nothing.";

    // Loaded: without a cached entry the reload takes the placeholder branch and
    // returns before the registry-persist tail under test.
    auto original = manager->GetAsset(handle).As<ScriptFileAsset>();
    ASSERT_TRUE(original)
        << "GetAsset returned null (or a non-ScriptFileAsset) for the probe — "
           "ScriptFileSerializer failed to load a CPU-only asset.";
    ASSERT_TRUE(manager->IsAssetLoaded(handle))
        << "the probe did not land in the loaded-asset cache.";
    ASSERT_EQ(original->GetClassName(), "BeforeReload")
        << "the probe did not round-trip its own contents, so the post-reload "
           "comparison below would prove nothing.";

    // What filewatch sees: the file changed on disk. ReloadData is exactly what
    // its Reload decision calls, via ReloadDataAsync onto the game thread.
    WriteProbe(probePath, "AfterReload");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    // Everything the worker touches is captured BY VALUE through shared state:
    // on a regression the thread parks forever and outlives this stack frame, so
    // a reference capture would dangle on exactly the failure path.
    auto reloadResult = std::make_shared<std::promise<bool>>();
    auto reloaded = reloadResult->get_future();
    // `mutable` because Ref<T> propagates constness: a by-value capture in a
    // non-mutable lambda is const, and ReloadData is not a const member.
    std::thread worker([manager, handle, reloadResult]() mutable
                       { reloadResult->set_value(manager->ReloadData(handle)); });
    // Detached for the same reason: a thread parked holding the registry lock can
    // never be joined. The process still exits cleanly.
    worker.detach();

    ASSERT_EQ(reloaded.wait_for(kReloadLivenessBudget), std::future_status::ready)
        << "EditorAssetManager::ReloadData did not return within "
        << kReloadLivenessBudget.count()
        << "s — the caller is parked, not slow. This is the issue #439 / #863 "
           "deadlock: something on ReloadData's path acquired the non-recursive "
           "m_RegistryMutex twice on one thread. Look for a call to "
           "SerializeAssetRegistry — or any other m_RegistryMutex acquisition — "
           "made while that lock is already held. See "
           "docs/agent-rules/non-recursive-lock-self-locking-helper.md.";

    EXPECT_TRUE(reloaded.get())
        << "ReloadData reported failure for a tracked, loaded, existing asset — "
           "hot-reload is broken in a different way than the deadlock this test "
           "guards.";

    // The reload actually took effect. A fix that quietly drops filewatch events
    // (or reloads into a discarded object) satisfies the liveness assertion above
    // and fails here, which is the point of asserting both.
    ASSERT_TRUE(manager->IsAssetLoaded(handle))
        << "ReloadData returned true but the asset is no longer cached.";
    auto refreshed = manager->GetAsset(handle).As<ScriptFileAsset>();
    ASSERT_TRUE(refreshed) << "the reloaded asset is missing or the wrong type.";
    EXPECT_EQ(refreshed->GetClassName(), "AfterReload")
        << "the cached asset still carries the pre-edit contents — the reload "
           "returned success without publishing the new data.";

    EXPECT_TRUE(manager->GetMetadata(handle).IsValid())
        << "the registry lost the probe's metadata across a reload.";
}
