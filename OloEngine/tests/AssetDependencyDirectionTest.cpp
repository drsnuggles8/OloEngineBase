// OLO_TEST_LAYER: unit

// Which way round an asset dependency edge points.
//
// AssetManager::RegisterDependency(handle, dependency) is documented as "handle
// depends on dependency" ("a material (handle) depends on a texture
// (dependency)"), and Asset::OnDependencyUpdated is implemented only by
// DEPENDENTS -- MaterialAsset, StaticMesh, AnimationAsset. Every AssetSerializer
// call site nevertheless passed the pair the other way round, each one directly
// beneath a trace line stating the correct relation.
//
// It was invisible because a SECOND error cancelled it: the reload path called
// UpdateDependencies(handle), which walked the forward map (what `handle`
// depends on) and notified those. Two wrong directions composed into working
// hot-reload notification, an empty m_AssetDependents for every asset, and a
// GetDependencies() that answered backwards for anyone who asked -- which is
// where issue #1128 hit it, needing exactly that answer.
//
// Fixing either half alone breaks hot-reload, so both moved together. These
// cases pin each half separately so a future revert of one is caught here rather
// than as a texture that stops hot-reloading.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u64 kMaterial = 111111;
        constexpr u64 kTexture = 222222;
    } // namespace

    // The contract, straight off the manager. No serializer involved.
    TEST(AssetDependencyDirection, RegisterPutsTheDependentOnTheForwardEdge)
    {
        auto manager = Ref<EditorAssetManager>::Create();
        manager->RegisterDependency(AssetHandle(kMaterial), AssetHandle(kTexture));

        const auto materialDependsOn = manager->GetDependencies(AssetHandle(kMaterial));
        EXPECT_TRUE(materialDependsOn.contains(AssetHandle(kTexture)))
            << "RegisterDependency(handle, dependency) means `handle` DEPENDS ON `dependency`; "
               "GetDependencies(material) must therefore name the texture";

        const auto textureDependsOn = manager->GetDependencies(AssetHandle(kTexture));
        EXPECT_FALSE(textureDependsOn.contains(AssetHandle(kMaterial)))
            << "a texture does not depend on the material that uses it -- if this fails, the edge is reversed and "
               "every 'what does this asset need' answer is backwards";
    }

    // The call site. A material file naming a texture handle must register the
    // MATERIAL as the dependent.
    class MaterialDependencyDirection : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Project = TempDir("depdir");
            std::error_code ec;
            std::filesystem::create_directories(m_Project / "Assets" / "Materials", ec);

            {
                std::ofstream material(m_Project / "Assets" / "Materials" / "Test.olomaterial");
                material << "Material:\n"
                            "  Shader: DefaultPBR\n"
                            "  Textures:\n"
                            "    AlbedoMap: "
                         << kTexture << "\n";
            }
            {
                std::ofstream project(m_Project / "Test.oloproj");
                project << "Project:\n"
                           "  Name: DepDir\n"
                           "  StartScene: \"\"\n"
                           "  AssetDirectory: \"Assets\"\n"
                           "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(m_Project / "Test.oloproj"));
            auto manager = Ref<EditorAssetManager>::Create();
            manager->Initialize(false);
            Project::SetAssetManager(manager);
            m_Manager = manager;
        }

        // Process-global state, in a binary that runs many cases per process; see
        // the matching note in McpAutomationAssetCommandsTest. Unload() is the only
        // way back -- SetAssetManager asserts on null.
        void TearDown() override
        {
            m_Manager = nullptr;
            Project::Unload();
        }

        std::filesystem::path m_Project;
        Ref<EditorAssetManager> m_Manager;
    };

    TEST_F(MaterialDependencyDirection, AMaterialDependsOnItsTextureNotTheOtherWayRound)
    {
        AssetMetadata metadata;
        metadata.Handle = AssetHandle(kMaterial);
        metadata.Type = AssetType::Material;
        metadata.FilePath = "Assets/Materials/Test.olomaterial";

        MaterialAssetSerializer().RegisterDependencies(metadata);

        EXPECT_TRUE(m_Manager->GetDependencies(AssetHandle(kMaterial)).contains(AssetHandle(kTexture)))
            << "MaterialAssetSerializer must register the MATERIAL as depending on the texture it names";
        EXPECT_FALSE(m_Manager->GetDependencies(AssetHandle(kTexture)).contains(AssetHandle(kMaterial)))
            << "the reverse edge belongs in m_AssetDependents, which is what UpdateDependents walks to hot-reload "
               "the material when the texture changes";
    }
} // namespace OloEngine::Tests
