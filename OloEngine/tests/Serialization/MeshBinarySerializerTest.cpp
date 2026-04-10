#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Serialization/MeshBinarySerializer.h"
#include "OloEngine/Serialization/MeshBinaryFormat.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// =============================================================================
// Helpers
// =============================================================================

static Ref<MeshSource> MakeSimpleMesh()
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));

    TArray<u32> indices;
    indices.Add(0);
    indices.Add(1);
    indices.Add(2);
    indices.Add(0);
    indices.Add(2);
    indices.Add(3);

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

    Submesh sub;
    sub.m_VertexCount = 4;
    sub.m_IndexCount = 6;
    sub.m_NodeName = "TestQuad";
    sub.m_MeshName = "Quad";
    mesh->GetSubmeshes().Add(sub);

    return mesh;
}

static Ref<MeshSource> MakeRiggedMesh()
{
    auto mesh = MakeSimpleMesh();

    // Add a minimal skeleton (2 bones)
    auto skeleton = Ref<Skeleton>::Create();
    skeleton->m_ParentIndices = { -1, 0 };
    skeleton->m_BoneNames = { "Root", "Child" };
    skeleton->m_LocalTransforms = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), { 0.0f, 1.0f, 0.0f }) };
    skeleton->m_GlobalTransforms = { glm::mat4(1.0f), skeleton->m_LocalTransforms[1] };
    skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
    skeleton->m_BindPoseMatrices = skeleton->m_GlobalTransforms;
    skeleton->m_InverseBindPoses = { glm::inverse(skeleton->m_GlobalTransforms[0]), glm::inverse(skeleton->m_GlobalTransforms[1]) };
    skeleton->m_BindPoseLocalTransforms = skeleton->m_LocalTransforms;
    skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };

    mesh->SetSkeleton(skeleton);

    // Bone influences (overwrite the pre-allocated ones)
    for (u32 i = 0; i < 4; ++i)
    {
        BoneInfluence bi;
        bi.m_BoneIDs[0] = 0;
        bi.m_BoneIDs[1] = 1;
        bi.m_Weights[0] = 0.5f;
        bi.m_Weights[1] = 0.5f;
        mesh->GetBoneInfluences()[static_cast<i32>(i)] = bi;
    }

    // Bone info
    mesh->GetBoneInfo().Add(BoneInfo(skeleton->m_InverseBindPoses[0], 0));
    mesh->GetBoneInfo().Add(BoneInfo(skeleton->m_InverseBindPoses[1], 1));

    mesh->GetSubmeshes()[0].m_IsRigged = true;

    return mesh;
}

static std::filesystem::path GetTestCacheDir()
{
    return std::filesystem::temp_directory_path() / "olo_test_cache";
}

static std::filesystem::path GetTestCachePath(const std::string& filename)
{
    auto dir = GetTestCacheDir();
    std::filesystem::create_directories(dir);
    return dir / filename;
}

// Fixture that ensures the temp directory is cleaned up after the suite,
// even when individual tests fail before their manual remove() calls.
class MeshBinarySerializerTest : public ::testing::Test
{
  protected:
    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(GetTestCacheDir(), ec);
    }
};

class AnimationBinarySerializerTest : public ::testing::Test
{
  protected:
    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(GetTestCacheDir(), ec);
    }
};

// =============================================================================
// MeshBinarySerializer Tests
// =============================================================================

TEST_F(MeshBinarySerializerTest, WriteAndReadStaticMesh)
{
    auto original = MakeSimpleMesh();
    auto path = GetTestCachePath("static_mesh.omesh");

    ASSERT_TRUE(MeshBinarySerializer::Write(path, *original, 12345));

    auto loaded = MeshBinarySerializer::Read(path);
    ASSERT_NE(loaded, nullptr);

    EXPECT_EQ(loaded->GetVertices().Num(), original->GetVertices().Num());
    EXPECT_EQ(loaded->GetIndices().Num(), original->GetIndices().Num());

    // Verify vertex position values survive round-trip
    for (i32 i = 0; i < original->GetVertices().Num(); ++i)
    {
        EXPECT_FLOAT_EQ(loaded->GetVertices()[i].Position.x, original->GetVertices()[i].Position.x);
        EXPECT_FLOAT_EQ(loaded->GetVertices()[i].Position.y, original->GetVertices()[i].Position.y);
        EXPECT_FLOAT_EQ(loaded->GetVertices()[i].Position.z, original->GetVertices()[i].Position.z);
    }

    // Verify submeshes
    EXPECT_EQ(loaded->GetSubmeshes().Num(), 1);
    EXPECT_EQ(loaded->GetSubmeshes()[0].m_NodeName, "TestQuad");
    EXPECT_EQ(loaded->GetSubmeshes()[0].m_MeshName, "Quad");
    EXPECT_EQ(loaded->GetSubmeshes()[0].m_VertexCount, 4u);
    EXPECT_EQ(loaded->GetSubmeshes()[0].m_IndexCount, 6u);

    std::filesystem::remove(path);
}

TEST_F(MeshBinarySerializerTest, ReadTimestampWorks)
{
    auto mesh = MakeSimpleMesh();
    auto path = GetTestCachePath("timestamp_test.omesh");

    ASSERT_TRUE(MeshBinarySerializer::Write(path, *mesh, 99887766));

    u64 ts = 0;
    ASSERT_TRUE(MeshBinarySerializer::ReadTimestamp(path, ts));
    EXPECT_EQ(ts, 99887766u);

    std::filesystem::remove(path);
}

TEST_F(MeshBinarySerializerTest, ReadTimestampFailsOnMissingFile)
{
    u64 ts = 0;
    EXPECT_FALSE(MeshBinarySerializer::ReadTimestamp("nonexistent_file.omesh", ts));
}

TEST_F(MeshBinarySerializerTest, WriteAndReadRiggedMesh)
{
    auto original = MakeRiggedMesh();
    auto path = GetTestCachePath("rigged_mesh.omesh");

    ASSERT_TRUE(MeshBinarySerializer::Write(path, *original, 0));

    auto loaded = MeshBinarySerializer::Read(path);
    ASSERT_NE(loaded, nullptr);

    // Geometry
    EXPECT_EQ(loaded->GetVertices().Num(), 4);
    EXPECT_EQ(loaded->GetIndices().Num(), 6);

    // Skeleton
    ASSERT_TRUE(loaded->HasSkeleton());
    const auto* skel = loaded->GetSkeleton();
    EXPECT_EQ(skel->m_BoneNames.size(), 2u);
    EXPECT_EQ(skel->m_BoneNames[0], "Root");
    EXPECT_EQ(skel->m_BoneNames[1], "Child");
    EXPECT_EQ(skel->m_ParentIndices[0], -1);
    EXPECT_EQ(skel->m_ParentIndices[1], 0);

    // Bone influences
    EXPECT_EQ(loaded->GetBoneInfluences().Num(), 4);
    EXPECT_FLOAT_EQ(loaded->GetBoneInfluences()[0].m_Weights[0], 0.5f);
    EXPECT_FLOAT_EQ(loaded->GetBoneInfluences()[0].m_Weights[1], 0.5f);

    // Bone info
    EXPECT_EQ(loaded->GetBoneInfo().Num(), 2);
    EXPECT_EQ(loaded->GetBoneInfo()[0].m_BoneIndex, 0u);
    EXPECT_EQ(loaded->GetBoneInfo()[1].m_BoneIndex, 1u);

    // Submesh rigging flag
    EXPECT_TRUE(loaded->GetSubmeshes()[0].m_IsRigged);

    std::filesystem::remove(path);
}

TEST_F(MeshBinarySerializerTest, ReadRejectsCorruptFile)
{
    auto path = GetTestCachePath("corrupt.omesh");

    // Write garbage data
    {
        std::ofstream out(path, std::ios::binary);
        const char garbage[] = "not a valid mesh file";
        out.write(garbage, sizeof(garbage));
    }

    auto loaded = MeshBinarySerializer::Read(path);
    EXPECT_EQ(loaded, nullptr);

    std::filesystem::remove(path);
}

// =============================================================================
// AnimationBinarySerializer Tests
// =============================================================================

static std::vector<Ref<AnimationClip>> MakeTestAnimations()
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "Walk";
    clip->Duration = 2.0f;

    BoneAnimation boneAnim;
    boneAnim.BoneName = "Root";
    boneAnim.PositionKeys.push_back({ 0.0, { 0.0f, 0.0f, 0.0f } });
    boneAnim.PositionKeys.push_back({ 1.0, { 1.0f, 0.0f, 0.0f } });
    boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.ScaleKeys.push_back({ 0.0, { 1.0f, 1.0f, 1.0f } });
    clip->BoneAnimations.push_back(boneAnim);

    MorphTargetKeyframe mk;
    mk.Time = 0.5;
    mk.TargetName = "MorphTarget_0";
    mk.Weight = 0.75f;
    clip->MorphKeyframes.push_back(mk);

    return { clip };
}

TEST_F(AnimationBinarySerializerTest, WriteAndReadRoundTrip)
{
    auto clips = MakeTestAnimations();
    auto path = GetTestCachePath("test_anim.oanim");

    ASSERT_TRUE(AnimationBinarySerializer::Write(path, clips, 55555));

    auto loaded = AnimationBinarySerializer::Read(path);
    ASSERT_EQ(loaded.size(), 1u);

    const auto& clip = loaded[0];
    EXPECT_EQ(clip->Name, "Walk");
    EXPECT_FLOAT_EQ(clip->Duration, 2.0f);

    // Bone animation
    ASSERT_EQ(clip->BoneAnimations.size(), 1u);
    const auto& rootAnim = clip->BoneAnimations[0];
    EXPECT_EQ(rootAnim.BoneName, "Root");
    EXPECT_EQ(rootAnim.PositionKeys.size(), 2u);
    EXPECT_FLOAT_EQ(rootAnim.PositionKeys[1].Position.x, 1.0f);
    EXPECT_EQ(rootAnim.RotationKeys.size(), 1u);
    EXPECT_EQ(rootAnim.ScaleKeys.size(), 1u);

    // Morph target keyframe
    ASSERT_EQ(clip->MorphKeyframes.size(), 1u);
    EXPECT_EQ(clip->MorphKeyframes[0].TargetName, "MorphTarget_0");
    EXPECT_FLOAT_EQ(clip->MorphKeyframes[0].Weight, 0.75f);

    std::filesystem::remove(path);
}

TEST_F(AnimationBinarySerializerTest, ReadTimestampWorks)
{
    auto clips = MakeTestAnimations();
    auto path = GetTestCachePath("ts_anim.oanim");

    ASSERT_TRUE(AnimationBinarySerializer::Write(path, clips, 77777));

    u64 ts = 0;
    ASSERT_TRUE(AnimationBinarySerializer::ReadTimestamp(path, ts));
    EXPECT_EQ(ts, 77777u);

    std::filesystem::remove(path);
}

TEST_F(AnimationBinarySerializerTest, ReadReturnsEmptyOnCorruptFile)
{
    auto path = GetTestCachePath("corrupt.oanim");

    {
        std::ofstream out(path, std::ios::binary);
        out.write("bad data", 8);
    }

    auto loaded = AnimationBinarySerializer::Read(path);
    EXPECT_TRUE(loaded.empty());

    std::filesystem::remove(path);
}
