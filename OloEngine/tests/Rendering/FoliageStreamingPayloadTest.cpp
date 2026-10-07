// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageStreamingPayload.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "../TestTempDir.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>

#include <filesystem>
#include <fstream>

namespace OloEngine::Tests
{
    class FoliageStreamingPayloadTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Directory = TempDir("foliage-payload");
        }

        void TearDown() override
        {
            std::error_code error;
            std::filesystem::remove_all(m_Directory, error);
        }

        std::filesystem::path Write(const char* name, const char* text)
        {
            const auto path = m_Directory / name;
            // PPM fixtures contain binary pixels: never translate header LF
            // into CRLF, whose extra LF stb would read as the first pixel.
            std::ofstream file(path, std::ios::binary);
            file << text;
            return path;
        }

        std::filesystem::path m_Directory;
    };

    TEST_F(FoliageStreamingPayloadTest, PreparationKeepsGeometryAndObjUvConventionWithoutGpuAllocations)
    {
        const auto file = Write("plant.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.2 0.25\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
        const u64 gpuBefore = RendererMemoryTracker::GetInstance().BuildReport().Gpu.ResidentBytes();
        const auto payload = LoadFoliageStreamingPayload(file);
        ASSERT_TRUE(payload);
        ASSERT_EQ(payload->Vertices.Num(), 3);
        ASSERT_EQ(payload->Indices.Num(), 3);
        ASSERT_EQ(payload->Parts.Num(), 1);
        EXPECT_FLOAT_EQ(payload->Vertices[0].TexCoord.y, 0.75f);
        EXPECT_FLOAT_EQ(payload->Bounds.Max.x, 1.0f);
        EXPECT_FLOAT_EQ(payload->Bounds.Max.y, 1.0f);
        EXPECT_EQ(payload->GetGpuBytes(), 3u * sizeof(Vertex) + 3u * sizeof(u32) + 8192u);
        EXPECT_GE(payload->GetCpuBytes(), 3u * sizeof(Vertex) + 3u * sizeof(u32));
        EXPECT_EQ(RendererMemoryTracker::GetInstance().BuildReport().Gpu.ResidentBytes(), gpuBefore);
        ASSERT_TRUE(payload->GetIOStats().has_value());
        EXPECT_GE(payload->GetIOStats()->ReadBytes, std::filesystem::file_size(file));
    }

    TEST_F(FoliageStreamingPayloadTest, SubmeshesUseGlobalIndicesAndTheirOwnMaterials)
    {
        Write("plant.mtl", "newmtl bark\nKd 1 1 1\nnewmtl leaf\nKd 0 1 0\n");
        const auto file = Write("plant.obj", "mtllib plant.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 4 0 0\nv 5 0 0\nv 4 1 0\n"
                                             "usemtl bark\nf 1 2 3\nusemtl leaf\nf 4 5 6\n");
        const auto payload = LoadFoliageStreamingPayload(file);
        ASSERT_TRUE(payload);
        ASSERT_EQ(payload->Parts.Num(), 2);
        EXPECT_EQ(payload->Parts[0].BaseIndex, 0u);
        EXPECT_EQ(payload->Parts[1].BaseIndex, 3u);
        for (u32 index : payload->Indices)
            EXPECT_LT(index, static_cast<u32>(payload->Vertices.Num()));
        EXPECT_NEAR(payload->Vertices[static_cast<i32>(payload->Indices[3])].Position.x, 4.0f, 1e-6f);
        EXPECT_GE(payload->GetIOStats()->ReadBytes,
                  std::filesystem::file_size(file) + std::filesystem::file_size(m_Directory / "plant.mtl"));
    }

    TEST_F(FoliageStreamingPayloadTest, MissingOrNonTriangleInputsKeepTheFallback)
    {
        EXPECT_FALSE(LoadFoliageStreamingPayload(m_Directory / "absent.obj"));
        const auto file = Write("line.obj", "v 0 0 0\nv 1 0 0\nl 1 2\n");
        EXPECT_FALSE(LoadFoliageStreamingPayload(file));
    }

    TEST_F(FoliageStreamingPayloadTest, ImportedPineRetainsEveryMaterialAndFitsThePreparationEstimate)
    {
        auto file = std::filesystem::path("OloEditor/SandboxProject/Assets/Models/Vegetation/pine/pine.obj");
        if (!std::filesystem::exists(file))
            file = "SandboxProject/Assets/Models/Vegetation/pine/pine.obj";
        ASSERT_TRUE(std::filesystem::exists(file));
        const auto payload = LoadFoliageStreamingPayload(std::filesystem::absolute(file));
        ASSERT_TRUE(payload);
        ASSERT_EQ(payload->Parts.Num(), 3);
        for (const auto& part : payload->Parts)
        {
            EXPECT_FALSE(part.AlbedoPath.IsEmpty());
            EXPECT_GT(part.AlbedoGpuBytes, 0u);
            EXPECT_GT(part.AlbedoUploadBytes, 0u);
        }
        EXPECT_LE(payload->GetCpuBytes(), std::filesystem::file_size(file) * 16u);
        EXPECT_GT(payload->GetGpuBytes(), payload->GetUploadBytes());
    }

    TEST_F(FoliageStreamingPayloadTest, TextureDecodeReservesBeforeReadingAndReleasesAfterPixels)
    {
        const auto file = Write("color.ppm", "P6\n2 1\n255\nabcdef");
        FFoliageStreamingPart part;
        part.AlbedoPath = file.generic_string();
        part.AlbedoWidth = 2;
        part.AlbedoHeight = 1;
        part.AlbedoUploadBytes = 8;
        part.AlbedoSourceBytes = std::filesystem::file_size(file);
        part.AlbedoDecodeBytes = part.AlbedoUploadBytes * 8u + part.AlbedoSourceBytes * 2u + 65536u;
        auto& budget = RepresentationStreaming::Get();
        const auto before = budget.GetStats();
        budget.SetStagingBudget(before.StagingCpuBytes + part.AlbedoDecodeBytes);
        {
            const auto image = DecodeFoliageStreamingAlbedo(part);
            EXPECT_TRUE(image);
            if (image)
            {
                EXPECT_EQ(image->Pixels.Num(), 8);
                EXPECT_EQ(image->Pixels[0], static_cast<u8>('a'));
                EXPECT_EQ(image->Pixels[3], 255u);
                EXPECT_EQ(budget.GetStats().StagingCpuBytes, before.StagingCpuBytes + part.AlbedoDecodeBytes);
                EXPECT_EQ(budget.GetStats().ReadBytes, before.ReadBytes + part.AlbedoSourceBytes);
            }
        }
        EXPECT_EQ(budget.GetStats().StagingCpuBytes, before.StagingCpuBytes);
        budget.SetStagingBudget(before.MaxStagingCpuBytes);
    }

    TEST_F(FoliageStreamingPayloadTest, LargeSidecarCannotBypassTheGeometryStagingBudget)
    {
        // A compact metadata-only PNG reaches the first IDAT boundary, where
        // stb's info scan stops after checking possible tRNS chunks. IHDR alone
        // is insufficient. The denied decode never needs image data or a giant
        // fixture allocation to establish an 8K working-set demand.
        constexpr u8 header[] = { 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
                                  0, 0, 32, 0, 0, 0, 32, 0, 8, 6, 0, 0, 0, 114, 170, 202, 89,
                                  0, 0, 0, 0, 73, 68, 65, 84, 53, 175, 6, 30,
                                  0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130 };
        int probeWidth = 0, probeHeight = 0, probeChannels = 0;
        ASSERT_TRUE(stbi_info_from_memory(header, sizeof(header), &probeWidth, &probeHeight, &probeChannels));
        ASSERT_EQ(probeWidth, 8192);
        ASSERT_EQ(probeHeight, 8192);
        ASSERT_EQ(probeChannels, 4);
        {
            std::ofstream image(m_Directory / "large.png", std::ios::binary);
            image.write(reinterpret_cast<const char*>(header), sizeof(header));
        }
        Write("plant.mtl", "newmtl leaf\nmap_Kd large.png\n");
        const auto file = Write("plant.obj", "mtllib plant.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl leaf\nf 1 2 3\n");
        const auto payload = LoadFoliageStreamingPayload(file);
        ASSERT_TRUE(payload);
        ASSERT_EQ(payload->Parts.Num(), 1);
        const auto& part = payload->Parts[0];
        ASSERT_EQ(part.AlbedoWidth, 8192u);
        ASSERT_EQ(part.AlbedoHeight, 8192u);
        EXPECT_EQ(part.AlbedoGpuBytes, 8192ull * 8192ull * 16u + 8192u);
        ASSERT_GT(part.AlbedoDecodeBytes, 256u * 1024u * 1024u);
        auto& budget = RepresentationStreaming::Get();
        const auto before = budget.GetStats();
        budget.SetStagingBudget(before.StagingCpuBytes + 1024u * 1024u);
        EXPECT_FALSE(DecodeFoliageStreamingAlbedo(part));
        EXPECT_EQ(budget.GetStats().StagingCpuBytes, before.StagingCpuBytes);
        EXPECT_EQ(budget.GetStats().DeniedForStaging, before.DeniedForStaging + 1u);
        budget.SetStagingBudget(before.MaxStagingCpuBytes);
    }

    TEST_F(FoliageStreamingPayloadTest, ReplacedImageCannotExceedItsPreparedDimensions)
    {
        const auto file = Write("color.ppm", "P6\n2 1\n255\nabcdef");
        FFoliageStreamingPart part;
        part.AlbedoPath = file.generic_string();
        part.AlbedoWidth = 2;
        part.AlbedoHeight = 1;
        part.AlbedoUploadBytes = 8;
        part.AlbedoSourceBytes = std::filesystem::file_size(file);
        part.AlbedoDecodeBytes = part.AlbedoUploadBytes * 8u + part.AlbedoSourceBytes * 2u + 65536u;
        // Preserve the compressed length while changing the probed dimensions.
        Write("color.ppm", "P6\n1 2\n255\nabcdef");
        auto& budget = RepresentationStreaming::Get();
        const auto before = budget.GetStats();
        budget.SetStagingBudget(before.StagingCpuBytes + part.AlbedoDecodeBytes);
        EXPECT_FALSE(DecodeFoliageStreamingAlbedo(part));
        EXPECT_EQ(budget.GetStats().StagingCpuBytes, before.StagingCpuBytes);
        budget.SetStagingBudget(before.MaxStagingCpuBytes);
    }

    TEST_F(FoliageStreamingPayloadTest, PreparedAlphaUsesUploadedRowOrderAndRepeatWrap)
    {
        auto image = Ref<FFoliageStreamingImage>::Create();
        image->Width = 2;
        image->Height = 2;
        constexpr u8 rgba[] = { 0, 0, 0, 11, 0, 0, 0, 22, 0, 0, 0, 33, 0, 0, 0, 44 };
        image->Pixels.Append(rgba, 16);
        EXPECT_EQ(image->AlphaAt({ 0.25f, 0.25f }), 11u);
        EXPECT_EQ(image->AlphaAt({ 0.75f, 0.75f }), 44u);
        EXPECT_EQ(image->AlphaAt({ 1.25f, -0.25f }), 33u);
        auto payload = Ref<FFoliageStreamingPayload>::Create();
        FFoliageStreamingPart part;
        part.AlbedoWidth = 2;
        part.AlbedoHeight = 2;
        part.AlbedoUploadBytes = 16;
        payload->Parts.Add(part);
        EXPECT_EQ(payload->GetUploadBytes(), 16u);
        // First level-zero upload, then replacement base + CPU level one.
        EXPECT_EQ(payload->GetUploadBytes(0.5f), 36u);
    }
} // namespace OloEngine::Tests
