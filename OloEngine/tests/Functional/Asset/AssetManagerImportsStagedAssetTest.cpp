#include "OloEnginePCH.h"

// =============================================================================
// AssetManagerImportsStagedAssetTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Project::Load → Project::SetAssetManager → EditorAssetManager::Initialize
//   → AssetRegistry directory scan → ImportAsset → AssetMetadata lookup.
//   This is the entire on-disk-asset registration path, exercised end-to-end
//   without an editor process. Anything that breaks how the asset registry
//   discovers files (extension map, project-relative path resolution,
//   handle generation) shows up here as either a zero handle from
//   ImportAsset or a wrong AssetType / FilePath in the returned metadata.
//
// Scenario: stage Checkerboard.png (a real SandboxProject texture) into
// the harness's temp project. After EnableAssetManager runs:
//   * Project::GetActive() must be non-null and point at our temp dir.
//   * The registry's directory scan should have already auto-imported the
//     PNG (EditorAssetManager::Initialize → ScanDirectoryForAssets).
//   * A subsequent ImportAsset call on the same file should return the
//     SAME handle (idempotent) rather than minting a fresh one.
//   * GetAssetType(handle) must report Texture2D, matching the .png
//     extension map entry in AssetExtensions.cpp.
//
// We deliberately don't call GetAsset(handle) here — that would invoke
// AssetImporter::ImportAsset for the texture, which needs a live GL
// context. The registry/metadata layer is what Functional is asserting on.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetTypes.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AssetManagerImportsStagedAssetTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Stage one known SandboxProject asset. The path is relative to
        // OloEditor/SandboxProject/; EnableAssetManager copies it into the
        // throwaway project preserving the relative layout.
        EnableAssetManager({ "Assets/Textures/Checkerboard.png" });
    }
};

TEST_F(AssetManagerImportsStagedAssetTest, RegistryRecognizesStagedTextureAndImportIsIdempotent)
{
    ASSERT_TRUE(Project::GetActive())
        << "Project::Load failed — no active project after EnableAssetManager.";

    auto manager = Project::GetAssetManager().As<EditorAssetManager>();
    ASSERT_TRUE(manager)
        << "Project::GetAssetManager returned null or non-EditorAssetManager — "
           "harness wiring is broken.";

    // Absolute path inside the temp project for the staged file.
    const auto absolutePath = StagedAssetAbsolutePath("Assets/Textures/Checkerboard.png");
    ASSERT_TRUE(std::filesystem::exists(absolutePath))
        << "harness did not stage Checkerboard.png at " << absolutePath.string();

    // Initialize() scanned the asset directory and should have auto-imported
    // the texture. ImportAsset on the same file must therefore return the
    // EXISTING handle (idempotent path), not allocate a second.
    const AssetHandle firstImport = manager->ImportAsset(absolutePath);
    EXPECT_NE(static_cast<u64>(firstImport), 0ULL)
        << "ImportAsset returned a zero handle — directory scan + extension "
           "map disagree on whether .png is a registered asset type, OR the "
           "project-relative path resolution mangled the lookup key.";

    const AssetHandle secondImport = manager->ImportAsset(absolutePath);
    EXPECT_EQ(static_cast<u64>(firstImport), static_cast<u64>(secondImport))
        << "ImportAsset on the same file produced two different handles — "
           "AssetRegistry::GetHandleFromPath is failing to find the existing "
           "entry, so every Import duplicates the registry.";

    // Type lookup goes through the registry's stored AssetMetadata.
    EXPECT_EQ(manager->GetAssetType(firstImport), AssetType::Texture2D)
        << "Registry stored the wrong AssetType for a .png — "
           "EditorAssetManager::ImportAsset and AssetExtensions disagree on "
           "the extension map.";
}
