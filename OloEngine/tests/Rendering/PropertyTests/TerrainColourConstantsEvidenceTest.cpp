// OLO_TEST_LAYER: integration
// =============================================================================
// TerrainColourConstantsEvidenceTest.cpp — #1565 on the real pipeline.
//
// The terrain's two colour programs (Terrain_PBR for Forward / Forward+,
// Terrain_GBuffer for Deferred) used to read the brush preview (binding 11)
// and the snow accumulation clipmap (binding 16) as fragment uniform blocks.
// On the raw bindless route that was one block too many for NVIDIA, so both
// now arrive as flat varyings written by the tessellation-evaluation stage.
// Nothing else exercised either lane on a rendered frame, so each is pinned
// here by an A/B that only that lane can produce:
//   * brush on vs off: a localised, cyan-shifted disk, and nothing else;
//   * clipmap accumulation on vs off with displacement scale 0, so the
//     geometry cannot move and only the fragment's snow weight can whiten.
//
// Evidence PNGs: TerrainBrushPreview[Off]_GL_<Path>.png and
// TerrainSnowCover[Off]_GL_<Path>.png under OloEditor/assets/tests/visual/.
// Run with OLO_RHI_BINDLESS=1 on a bindless-capable driver for the raw route.
// =============================================================================
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 480;
        constexpr u32 kHeight = 360;
        constexpr u32 kSettleFrames = 8;

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Other";
            }
        }

        struct FrameDiff
        {
            u64 Changed = 0;         // pixels with any channel differing by more than 2
            f64 MeanCyanShift = 0.0; // over changed pixels: ((dG + dB) / 2) - dR
            f64 MeanLumaShift = 0.0; // over changed pixels
        };

        [[nodiscard]] FrameDiff Compare(const std::vector<u8>& off, const std::vector<u8>& on)
        {
            FrameDiff d;
            for (sizet i = 0; i + 3 < off.size() && i + 3 < on.size(); i += 4)
            {
                const i32 dr = static_cast<i32>(on[i]) - off[i];
                const i32 dg = static_cast<i32>(on[i + 1]) - off[i + 1];
                const i32 db = static_cast<i32>(on[i + 2]) - off[i + 2];
                if (std::abs(dr) > 2 || std::abs(dg) > 2 || std::abs(db) > 2)
                {
                    ++d.Changed;
                    d.MeanCyanShift += 0.5 * (dg + db) - dr;
                    d.MeanLumaShift += 0.2126 * dr + 0.7152 * dg + 0.0722 * db;
                }
            }
            if (d.Changed > 0)
            {
                d.MeanCyanShift /= static_cast<f64>(d.Changed);
                d.MeanLumaShift /= static_cast<f64>(d.Changed);
            }
            return d;
        }
    } // namespace

    class TerrainColourConstantsEvidence : public RendererAttachedTest
    {
      protected:
        Ref<UniformBuffer> m_Brush;
        EditorCamera m_Camera{ 50.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 300.0f };
        SnowSettings m_SavedSnow{};
        SnowAccumulationSettings m_SavedAccumulation{};
        bool m_Built = false; // BuildScene does not run when SetUp skips for want of a GPU

        void BuildScene() override
        {
            m_Built = true;
            EnableRendering(kWidth, kHeight);
            m_SavedSnow = Renderer3D::GetSnowSettings();
            m_SavedAccumulation = Renderer3D::GetSnowAccumulationSettings();
            Renderer3D::GetPostProcessSettings() = PostProcessSettings{};
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.SSAOEnabled = false;
            pp.GTAOEnabled = false;
            auto& rs = Renderer3D::GetRendererSettings();
            rs.ForwardPlusAutoSwitch = false;
            rs.EditorDebugDrawsEnabled = false;
            rs.ShowGrid = false;
            rs.ShowWorldAxisHelper = false;
            rs.ShowCameraFrustums = false;

            Entity sun = GetScene().CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.3f, -0.8f, -0.4f));
            light.m_Intensity = 3.0f;
            light.m_CastShadows = false;

            Entity terrainEntity = GetScene().CreateEntity("Flat terrain");
            auto& terrain = terrainEntity.AddComponent<TerrainComponent>();
            terrain.m_WorldSizeX = 16.0f;
            terrain.m_WorldSizeZ = 16.0f;
            terrain.m_HeightScale = 1.0f;
            terrain.m_CollisionEnabled = false;
            terrain.m_TessellationEnabled = true;
            terrain.m_TerrainData = Ref<TerrainData>::Create();
            terrain.m_TerrainData->CreateFlat(65, 0.0f);
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            terrainEntity.GetComponent<TransformComponent>().Translation = { -8.0f, 0.0f, -8.0f };

            // The editor owns this buffer in a real session (EditorLayer's
            // m_BrushPreviewUBO); the test stands in for it.
            m_Brush = UniformBuffer::Create(ShaderBindingLayout::BrushPreviewUBO::GetSize(),
                                            ShaderBindingLayout::UBO_BRUSH_PREVIEW);
            SetBrush(false);

            m_Camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            m_Camera.SetPose({ 0.0f, 11.0f, 11.0f }, 0.0f, 0.75f);
        }

        void TearDown() override
        {
            if (!m_Built)
            {
                RendererAttachedTest::TearDown();
                return;
            }
            Renderer3D::GetSnowSettings() = m_SavedSnow;
            Renderer3D::GetSnowAccumulationSettings() = m_SavedAccumulation;
            SnowAccumulationSystem::Update(m_SavedAccumulation, m_Camera.GetPosition(), Timestep(0.0f));
            SnowAccumulationSystem::Reset();
            m_Brush = nullptr;
            RendererAttachedTest::TearDown();
        }

        void SetBrush(bool active)
        {
            ShaderBindingLayout::BrushPreviewUBO brush{};
            if (active)
            {
                brush.BrushPosAndRadius = glm::vec4(0.0f, 0.0f, 0.0f, 3.0f);
                brush.BrushParams = glm::vec4(1.0f, 0.5f, 0.0f, 0.0f); // active, falloff, sculpt (cyan)
            }
            m_Brush->SetData(&brush, sizeof(brush));
        }

        void UsePath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        [[nodiscard]] std::vector<u8> Capture(const std::string& name)
        {
            RunEditorFrames(m_Camera, kSettleFrames, 0.0f);
            std::vector<u8> pixels;
            u32 width = 0;
            u32 height = 0;
            EXPECT_TRUE(ReadbackComposite(pixels, width, height));
            EXPECT_EQ(width, kWidth);
            EXPECT_EQ(height, kHeight);
            if (!pixels.empty())
            {
                const auto directory = std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual";
                std::filesystem::create_directories(directory);
                stbi_flip_vertically_on_write(1);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(), static_cast<int>(width),
                                         static_cast<int>(height), 4, pixels.data(), static_cast<int>(width * 4)),
                          0);
                stbi_flip_vertically_on_write(0);
            }
            return pixels;
        }
    };

    // The brush reaches both terrain colour programs through the
    // tessellation-evaluation stage. A dropped lane draws no disk; a lane
    // carrying the wrong vec4 draws it in the wrong place or colour.
    TEST_F(TerrainColourConstantsEvidence, TheBrushPreviewDrawsACyanDiskOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(PathName(path));
            UsePath(path);
            SetBrush(false);
            const std::vector<u8> off = Capture(std::string("TerrainBrushPreviewOff_GL_") + PathName(path));
            SetBrush(true);
            const std::vector<u8> on = Capture(std::string("TerrainBrushPreview_GL_") + PathName(path));
            SetBrush(false);

            const FrameDiff d = Compare(off, on);
            const u64 total = static_cast<u64>(kWidth) * kHeight;
            GTEST_LOG_(INFO) << PathName(path) << ": brush changed " << d.Changed << " px, mean cyan shift "
                             << d.MeanCyanShift;
            // A 3 m disk on a 16 m terrain seen from 15 m: a few percent of the
            // frame, never most of it.
            EXPECT_GT(d.Changed, total / 100u) << "no brush disk: the lane did not reach the fragment stage";
            EXPECT_LT(d.Changed, total / 4u) << "the brush changed most of the frame, not a 3 m disk";
            EXPECT_GT(d.MeanCyanShift, 5.0) << "the disk is not the sculpt colour (cyan)";
        }
    }

    // The clipmap constants reach the fragment's snow weight. Displacement scale
    // 0 keeps the geometry still, and a coverage band far above the terrain
    // keeps the procedural cover at zero, so the accumulation boost is the only
    // thing that can whiten the frame.
    TEST_F(TerrainColourConstantsEvidence, AccumulatedSnowWhitensTheTerrainOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SnowSettings& snow = Renderer3D::GetSnowSettings();
        snow = SnowSettings{};
        snow.Enabled = true;
        snow.HeightStart = 50.0f;
        snow.HeightFull = 60.0f;
        snow.SSSBlurEnabled = false;
        SnowAccumulationSettings& accumulation = Renderer3D::GetSnowAccumulationSettings();

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(PathName(path));
            UsePath(path);
            accumulation = SnowAccumulationSettings{};
            accumulation.Enabled = false;
            SnowAccumulationSystem::Reset();
            SnowAccumulationSystem::Update(accumulation, m_Camera.GetPosition(), Timestep(0.0f));
            const std::vector<u8> off = Capture(std::string("TerrainSnowCoverOff_GL_") + PathName(path));

            accumulation.Enabled = true;
            accumulation.AccumulationRate = 1.0f;
            accumulation.MaxDepth = 1.0f;
            accumulation.MeltRate = 0.0f;
            accumulation.RestorationRate = 0.0f;
            accumulation.DisplacementScale = 0.0f;
            SnowAccumulationSystem::Reset();
            SnowAccumulationSystem::Update(accumulation, m_Camera.GetPosition(), Timestep(2.0f));
            const std::vector<u8> on = Capture(std::string("TerrainSnowCover_GL_") + PathName(path));

            const FrameDiff d = Compare(off, on);
            const u64 total = static_cast<u64>(kWidth) * kHeight;
            GTEST_LOG_(INFO) << PathName(path) << ": accumulation changed " << d.Changed << " px, mean luma shift "
                             << d.MeanLumaShift;
            EXPECT_GT(d.Changed, total / 10u) << "accumulated snow did not reach the terrain's fragment snow weight";
            EXPECT_GT(d.MeanLumaShift, 10.0) << "the accumulated cover did not whiten the terrain";
        }
        accumulation.Enabled = false;
    }
} // namespace OloEngine::Tests
