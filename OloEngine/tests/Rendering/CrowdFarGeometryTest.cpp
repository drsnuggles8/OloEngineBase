// OLO_TEST_LAYER: cullinglod
#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace OloEngine::Tests
{
    TEST(CrowdFarGeometry, VertexFramesPackSubmeshLocalLanesWithoutNormalisingPoseSamples)
    {
        TArray<Vertex> vertices;
        for (u32 v = 0; v < 8; ++v)
            vertices.Emplace(glm::vec3(v, 2, 3), glm::vec3(1, 0, 0), glm::vec2(0));
        auto geometry = Ref<MeshSource>::Create(MoveTemp(vertices), TArray<u32>{ 0, 1, 2, 3, 4, 5, 5, 6, 7 });
        Submesh prefix;
        prefix.m_VertexCount = prefix.m_IndexCount = 3;
        Submesh part;
        part.m_BaseVertex = part.m_BaseIndex = 3;
        part.m_VertexCount = 5;
        part.m_IndexCount = 6;
        geometry->SetSubmeshes({ prefix, part });
        auto& weights = geometry->GetBoneInfluences();
        weights[3].SetBoneData(0, 0, 0.25f);
        weights[3].SetBoneData(1, 1, 0.25f);
        weights[4].SetBoneData(0, 1, 0.25f);
        weights[4].SetBoneData(1, 99, 0.75f); // outside the two-bone palette

        CrowdClip clip;
        clip.BoneCount = 2;
        clip.FrameCount = 2;
        clip.Atlas.SetNum(2);
        clip.Atlas[1].Transform[0].x = 2.0f;
        clip.Atlas[1].Transform[3].x = 8.0f;
        clip.Atlas[1].Normal[0].x = 4.0f;
        clip.Atlas[1].Normal[3].x = -4.0f;
        const auto baked = BakeCrowdVertexFrames(*geometry, 1, clip, 6);
        ASSERT_EQ(baked.RecordsPerFrame, 3u);
        ASSERT_EQ(baked.Records.Num(), 6);
        for (u32 frame = 0; frame < 2; ++frame)
        {
            const u32 offset = frame * baked.RecordsPerFrame;
            EXPECT_EQ(baked.Records[offset].CrowdPose.x, part.m_BaseVertex);
            EXPECT_EQ(baked.Records[offset].CrowdPose.y, baked.RecordsPerFrame);
            const auto& first = baked.Records[offset + 1];
            EXPECT_FLOAT_EQ(first.Transform[0].x, frame == 0 ? 1.5f : 5.5f);
            EXPECT_FLOAT_EQ(first.Transform[0].w, 0.5f);
            EXPECT_FLOAT_EQ(first.Normal[0].x, frame == 0 ? 0.5f : 1.5f);
            EXPECT_FLOAT_EQ(first.Normal[0].w, 0.0f);
            EXPECT_FLOAT_EQ(first.Transform[1].w, 0.25f);
            // The fifth vertex spills to a second payload record. Its zero
            // weights retain its rest position and normal at both times.
            const auto& second = baked.Records[offset + 2];
            EXPECT_FLOAT_EQ(second.Transform[0].x, 7.0f);
            EXPECT_FLOAT_EQ(second.Transform[0].w, 1.0f);
            EXPECT_FLOAT_EQ(second.Normal[0].x, 1.0f);
        }
        EXPECT_TRUE(BakeCrowdVertexFrames(*geometry, 1, clip, 5).Records.IsEmpty());
        geometry->GetIndices()[part.m_BaseIndex] = 0; // a valid vertex in the wrong submesh
        EXPECT_TRUE(BakeCrowdVertexFrames(*geometry, 1, clip, 6).Records.IsEmpty());
    }

    TEST(CrowdFarGeometry, MultiMaterialSplatsKeepAbsoluteIndicesSurfaceUvsAndBoneMotion)
    {
        // Two disjoint textured patches. Each patch spans two bones so an
        // identity-only farMesh representation cannot pass the posed proxy check.
        constexpr u32 cells = 8;
        constexpr u32 side = cells + 1;
        TArray<Vertex> vertices;
        TArray<u32> indices;
        TArray<Submesh> parts;
        TArray<BoneInfluence> influences;
        for (u32 p = 0; p < 2; ++p)
        {
            Submesh part;
            part.m_BaseVertex = static_cast<u32>(vertices.Num());
            part.m_BaseIndex = static_cast<u32>(indices.Num());
            part.m_MaterialIndex = 1u - p;
            part.m_IsRigged = true;
            for (u32 y = 0; y < side; ++y)
                for (u32 x = 0; x < side; ++x)
                {
                    const glm::vec2 uv = glm::vec2(x, y) / static_cast<f32>(cells);
                    vertices.Emplace(glm::vec3(x + p * 20, y, 0), glm::normalize(glm::vec3(uv - 0.5f, 1.0f)), uv);
                    BoneInfluence weight;
                    weight.SetBoneData(0, x < cells / 2 ? 0u : 1u, 1.0f);
                    influences.Add(weight);
                    if (x < cells && y < cells)
                    {
                        const u32 i = part.m_BaseVertex + y * side + x;
                        for (u32 vertex : { i, i + 1u, i + side, i + side, i + 1u, i + side + 1u })
                            indices.Add(vertex);
                    }
                }
            part.m_VertexCount = side * side;
            part.m_IndexCount = 6 * cells * cells;
            parts.Add(part);
        }
        auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        source->SetSkeleton(Ref<Skeleton>::Create(2));
        source->GetBoneInfluences() = MoveTemp(influences);
        source->SetSubmeshes(parts);
        source->SetMaterial(0, AssetHandle{ 123 });
        source->SetMaterial(1, AssetHandle{ 456 });

        const auto farMesh = MakeCrowdFarGeometry(source);
        ASSERT_TRUE(farMesh);
        ASSERT_NE(farMesh, source);
        ASSERT_EQ(farMesh->GetSubmeshes().Num(), 2);
        EXPECT_EQ(farMesh->GetSkeletonRef(), source->GetSkeletonRef());
        EXPECT_LT(farMesh->GetVertices().Num(), source->GetVertices().Num());
        EXPECT_LT(farMesh->GetIndices().Num(), source->GetIndices().Num());

        CrowdClip clip;
        clip.BoneCount = 2;
        clip.FrameCount = 1;
        clip.Atlas.Add(InstanceData{});
        // Bone 1 occupies the second matrix lane of the first atlas record.
        clip.Atlas[0].Normal[3].z = 3.0f;
        const auto posed = BakeCrowdProxy(farMesh, clip, 0);
        ASSERT_TRUE(posed);
        for (u32 p = 0; p < 2; ++p)
        {
            const auto& part = farMesh->GetSubmeshes()[p];
            ASSERT_GT(part.m_IndexCount, 0u);
            EXPECT_EQ(part.m_MaterialIndex, parts[p].m_MaterialIndex);
            EXPECT_EQ(farMesh->GetMaterial(p), source->GetMaterial(p));
            bool rigid = false, moved = false;
            std::array<u32, 3> donorCorners{};
            for (u32 i = 0; i < part.m_IndexCount; ++i)
            {
                const u32 index = farMesh->GetIndices()[part.m_BaseIndex + i];
                ASSERT_GE(index, part.m_BaseVertex);
                ASSERT_LT(index, part.m_BaseVertex + part.m_VertexCount);
                const Vertex& support = farMesh->GetVertices()[index];
                const u32 x = static_cast<u32>(std::lround(support.TexCoord.x * cells));
                const u32 y = static_cast<u32>(std::lround(support.TexCoord.y * cells));
                ASSERT_LE(x, cells);
                ASSERT_LE(y, cells);
                const u32 donor = parts[p].m_BaseVertex + y * side + x;
                EXPECT_FLOAT_EQ(support.TexCoord.x, source->GetVertices()[donor].TexCoord.x);
                EXPECT_FLOAT_EQ(support.TexCoord.y, source->GetVertices()[donor].TexCoord.y);
                EXPECT_LT(glm::length(support.Normal - source->GetVertices()[donor].Normal), 1e-6f);
                donorCorners[i % 3] = donor;
                if (i % 3 == 2)
                {
                    // The proxy triangle may span many original cells. Its
                    // three UVs must be those of one actual source triangle.
                    std::ranges::sort(donorCorners);
                    bool matched = false;
                    for (u32 j = 0; j < parts[p].m_IndexCount && !matched; j += 3)
                    {
                        const u32 first = parts[p].m_BaseIndex + j;
                        std::array<u32, 3> face{ source->GetIndices()[first], source->GetIndices()[first + 1], source->GetIndices()[first + 2] };
                        std::ranges::sort(face);
                        matched = face == donorCorners;
                    }
                    EXPECT_TRUE(matched);
                }
                const bool onBoneOne = support.Position.x - static_cast<f32>(p * 20) >= cells / 2;
                EXPECT_FLOAT_EQ(posed->GetVertices()[index].Position.z, onBoneOne ? 3.0f : 0.0f);
                rigid |= !onBoneOne;
                moved |= onBoneOne;
            }
            EXPECT_TRUE(rigid);
            EXPECT_TRUE(moved);
        }
    }

    TEST(CrowdFarGeometry, ThinDiagonalLeafRetainsSourceFacesWhileItsSiblingReduces)
    {
        constexpr u32 ringSize = 24;
        TArray<Vertex> vertices;
        TArray<u32> indices;
        const auto addVertex = [&](const glm::vec3& position)
        {
            const f32 id = static_cast<f32>(vertices.Num());
            vertices.Emplace(position, glm::vec3(0, 0, 1), glm::vec2(id / 64.0f, 1.0f - id / 64.0f));
        };
        // One material, two separated 48-face bipyramids. Every support of
        // the thin one lands on a tip. The other produces nonzero-area faces.
        for (u32 patch = 0; patch < 2; ++patch)
        {
            const bool thin = patch == 0;
            const u32 base = static_cast<u32>(vertices.Num());
            const glm::vec3 center(thin ? 0.5f : 12.5f, 0.5f, 0.5f);
            const f32 radius = thin ? 0.06f : 0.5f;
            addVertex(thin ? glm::vec3(0) : glm::vec3(center.x, center.y, 0));
            addVertex(thin ? glm::vec3(1) : glm::vec3(center.x, center.y, 1));
            for (u32 i = 0; i < ringSize; ++i)
            {
                const f32 angle = 2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / ringSize;
                addVertex(center + radius * glm::vec3(std::cos(angle), std::sin(angle), 0));
                const u32 left = base + 2 + i, right = base + 2 + (i + 1) % ringSize;
                for (const u32 corner : { base, right, left, base + 1, left, right })
                    indices.Add(corner);
            }
        }
        auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        Submesh part;
        part.m_VertexCount = static_cast<u32>(source->GetVertices().Num());
        part.m_IndexCount = static_cast<u32>(source->GetIndices().Num());
        source->AddSubmesh(part);

        const auto farMesh = MakeCrowdFarGeometry(source);
        ASSERT_TRUE(farMesh);
        ASSERT_NE(farMesh, source);
        constexpr u32 thinCorners = ringSize * 6;
        EXPECT_GT(farMesh->GetIndices().Num(), static_cast<i32>(thinCorners));
        EXPECT_LT(farMesh->GetIndices().Num(), source->GetIndices().Num());
        u32 retained = 0;
        for (const u32 index : farMesh->GetIndices())
        {
            const auto& vertex = farMesh->GetVertices()[index];
            if (vertex.Position.x > 1.0f)
                continue;
            ASSERT_LT(retained, thinCorners);
            const auto& original = source->GetVertices()[source->GetIndices()[retained++]];
            EXPECT_FLOAT_EQ(vertex.TexCoord.x, original.TexCoord.x);
            EXPECT_FLOAT_EQ(vertex.TexCoord.y, original.TexCoord.y);
            EXPECT_LT(glm::length(vertex.Position - original.Position), 1e-6f);
        }
        EXPECT_EQ(retained, thinCorners);
    }
} // namespace OloEngine::Tests
