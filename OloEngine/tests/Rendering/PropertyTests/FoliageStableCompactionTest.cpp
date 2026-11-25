// OLO_TEST_LAYER: L6

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace OloEngine::Tests
{
    class FoliageStableCompactionTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            // The fixture submits the canonical GPU rows directly.
        }
    };

    TEST_F(FoliageStableCompactionTest, CanonicalMembershipPayloadAndCapsCrossScanBoundaries)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!RenderCommand::SupportsIndirectFirstInstance())
            GTEST_SKIP() << "this device does not support a separate indirect mesh region";

        FoliageGPUCuller culler;
        for (const u32 count : { 255u, 256u, 257u, 65535u, 65536u, 65537u })
        {
            SCOPED_TRACE(count);
            constexpr BoundingBox bounds{ { -0.1f, -0.1f, 0.4f }, { 4.1f, 0.1f, 0.6f } };
            constexpr u32 headerWords = static_cast<u32>(sizeof(FoliageCullLayerHeader) / sizeof(u32));
            FoliageGPUCuller::PreparedLayer prepared;
            prepared.GroupCount = 1u;
            prepared.InstanceCount = count;
            prepared.LocalBounds = bounds;
            prepared.Words.Init(0u, headerWords + 8u + count);
            const FoliageCullLayerHeader header{
                .GroupCount = 1u,
                .InstanceCount = count,
                .RowGroupOffset = 8u,
                .HalfExtentXZ = 0.01f,
            };
            std::memcpy(prepared.Words.GetData(), &header, sizeof(header));
            for (u32 axis = 0; axis < 3u; ++axis)
            {
                prepared.Words[headerWords + axis] = std::bit_cast<u32>(bounds.Min[axis]);
                prepared.Words[headerWords + 4u + axis] = std::bit_cast<u32>(bounds.Max[axis]);
            }

            std::vector<FoliageInstanceData> rows(count);
            constexpr std::array xByKind{ 4.0f, 0.1f, 0.1f, 0.9f, 0.9f };
            for (u32 i = 0; i < count; ++i)
            {
                rows[i].PositionScale = { xByKind[i % xByKind.size()], 0.0f, 0.5f, 1.0f };
                rows[i].RotationHeight = { 0.0f, 0.05f, static_cast<f32>(i) / 65537.0f, 0.0f };
                rows[i].ColorAlpha = { 0.1f, 0.4f, 0.2f, 0.5f };
            }

            FoliageGPUCuller::LayerResources layer;
            Ref<VertexBuffer> input;
            std::array<FoliageGPUCuller::ViewResources, 2> views;
            const std::array<u32, 2> cardCapacity{ count, count / 4u };
            const std::array<u32, 2> meshCapacity{ count / 7u, count };
            RunInFrame([&]()
                       {
                culler.EnsureInitialised();
                ASSERT_TRUE(culler.IsAvailable());
                ASSERT_TRUE(FoliageGPUCuller::BeginLayerUpload(layer, prepared));
                layer.LayerBuffer->SetData(prepared.Words.GetData(),
                                           static_cast<u32>(prepared.Words.Num() * sizeof(u32)), 0u);
                input = VertexBuffer::Create(static_cast<const void*>(rows.data()),
                                             count * static_cast<u32>(sizeof(FoliageInstanceData)));
                ASSERT_TRUE(input);
                constexpr std::array parts{
                    FoliageGPUCuller::Part{ 6u, 0u, false },
                    FoliageGPUCuller::Part{ 12u, 6u, true },
                    FoliageGPUCuller::Part{ 18u, 18u, true },
                };
                for (u32 slot = 0; slot < views.size(); ++slot)
                {
                    culler.EnsureViewCapacity(views[slot], cardCapacity[slot], meshCapacity[slot], 1u, count);
                    glm::mat4 clip{ 1.0f };
                    clip[3].x = slot == 0u ? 0.0f : 0.5f;
                    const FoliageGPUCuller::ViewInputs inputs{ .ViewFrustum = Frustum(clip),
                                                              .DistanceOrigin = glm::vec3(0.0f),
                                                              .MaxDistance = 10.0f };
                    const FoliageGPUCuller::LodInputs lod{ .MeshReach = 0.75f };
                    ASSERT_TRUE(culler.Cull(layer, views[slot], input->GetRHIHandle(), parts, inputs, false, lod));
                } });
            ASSERT_FALSE(HasFatalFailure());

            for (u32 slot = 0; slot < views.size(); ++slot)
            {
                SCOPED_TRACE(slot);
                std::vector<u32> cards;
                std::vector<u32> meshes;
                u32 visible = 0;
                for (u32 i = 0; i < count; ++i)
                {
                    const u32 kind = i % 5u;
                    const bool mesh = kind == 1u || kind == 2u;
                    if (kind == 0u || (slot == 1u && !mesh))
                        continue;
                    if (++visible > cardCapacity[slot])
                        continue;
                    cards.push_back(i);
                    if (mesh && meshes.size() < meshCapacity[slot])
                        meshes.push_back(i);
                }
                FoliageGPUCuller::Readback result;
                ASSERT_TRUE(culler.ReadbackResult(layer, views[slot], result));
                EXPECT_EQ(result.Visible, visible);
                EXPECT_EQ(result.Reserved, visible);
                EXPECT_EQ(result.Submitted, cards.size());
                EXPECT_EQ(result.MeshSubmitted, meshes.size());
                ASSERT_TRUE(std::ranges::equal(result.SourceRows, cards));
                ASSERT_TRUE(std::ranges::equal(result.MeshSourceRows, meshes));
                ASSERT_EQ(result.Compacted.Num(), cards.size());
                std::cout << "[foliage-kernel-order] rows=" << count << " slot=" << slot
                          << " visible=" << visible << " cards=" << result.Submitted
                          << " meshes=" << result.MeshSubmitted << '\n';

                constexpr u32 stride = static_cast<u32>(sizeof(FoliageInstanceData));
                StagedBufferReadback staging;
                std::vector<FoliageInstanceData> meshPayload(meshes.size());
                staging.Stage(views[slot].Compacted->GetRHIHandle(), static_cast<u64>(cardCapacity[slot]) * stride,
                              meshPayload.size() * stride, MemoryBarrierFlags::ShaderStorage);
                ASSERT_TRUE(staging.Read(meshPayload.data(), meshPayload.size() * stride));
                for (sizet i = 0; i < cards.size(); ++i)
                    ASSERT_EQ(std::memcmp(&result.Compacted[i], &rows[cards[i]], stride), 0) << i;
                for (sizet i = 0; i < meshes.size(); ++i)
                    ASSERT_EQ(std::memcmp(&meshPayload[i], &rows[meshes[i]], stride), 0) << i;
            }
        }
    }
} // namespace OloEngine::Tests
