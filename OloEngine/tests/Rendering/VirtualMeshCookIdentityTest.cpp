// OLO_TEST_LAYER: unit
//
// Cook-identity guard for the virtualized-geometry blob (issue #629, finding C6).
//
// A cooked cluster LOD DAG rides inside the mesh's `.omesh` cache entry, whose
// validity is only (source path hash, flipUV prefix, source mtime). Nothing there
// notices that the BUILDER changed. And because the DAG bakes its own COPY of the
// vertex data, a stale cook is fully self-consistent: it passes every structural
// check in the deserializer and then quietly draws different geometry from the
// classic path — forever, until someone touches the source file or hand-deletes the
// cache. That happened, and it cost real time.
//
// So the blob header now carries a cook identity — kVirtualMeshBuilderVersion plus a
// fingerprint of the build config's defaults — and the reader rejects a mismatch,
// dropping the registry onto its runtime-build fallback. These tests pin that:
// a blob whose identity words are tampered with must NOT load.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include <cstring>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Header word layout (OVGM): magic, version, builderVersion, configFingerprint, ...
        constexpr sizet kBuilderVersionOffset = 2 * sizeof(u32);
        constexpr sizet kConfigFingerprintOffset = 3 * sizeof(u32);

        Ref<MeshSource> MakeGridMesh(u32 quadsPerSide)
        {
            TArray<Vertex> vertices;
            TArray<u32> indices;
            u32 const verts = quadsPerSide + 1;
            for (u32 y = 0; y < verts; ++y)
            {
                for (u32 x = 0; x < verts; ++x)
                {
                    f32 const u = static_cast<f32>(x) / static_cast<f32>(quadsPerSide);
                    f32 const v = static_cast<f32>(y) / static_cast<f32>(quadsPerSide);
                    vertices.Add(Vertex(glm::vec3(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f),
                                        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(u, v)));
                }
            }
            for (u32 y = 0; y < quadsPerSide; ++y)
            {
                for (u32 x = 0; x < quadsPerSide; ++x)
                {
                    u32 const i0 = y * verts + x;
                    for (u32 const idx : { i0, i0 + 1, i0 + verts + 1, i0, i0 + verts + 1, i0 + verts })
                    {
                        indices.Add(idx);
                    }
                }
            }
            return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        }

        void PokeU32(std::vector<u8>& blob, sizet offset, u32 value)
        {
            std::memcpy(blob.data() + offset, &value, sizeof(value));
        }

        u32 PeekU32(const std::vector<u8>& blob, sizet offset)
        {
            u32 value = 0;
            std::memcpy(&value, blob.data() + offset, sizeof(value));
            return value;
        }
    } // namespace

    TEST(VirtualMeshCookIdentity, BlobRecordsTheBuilderVersionAndConfigFingerprint)
    {
        VirtualMesh const dag = VirtualMeshBuilder::Build(*MakeGridMesh(16));
        ASSERT_TRUE(dag.IsValid());

        std::vector<u8> const blob = VirtualMeshSerializer::SerializeToBlob(dag);
        ASSERT_GT(blob.size(), 4 * sizeof(u32));

        EXPECT_EQ(PeekU32(blob, kBuilderVersionOffset), kVirtualMeshBuilderVersion);
        EXPECT_EQ(PeekU32(blob, kConfigFingerprintOffset), VirtualMeshSerializer::CurrentCookFingerprint());

        VirtualMesh loaded;
        EXPECT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(blob, loaded))
            << "a blob this build wrote must load in this build";
    }

    // The whole point: change the builder (bump the version) or a build-config default
    // (change the fingerprint) and every already-cached DAG must be rejected, so it is
    // re-cooked from geometry instead of rendering the old cook forever.
    TEST(VirtualMeshCookIdentity, RejectsABlobCookedByADifferentBuilderOrConfig)
    {
        VirtualMesh const dag = VirtualMeshBuilder::Build(*MakeGridMesh(16));
        ASSERT_TRUE(dag.IsValid());
        std::vector<u8> const blob = VirtualMeshSerializer::SerializeToBlob(dag);

        {
            std::vector<u8> stale = blob;
            PokeU32(stale, kBuilderVersionOffset, kVirtualMeshBuilderVersion + 1u);
            VirtualMesh out;
            EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(stale, out))
                << "a DAG cooked by a different builder version must be rejected, not silently rendered";
        }
        {
            std::vector<u8> stale = blob;
            PokeU32(stale, kConfigFingerprintOffset,
                    VirtualMeshSerializer::CurrentCookFingerprint() ^ 0xDEADBEEFu);
            VirtualMesh out;
            EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(stale, out))
                << "a DAG cooked with different VirtualMeshBuildConfig defaults must be rejected";
        }
        {
            // A pre-identity (v1) blob: the wire version alone already rejects it.
            std::vector<u8> stale = blob;
            PokeU32(stale, sizeof(u32), 1u);
            VirtualMesh out;
            EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(stale, out));
        }
    }

    // The set container carries the same guard through every part's OVGM header, so a
    // stale part cannot sneak in via the multi-submesh path either.
    TEST(VirtualMeshCookIdentity, SetReaderRejectsAStalePart)
    {
        VirtualMeshSet const set = VirtualMeshBuilder::BuildSet(*MakeGridMesh(16));
        ASSERT_TRUE(set.IsValid());
        std::vector<u8> const blob = VirtualMeshSerializer::SerializeSetToBlob(set);

        VirtualMeshSet roundTripped;
        ASSERT_TRUE(VirtualMeshSerializer::DeserializeSetFromBlob(blob, roundTripped));
        EXPECT_EQ(roundTripped.Parts.size(), set.Parts.size());

        // OVGS header is magic + version + partCount; the first part's OVGM starts after
        // submeshIndex (u32) + materialIndex (u32) + blobSize (u64).
        constexpr sizet kFirstPartBlob = 3 * sizeof(u32) + 2 * sizeof(u32) + sizeof(u64);
        std::vector<u8> stale = blob;
        PokeU32(stale, kFirstPartBlob + kBuilderVersionOffset, kVirtualMeshBuilderVersion + 7u);
        VirtualMeshSet out;
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeSetFromBlob(stale, out))
            << "a set whose part was cooked by a different builder must be rejected wholesale";
    }

    TEST(VirtualMeshCookIdentity, FingerprintIsStableAndConfigSensitive)
    {
        // Deterministic within a build...
        EXPECT_EQ(VirtualMeshSerializer::CurrentCookFingerprint(), VirtualMeshSerializer::CurrentCookFingerprint());
        // ...and not a degenerate constant.
        EXPECT_NE(VirtualMeshSerializer::CurrentCookFingerprint(), 0u);
        EXPECT_NE(VirtualMeshSerializer::CurrentCookFingerprint(), kVirtualMeshBuilderVersion);
    }
} // namespace OloEngine::Tests
