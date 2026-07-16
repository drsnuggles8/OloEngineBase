#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Serialization/MeshBinarySerializer.h"
#include "OloEngine/Serialization/MeshBinaryFormat.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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
    sub.m_BoundingBox = BoundingBox({ 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f });
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
    // Per-process directory: under `ctest -j` every gtest case runs as its own
    // OloEngine-Tests.exe process, so a shared fixed dir would let one process's
    // TearDownTestSuite remove_all() wipe a concurrent process's cache files
    // mid-test. Keying by PID isolates each process. (Matches the PID-keyed
    // pattern in ShaderBinaryCacheRoundTripTest / RuntimeAssetManagerTest.)
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(::getpid());
#endif
    return std::filesystem::temp_directory_path() / ("olo_test_cache_" + std::to_string(pid));
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

    // Bounding box survives the round-trip — the authored submesh supplied
    // a (0,0,0)..(1,1,0) box; if the serializer drops or truncates the box
    // fields the engine would end up frustum-culling valid geometry.
    const auto& originalBox = original->GetSubmeshes()[0].m_BoundingBox;
    const auto& loadedBox = loaded->GetSubmeshes()[0].m_BoundingBox;
    EXPECT_FLOAT_EQ(loadedBox.Min.x, originalBox.Min.x);
    EXPECT_FLOAT_EQ(loadedBox.Min.y, originalBox.Min.y);
    EXPECT_FLOAT_EQ(loadedBox.Min.z, originalBox.Min.z);
    EXPECT_FLOAT_EQ(loadedBox.Max.x, originalBox.Max.x);
    EXPECT_FLOAT_EQ(loadedBox.Max.y, originalBox.Max.y);
    EXPECT_FLOAT_EQ(loadedBox.Max.z, originalBox.Max.z);

    std::filesystem::remove(path);
}

// v2 cook slice (issue #629): a cooked OVGM blob attached to the MeshSource
// must survive the .omesh round-trip byte-exactly through the appended
// VirtualMesh section. (OVGM semantic validation is VirtualMeshSerializer's
// own test surface — this section carries the bytes verbatim.)
TEST_F(MeshBinarySerializerTest, VirtualMeshBlobRoundTripsThroughV2Section)
{
    auto original = MakeSimpleMesh();
    std::vector<u8> blob(4097);
    for (sizet i = 0; i < blob.size(); ++i)
    {
        blob[i] = static_cast<u8>((i * 31u + 7u) & 0xFFu);
    }
    original->SetVirtualMeshBlob(blob);

    auto path = GetTestCachePath("virtual_mesh_blob.omesh");
    ASSERT_TRUE(MeshBinarySerializer::Write(path, *original, 424242));

    auto loaded = MeshBinarySerializer::Read(path);
    ASSERT_NE(loaded, nullptr);
    ASSERT_TRUE(loaded->HasVirtualMeshBlob());
    ASSERT_EQ(loaded->GetVirtualMeshBlob().size(), blob.size());
    EXPECT_EQ(loaded->GetVirtualMeshBlob(), blob);

    // The geometry still round-trips alongside the new section
    EXPECT_EQ(loaded->GetVertices().Num(), original->GetVertices().Num());
    EXPECT_EQ(loaded->GetIndices().Num(), original->GetIndices().Num());

    std::filesystem::remove(path);
}

// v4 slice (issue #629): the materials the mesh was IMPORTED with must ride along in
// the cache. Without them, Model::LoadModel had to re-run a full Assimp import of the
// source file on every warm load purely to rebuild materials — and MeshSourceSerializer,
// which reads a MeshSource, had no materials to hand the asset pack, so a shipped game
// rendered every mesh flat grey.
//
// Textureless materials here on purpose: this pins the non-GPU half of the table
// (factors / alpha mode / cutoff / flags / per-submesh index mapping) so it runs on
// headless CI too. The texture-handle half needs a GL context and lives in
// Rendering/PropertyTests/ImportedMaterialPackTest.cpp.
TEST_F(MeshBinarySerializerTest, ImportedMaterialsRoundTripThroughV4Section)
{
    auto original = MakeSimpleMesh();

    auto stone = Material::CreatePBR("Stone", glm::vec3(0.6f, 0.5f, 0.4f), 0.25f, 0.7f);
    stone->SetAlphaMode(AlphaMode::Mask);
    stone->SetAlphaCutoff(0.42f);
    stone->SetNormalScale(0.5f);
    stone->SetOcclusionStrength(0.25f);
    stone->SetFlag(MaterialFlag::TwoSided, true);
    auto glass = Material::CreatePBR("Glass", glm::vec3(0.9f, 0.95f, 1.0f), 0.0f, 0.05f);
    glass->SetAlphaMode(AlphaMode::Blend);
    original->SetImportedMaterials({ stone, glass });

    auto path = GetTestCachePath("imported_materials.omesh");
    ASSERT_TRUE(MeshBinarySerializer::Write(path, *original, 909090));

    auto loaded = MeshBinarySerializer::Read(path);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetImportedMaterials().size(), 2u);

    // The submesh MakeSimpleMesh authors has m_MaterialIndex 0 — resolve through the
    // accessor the renderer actually calls.
    auto resolved = loaded->GetImportedMaterialForSubmesh(0);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->GetName(), "Stone");
    EXPECT_EQ(resolved->GetAlphaMode(), AlphaMode::Mask);
    EXPECT_FLOAT_EQ(resolved->GetAlphaCutoff(), 0.42f);
    EXPECT_FLOAT_EQ(resolved->GetMetallicFactor(), 0.25f);
    EXPECT_FLOAT_EQ(resolved->GetRoughnessFactor(), 0.7f);
    EXPECT_FLOAT_EQ(resolved->GetNormalScale(), 0.5f);
    EXPECT_FLOAT_EQ(resolved->GetOcclusionStrength(), 0.25f);
    EXPECT_TRUE(resolved->GetFlag(MaterialFlag::TwoSided));
    EXPECT_FLOAT_EQ(resolved->GetBaseColorFactor().r, 0.6f);

    const auto& second = loaded->GetImportedMaterials()[1];
    ASSERT_TRUE(second);
    EXPECT_EQ(second->GetName(), "Glass");
    EXPECT_EQ(second->GetAlphaMode(), AlphaMode::Blend);
    EXPECT_FLOAT_EQ(second->GetRoughnessFactor(), 0.05f);

    // The geometry still round-trips alongside the new section.
    EXPECT_EQ(loaded->GetVertices().Num(), original->GetVertices().Num());
    EXPECT_EQ(loaded->GetSubmeshes().Num(), original->GetSubmeshes().Num());

    std::filesystem::remove(path);
}

// Version-range back-compat (docs/agent-rules/binary-format-versioning.md):
// a version-1 file — 7-entry section directory, no VirtualMesh section — must
// still load in the v2 reader. Simulated by patching the header version of a
// blob-less v2 file: the version word lives in the (unchecksummed) FileHeader,
// and the v1 read path then sizes the directory read to 7 entries.
TEST_F(MeshBinarySerializerTest, VersionOneFileStillLoadsWithoutVirtualMeshSection)
{
    auto original = MakeSimpleMesh();
    auto path = GetTestCachePath("v1_compat.omesh");
    ASSERT_TRUE(MeshBinarySerializer::Write(path, *original, 777));

    // Patch FileHeader::Version (u32 at byte offset 4) from 2 to 1.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(4);
        u32 const v1 = 1;
        file.write(reinterpret_cast<const char*>(&v1), sizeof(v1));
    }

    auto loaded = MeshBinarySerializer::Read(path);
    ASSERT_NE(loaded, nullptr) << "the v2 reader must accept a v1 file (range check + version-sized directory)";
    EXPECT_EQ(loaded->GetVertices().Num(), original->GetVertices().Num());
    EXPECT_EQ(loaded->GetIndices().Num(), original->GetIndices().Num());
    EXPECT_FALSE(loaded->HasVirtualMeshBlob());

    // A version NEWER than this build must be rejected, not misparsed.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(4);
        u32 const vFuture = OMeshFormat::CurrentVersion + 1;
        file.write(reinterpret_cast<const char*>(&vFuture), sizeof(vFuture));
    }
    EXPECT_EQ(MeshBinarySerializer::Read(path), nullptr);

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
