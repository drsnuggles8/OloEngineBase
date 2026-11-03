// OLO_TEST_LAYER: unit
// =============================================================================
// AssimpSmoothNormalsTest — GenerateMissingSmoothNormals on an INDEXED mesh.
//
// aiProcess_GenSmoothNormals assumes one vertex per face corner. On an indexed
// mesh (what glTF2 delivers) it gives every shared vertex the normal of the last
// face that wrote it. GenerateMissingSmoothNormals works on the indexed mesh:
// the crease case is a cube whose 8 corners are shared by all 12 triangles. At
// a 66 degree limit no two faces at a corner smooth together, so each corner
// must split into three vertices, one per face direction, and each copy must
// carry its source's bone weight and morph-target position.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/AssimpSmoothNormals.h"

#include <assimp/mesh.h>

#include <array>
#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        // A cube from -1 to 1 with its 8 corners shared, outward-wound, one bone
        // weighting every vertex fully, and one morph target at twice the size.
        void BuildSharedCornerCube(aiMesh& mesh)
        {
            mesh.mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
            mesh.mNumVertices = 8;
            mesh.mVertices = new aiVector3D[8];
            for (u32 i = 0; i < 8; ++i)
            {
                mesh.mVertices[i] = aiVector3D((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            }

            // Each side as a quad of corner indices in order around it.
            constexpr std::array<std::array<u32, 4>, 6> sides = { { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                                                   { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } } };
            mesh.mNumFaces = 12;
            mesh.mFaces = new aiFace[12];
            u32 face = 0;
            for (const auto& side : sides)
            {
                for (const std::array<u32, 3> triangle : { std::array<u32, 3>{ side[0], side[1], side[2] },
                                                           std::array<u32, 3>{ side[0], side[2], side[3] } })
                {
                    std::array<u32, 3> wound = triangle;
                    const aiVector3D& a = mesh.mVertices[wound[0]];
                    const aiVector3D normal = (mesh.mVertices[wound[1]] - a) ^ (mesh.mVertices[wound[2]] - a);
                    const aiVector3D centre = (a + mesh.mVertices[wound[1]] + mesh.mVertices[wound[2]]) / 3.0f;
                    if (normal * centre < 0.0f)
                        std::swap(wound[1], wound[2]);
                    mesh.mFaces[face].mNumIndices = 3;
                    mesh.mFaces[face].mIndices = new unsigned int[3]{ wound[0], wound[1], wound[2] };
                    ++face;
                }
            }

            mesh.mNumBones = 1;
            mesh.mBones = new aiBone*[1]{ new aiBone };
            mesh.mBones[0]->mNumWeights = 8;
            mesh.mBones[0]->mWeights = new aiVertexWeight[8];
            for (u32 i = 0; i < 8; ++i)
                mesh.mBones[0]->mWeights[i] = aiVertexWeight(i, 1.0f);

            mesh.mNumAnimMeshes = 1;
            mesh.mAnimMeshes = new aiAnimMesh*[1]{ new aiAnimMesh };
            mesh.mAnimMeshes[0]->mNumVertices = 8;
            mesh.mAnimMeshes[0]->mVertices = new aiVector3D[8];
            for (u32 i = 0; i < 8; ++i)
                mesh.mAnimMeshes[0]->mVertices[i] = mesh.mVertices[i] * 2.0f;
        }
    } // namespace

    TEST(AssimpSmoothNormals, ACreaseSplitsASharedVertexAndItsSkinAndMorphData)
    {
        aiMesh mesh;
        BuildSharedCornerCube(mesh);

        GenerateMissingSmoothNormals(mesh, 66.0f);

        ASSERT_TRUE(mesh.HasNormals());
        ASSERT_EQ(mesh.mNumVertices, 24u) << "each corner needs one vertex per face direction";
        for (u32 f = 0; f < mesh.mNumFaces; ++f)
        {
            const aiFace& face = mesh.mFaces[f];
            const aiVector3D& a = mesh.mVertices[face.mIndices[0]];
            const aiVector3D faceNormal =
                ((mesh.mVertices[face.mIndices[1]] - a) ^ (mesh.mVertices[face.mIndices[2]] - a)).Normalize();
            for (u32 k = 0; k < 3; ++k)
            {
                const aiVector3D& normal = mesh.mNormals[face.mIndices[k]];
                EXPECT_GT(normal * faceNormal, 0.9999f) << "face " << f << " corner " << k << " is not flat across the crease";
            }
        }

        const aiBone& bone = *mesh.mBones[0];
        ASSERT_EQ(bone.mNumWeights, 24u) << "every copy keeps its source's bone weight";
        std::array<u32, 24> weightedTimes{};
        for (u32 w = 0; w < bone.mNumWeights; ++w)
        {
            ASSERT_LT(bone.mWeights[w].mVertexId, 24u);
            ++weightedTimes[bone.mWeights[w].mVertexId];
            EXPECT_FLOAT_EQ(bone.mWeights[w].mWeight, 1.0f);
        }
        for (u32 v = 0; v < 24; ++v)
            EXPECT_EQ(weightedTimes[v], 1u) << "vertex " << v;

        const aiAnimMesh& target = *mesh.mAnimMeshes[0];
        ASSERT_EQ(target.mNumVertices, 24u);
        for (u32 v = 0; v < 24; ++v)
        {
            const aiVector3D expected = mesh.mVertices[v] * 2.0f;
            EXPECT_FLOAT_EQ(target.mVertices[v].x, expected.x) << "vertex " << v;
            EXPECT_FLOAT_EQ(target.mVertices[v].y, expected.y) << "vertex " << v;
            EXPECT_FLOAT_EQ(target.mVertices[v].z, expected.z) << "vertex " << v;
        }
    }

    TEST(AssimpSmoothNormals, AWideLimitSmoothsTheSharedCornersWithoutSplitting)
    {
        aiMesh mesh;
        BuildSharedCornerCube(mesh);

        GenerateMissingSmoothNormals(mesh, 180.0f);

        ASSERT_TRUE(mesh.HasNormals());
        ASSERT_EQ(mesh.mNumVertices, 8u);
        // Not exactly radial: a side contributes once per triangle touching the corner,
        // one or two depending on its diagonal, as in Assimp. Still within ~20 degrees.
        for (u32 v = 0; v < 8; ++v)
        {
            const aiVector3D radial = aiVector3D(mesh.mVertices[v]).Normalize();
            EXPECT_GT(mesh.mNormals[v] * radial, 0.94f) << "vertex " << v << " should point out of its corner";
        }
    }

    TEST(AssimpSmoothNormals, AMeshThatHasNormalsIsLeftAlone)
    {
        aiMesh mesh;
        BuildSharedCornerCube(mesh);
        mesh.mNormals = new aiVector3D[8];
        for (u32 v = 0; v < 8; ++v)
            mesh.mNormals[v] = aiVector3D(0.0f, 1.0f, 0.0f);

        GenerateMissingSmoothNormals(mesh, 66.0f);

        ASSERT_EQ(mesh.mNumVertices, 8u);
        for (u32 v = 0; v < 8; ++v)
            EXPECT_FLOAT_EQ(mesh.mNormals[v].y, 1.0f);
    }
} // namespace OloEngine::Tests
