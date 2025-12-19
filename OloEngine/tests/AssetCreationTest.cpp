#include <gtest/gtest.h>
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Asset/Asset.h"

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

// @brief Test AssetType enum values
TEST_F(AssetCreationTest, AssetType_Values)
{
    // Verify our asset type enum values match expected values
    EXPECT_EQ(static_cast<i32>(AssetType::MeshCollider), 17) << "MeshCollider should have AssetType value 17";
    EXPECT_EQ(static_cast<i32>(AssetType::ScriptFile), 15) << "ScriptFile should have AssetType value 15";
    EXPECT_EQ(static_cast<i32>(AssetType::Audio), 10) << "Audio should have AssetType value 10";
}
