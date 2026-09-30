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

#include <memory>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

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
