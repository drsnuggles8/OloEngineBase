// OLO_TEST_LAYER: cullinglod
#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace OloEngine::Tests
{
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
} // namespace OloEngine::Tests
