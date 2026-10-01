#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace OloEngine;

// @brief Tests for asset creation and basic functionality
class AssetCreationTest : public ::testing::Test
{
};

// @brief Test MeshColliderAsset creation and property access
TEST_F(AssetCreationTest, MeshColliderAsset_Creation)
{
    // Test asset creation
    auto meshCollider = Ref<MeshColliderAsset>::Create();
    EXPECT_NE(meshCollider, nullptr) << "MeshColliderAsset should be created successfully";

    // Test property assignment
    meshCollider->m_ColliderMesh = 12345;
    meshCollider->m_Material.SetStaticFriction(0.7f);
    meshCollider->m_Material.SetRestitution(0.3f);
    meshCollider->m_EnableVertexWelding = true;
    meshCollider->m_VertexWeldTolerance = 0.05f;
    meshCollider->m_CollisionComplexity = ECollisionComplexity::UseComplexAsSimple;
    meshCollider->m_ColliderScale = glm::vec3(2.0f, 1.5f, 3.0f);

    // Verify properties
    EXPECT_EQ(meshCollider->m_ColliderMesh, 12345) << "ColliderMesh should be set correctly";
    EXPECT_FLOAT_EQ(meshCollider->m_Material.GetStaticFriction(), 0.7f) << "Material friction should be set correctly";
    EXPECT_FLOAT_EQ(meshCollider->m_Material.GetRestitution(), 0.3f) << "Material restitution should be set correctly";
    EXPECT_TRUE(meshCollider->m_EnableVertexWelding) << "Vertex welding should be enabled";
    EXPECT_EQ(meshCollider->m_CollisionComplexity, ECollisionComplexity::UseComplexAsSimple) << "Collision complexity should be set correctly";
    EXPECT_EQ(meshCollider->m_ColliderScale.x, 2.0f) << "Collider scale X should be set correctly";
}

// @brief Test ScriptFileAsset creation and getter/setter methods
TEST_F(AssetCreationTest, ScriptFileAsset_Creation)
{
    // Test asset creation
    auto scriptAsset = Ref<ScriptFileAsset>::Create();
    EXPECT_NE(scriptAsset, nullptr) << "ScriptFileAsset should be created successfully";

    // Test property assignment via setters
    scriptAsset->SetClassNamespace("MyGame.Components");
    scriptAsset->SetClassName("PlayerController");

    // Verify properties via getters
    EXPECT_EQ(scriptAsset->GetClassNamespace(), "MyGame.Components") << "Namespace should be set correctly";
    EXPECT_EQ(scriptAsset->GetClassName(), "PlayerController") << "Class name should be set correctly";
}

// A registered script is C# source; ".cs" is the only extension that maps to
// ScriptFile. The loader used to parse it as YAML, so every script failed to
// load and a pack build logged an error per script and shipped none of them;
// and Serialize wrote YAML over the .cs file itself (found on #1392).
TEST_F(AssetCreationTest, ScriptFileAsset_LoadsFromCSharpSourceAndNeverOverwritesIt)
{
    namespace fs = std::filesystem;
    // Restores the active project on every exit path, ASSERTs included, so a
    // failure here cannot strand later tests on this temp project.
    struct RestoreProject
    {
        Ref<Project> Previous = Project::GetActive();
        ~RestoreProject()
        {
            if (Previous)
                Project::NewInMemory(Previous->GetDirectory(), Previous->GetConfig());
            else
                Project::Unload();
        }
    } const restoreProject;
    const fs::path root = OloEngine::Tests::TempDir("script-asset");
    fs::create_directories(root / "Assets/Scripts");
    ProjectConfig config;
    config.AssetDirectory = "Assets";
    Project::NewInMemory(root, config);

    const std::string source = "using OloEngine;\n"
                               "// class NotThisOne : Entity\n"
                               "/* class NorThis */\n"
                               "namespace Sandbox.Gameplay\n"
                               "{\n"
                               "    public sealed class PlayerController : Entity\n"
                               "    {\n"
                               "        private string m_Note = \"class Decoy\";\n"
                               "    }\n"
                               "}\n";
    const fs::path script = root / "Assets/Scripts/PlayerController.cs";
    std::ofstream(script, std::ios::binary) << source;

    AssetMetadata metadata;
    metadata.Handle = 42;
    metadata.Type = AssetType::ScriptFile;
    metadata.FilePath = "Assets/Scripts/PlayerController.cs";

    ScriptFileSerializer serializer;
    Ref<Asset> asset;
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    const Ref<ScriptFileAsset> scriptAsset = asset.As<ScriptFileAsset>();
    ASSERT_TRUE(scriptAsset);
    EXPECT_EQ(scriptAsset->GetClassNamespace(), "Sandbox.Gameplay");
    EXPECT_EQ(scriptAsset->GetClassName(), "PlayerController");
    EXPECT_EQ(static_cast<u64>(scriptAsset->GetHandle()), 42u);

    // File-scoped namespace, the C# 10 spelling.
    std::ofstream(script, std::ios::binary) << "namespace Sandbox;\npublic class Door : Entity { }\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetClassNamespace(), "Sandbox");
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetClassName(), "Door");

    // Saving the asset must leave the source exactly as it was.
    const std::string before = "namespace Sandbox;\npublic class Door : Entity { }\n";
    serializer.Serialize(metadata, asset);
    std::ifstream in(script, std::ios::binary);
    const std::string after{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    EXPECT_EQ(after, before) << "Serialize overwrote the C# source";

    // The class named like the file wins over a helper declared before it, and
    // an apostrophe in a preprocessor line does not open a character literal.
    std::ofstream(script, std::ios::binary) << "#region Player's tuning\n"
                                               "namespace Sandbox\n{\n"
                                               "    internal class Tuning { }\n"
                                               "    public class PlayerController : Entity\n"
                                               "    {\n"
                                               "        string m_Path = @$\"C:\\{m_Name}\";\n"
                                               "    }\n}\n"
                                               "#endregion\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetClassName(), "PlayerController");
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetClassNamespace(), "Sandbox");

    // The namespace is the one the chosen class is declared in, not the file's first.
    std::ofstream(script, std::ios::binary) << "namespace Helpers { class Tuning { } }\n"
                                               "namespace Sandbox.Gameplay { public class PlayerController : Entity { } }\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetFullyQualifiedClassName(), "Sandbox.Gameplay.PlayerController");

    // A class after a closed namespace block is in the global namespace...
    std::ofstream(script, std::ios::binary) << "namespace Helpers { class Tuning { } }\n"
                                               "public class PlayerController : Entity { }\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetFullyQualifiedClassName(), "PlayerController");

    // "class" as a generic constraint opens no declaration and still lets its
    // brace open a scope.
    std::ofstream(script, std::ios::binary) << "namespace Sandbox { class Pool<T> where T : class { }\n"
                                               "    public class PlayerController : Entity { } }\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetFullyQualifiedClassName(), "Sandbox.PlayerController");

    // ...and nested blocks join, outermost first.
    std::ofstream(script, std::ios::binary) << "namespace Sandbox{namespace Gameplay{\n"
                                               "public class PlayerController:Entity{}\n}}\n";
    ASSERT_TRUE(serializer.TryLoadData(metadata, asset));
    EXPECT_EQ(asset.As<ScriptFileAsset>()->GetFullyQualifiedClassName(), "Sandbox.Gameplay.PlayerController");
}

// @brief Test ColliderMaterial structure
TEST_F(AssetCreationTest, ColliderMaterial_Basic)
{
    ColliderMaterial material;

    // Test default values
    EXPECT_FLOAT_EQ(material.GetStaticFriction(), 0.6f) << "Default static friction should be 0.6";
    EXPECT_FLOAT_EQ(material.GetRestitution(), 0.0f) << "Default restitution should be 0.0";

    // Test assignment through setters
    material.SetStaticFriction(0.8f);
    material.SetRestitution(0.2f);

    EXPECT_FLOAT_EQ(material.GetStaticFriction(), 0.8f) << "Static friction should be assignable";
    EXPECT_FLOAT_EQ(material.GetRestitution(), 0.2f) << "Restitution should be assignable";
}

// AssetType_Values retired -- the enum's integer values aren't an on-disk
// contract (we serialize by name via AssetExtensions and the registry),
// so pinning specific integer values fires on any reorder while
// surfacing zero real bugs. See docs/testing.md section 4.6.
