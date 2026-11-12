// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// An asset manager's Shutdown() releases its reference on the process-wide
// placeholder set. Both managers' destructors call Shutdown(), so a caller that
// shuts a manager down explicitly first (AssetPackBuilder does, for its temporary
// EditorAssetManager) used to release that reference twice — and with another
// manager alive, the second release tore the shared placeholder set down under
// it: every later placeholder lookup answered null.

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <memory>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

#if OLO_ASYNC_ASSETS
namespace
{
    // Counts the reloads the manager is asked for instead of doing them.
    class FCountingEditorAssetManager final : public EditorAssetManager
    {
      public:
        std::atomic<int> Reloads{ 0 };

        [[nodiscard]] bool ReloadData(AssetHandle /*assetHandle*/) override
        {
            Reloads.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    };

    // Stands in for Application::Run's per-frame drain of the game-thread queue.
    void PumpGameThreadQueue()
    {
        Tasks::FNamedThreadManager::Get().GetQueue(Tasks::ENamedThread::GameThread).ProcessAll(true);
    }
} // namespace
#endif // OLO_ASYNC_ASSETS

TEST(AssetManagerShutdown, ExplicitRuntimeShutdownReleasesThePlaceholderSetOnce)
{
    RuntimeAssetManager survivor(/*autoLoadDefaultPack=*/false);
    {
        auto transient = std::make_unique<RuntimeAssetManager>(/*autoLoadDefaultPack=*/false);
        transient->Shutdown();
    } // the destructor's Shutdown() must be a no-op

    EXPECT_TRUE(PlaceholderAssetManager::GetPlaceholderAsset(AssetType::Terrain))
        << "a manager that is still alive lost the placeholder set";
}

TEST(AssetManagerShutdown, ExplicitEditorShutdownReleasesThePlaceholderSetOnce)
{
    RuntimeAssetManager survivor(/*autoLoadDefaultPack=*/false);
    {
        // AssetPackBuilder's pattern: a temporary editor manager, shut down explicitly.
        auto transient = Ref<EditorAssetManager>::Create();
        transient->Shutdown();
    }

    EXPECT_TRUE(PlaceholderAssetManager::GetPlaceholderAsset(AssetType::Terrain))
        << "a manager that is still alive lost the placeholder set";
}

#if OLO_ASYNC_ASSETS
// ReloadDataAsync queues the reload to the game thread (with OLO_ASYNC_ASSETS
// off it reloads at once, and these tests have nothing to check). The queue outlives the
// manager (Open Project replaces it, ~Application destroys it), so what is
// still queued when the manager shuts down must neither be waited for nor run.

TEST(AssetManagerShutdown, AQueuedReloadRunsWhileTheManagerIsOpen)
{
    auto manager = Ref<FCountingEditorAssetManager>::Create();
    manager->ReloadDataAsync(AssetHandle(42));
    EXPECT_EQ(manager->Reloads.load(), 0) << "the reload is deferred to the game thread's queue";

    PumpGameThreadQueue();
    EXPECT_EQ(manager->Reloads.load(), 1);
}

// Shutdown() used to wait until every queued reload had run. It runs on the
// game thread, the only thread that drains the queue, so with one reload queued
// it never returned: the editor hung on Open Project or on exit.
TEST(AssetManagerShutdown, ShutdownDoesNotWaitForAReloadOnlyItsOwnThreadCanRun)
{
    auto manager = Ref<FCountingEditorAssetManager>::Create();
    manager->ReloadDataAsync(AssetHandle(42));
    manager->Shutdown();

    PumpGameThreadQueue();
    EXPECT_EQ(manager->Reloads.load(), 0) << "a manager that was shut down was still asked to reload";
}

// The queued task captured the manager by raw pointer. Run after the manager
// was destroyed, it called into freed memory (AddressSanitizer reports it).
TEST(AssetManagerShutdown, AQueuedReloadDoesNotTouchADestroyedManager)
{
    {
        auto manager = Ref<FCountingEditorAssetManager>::Create();
        manager->ReloadDataAsync(AssetHandle(42));
    }
    PumpGameThreadQueue();
    SUCCEED();
}
#endif // OLO_ASYNC_ASSETS
