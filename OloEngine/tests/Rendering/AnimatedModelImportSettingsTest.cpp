// OLO_TEST_LAYER: unit
// =============================================================================
// AnimatedModelImportSettingsTest — the animated route's Assimp settings weld a
// normal-less source and give it shared smooth normals.
//
// The import contract behind AnimatedModelNormalsImportTest, without the GL
// context that AnimatedModel::LoadModel needs, so it runs on every CI runner.
// It reads the fixture with exactly AnimatedModel::ConfigureImporter and
// AnimatedModel::ImportFlags: with aiProcess_GenNormals in their place the grid
// arrives as 24 vertices with per-face normals.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "NormallessSkinnedMeshFixture.h"
#include "TestTempDir.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/AssimpSmoothNormals.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

namespace OloEngine::Tests
{
    TEST(AnimatedModelImportSettings, ANormallessRiggedSourceImportsWeldedWithSharedNormals)
    {
        const std::filesystem::path gltf = NormallessSkinnedGrid::Write(TempDir("grid"));

        Assimp::Importer importer;
        AnimatedModel::ConfigureImporter(importer);
        const aiScene* scene = ReadSceneWithSmoothNormals(importer, gltf.string(), AnimatedModel::ImportFlags());
        ASSERT_NE(scene, nullptr) << importer.GetErrorString();
        ASSERT_EQ(scene->mNumMeshes, 1u);

        const aiMesh* mesh = scene->mMeshes[0];
        ASSERT_TRUE(mesh->HasBones()) << "the fixture is rigged";
        EXPECT_EQ(mesh->mNumVertices, NormallessSkinnedGrid::kVertexCount)
            << "flat normals split the grid into " << NormallessSkinnedGrid::kTriangleCount * 3 << " vertices";
        ASSERT_TRUE(mesh->HasNormals());
        for (u32 i = 0; i < mesh->mNumVertices; ++i)
        {
            EXPECT_NEAR(mesh->mNormals[i].x, 0.0f, 1e-4f);
            EXPECT_NEAR(mesh->mNormals[i].y, 0.0f, 1e-4f);
            EXPECT_NEAR(mesh->mNormals[i].z, 1.0f, 1e-4f);
        }
    }

    // The curved case: each shared normal averages the faces around its vertex, so on a
    // unit sphere it points along the vertex's own position. A flat normal leans by up to
    // half a facet's angle (11 degrees across a 16-segment ring), and splits the mesh.
    TEST(AnimatedModelImportSettings, ANormallessRiggedSphereImportsWithRadialNormals)
    {
        const std::filesystem::path gltf = NormallessSkinnedSphere::Write(TempDir("sphere"));

        Assimp::Importer importer;
        AnimatedModel::ConfigureImporter(importer);
        const aiScene* scene = ReadSceneWithSmoothNormals(importer, gltf.string(), AnimatedModel::ImportFlags());
        ASSERT_NE(scene, nullptr) << importer.GetErrorString();
        ASSERT_EQ(scene->mNumMeshes, 1u);

        const aiMesh* mesh = scene->mMeshes[0];
        EXPECT_EQ(mesh->mNumVertices, NormallessSkinnedSphere::kVertexCount)
            << "flat normals split the sphere into " << NormallessSkinnedSphere::kTriangleCount * 3 << " vertices";
        ASSERT_TRUE(mesh->HasNormals());
        for (u32 i = 0; i < mesh->mNumVertices; ++i)
        {
            const aiVector3D& position = mesh->mVertices[i];
            const aiVector3D& normal = mesh->mNormals[i];
            EXPECT_GT(position * normal, 0.99f)
                << "vertex " << i << " at (" << position.x << ", " << position.y << ", " << position.z
                << ") has normal (" << normal.x << ", " << normal.y << ", " << normal.z << "), which is not radial";
        }
    }
} // namespace OloEngine::Tests
