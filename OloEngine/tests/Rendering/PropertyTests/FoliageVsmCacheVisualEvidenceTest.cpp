#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// FoliageVsmCacheVisualEvidenceTest — issue #1580.
//
// Foliage now keeps its virtual-shadow-map pages in clip levels too coarse to
// resolve its declared motion (FoliageShadowInvalidation.h). The claim is that
// the shadow this leaves on screen matches what invalidating the whole
// footprint every frame gives, within that bound. Checked under moving wind,
// with the clock then STOPPED and every foliage page redrawn at the same
// instant, so "cached" and "fresh" differ only in which pages were redrawn
// (the scene's animation clock is cumulative, so two separate runs never land
// on the same wind phase -- comparing across arms measures the wind):
//
//   EveryFrame  the pre-#1580 behaviour: the floor, cached == fresh;
//   Policy      what ships: within the floor plus a small fraction of
//   Never       foliage pages never invalidated by motion -- the NEGATIVE
//               CONTROL, which must differ, or the A/B cannot see staleness.
//
// Measured on VSM DebugMode 7 (the shadow factor the lit pass receives), from
// three camera angles; PNGs of the shipped beauty frame, its fresh twin and
// x8 difference images go to
// OloEditor/assets/tests/visual/FoliageVsmCache_GL_Deferred_<Angle>_*.png.
//
// Classification: L8 / visual evidence. Skips cleanly without GL 4.6; never
// DISABLED_.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/FoliageShadowInvalidation.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace FSI = FoliageShadowInvalidation;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        constexpr f32 kStartTime = 4.0f;
        // A third of a second of wind at 30 Hz: the plants sway a visible
        // fraction of their declared bound, and the rotation refreshes at least
        // two coarse levels on the way.
        constexpr u32 kAdvanceFrames = 10;
        constexpr f32 kFrameSeconds = 1.0f / 30.0f;

        struct ScopedMode
        {
            explicit ScopedMode(FSI::Mode mode)
                : m_Previous(FSI::GetMode())
            {
                FSI::SetMode(mode);
            }
            ~ScopedMode()
            {
                FSI::SetMode(m_Previous);
            }
            ScopedMode(const ScopedMode&) = delete;
            ScopedMode& operator=(const ScopedMode&) = delete;
            ScopedMode(ScopedMode&&) = delete;
            ScopedMode& operator=(ScopedMode&&) = delete;

          private:
            FSI::Mode m_Previous;
        };

        struct AngleCase
        {
            const char* m_Name;
            glm::vec3 m_Eye;
            f32 m_Yaw;
            f32 m_Pitch;
        };

        // The FoliageGPUCullEvidenceTest island framings.
        constexpr std::array<AngleCase, 3> kAngles = { {
            { "Ground", glm::vec3(128.0f, 34.0f, 162.0f), 0.0f, 0.2f },
            { "Reverse", glm::vec3(128.0f, 42.0f, -25.0f), 3.14159265f, 0.18f },
            { "Overview", glm::vec3(128.0f, 130.0f, 250.0f), 0.0f, 0.72f },
        } };

        struct Diff
        {
            f64 MeanAbs = 0.0; // mean |dL| over the frame, 0..255
            u32 Differing = 0; // pixels whose |dL| > 8/255
        };

        [[nodiscard]] Diff Compare(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            Diff out;
            if (a.size() != b.size() || a.empty())
            {
                out.MeanAbs = 1e9;
                return out;
            }
            f64 sum = 0.0;
            sizet n = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4, ++n)
            {
                const f64 la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
                const f64 lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
                const f64 d = std::abs(la - lb);
                sum += d;
                if (d > 8.0)
                    ++out.Differing;
            }
            out.MeanAbs = n == 0 ? 0.0 : sum / static_cast<f64>(n);
            return out;
        }
    } // namespace

    class FoliageVsmCacheVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity light = scene.CreateEntity("Sun");
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
            dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
            dl.m_Intensity = 3.0f;
            dl.m_CastShadows = true;

            m_TerrainEntity = scene.CreateEntity("Terrain");
            auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 7;
            terrain.m_ProceduralResolution = 192;
            terrain.m_ProceduralOctaves = 5;
            terrain.m_ProceduralFrequency = 2.0f;
            terrain.m_HeightShaping.HeightExponent = 1.3f;
            terrain.m_WorldSizeX = 256.0f;
            terrain.m_WorldSizeZ = 256.0f;
            terrain.m_HeightScale = 28.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_AutoMaterial = true;
            terrain.m_SplatmapGenResolution = 256;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);
            terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
            terrain.m_MaterialNeedsRebuild = true;
            terrain.m_AutoSplatNeedsRebuild = true;

            auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;
            foliage.m_Layers = TerrainGenerator::MakeFoliageLayersFromRules(terrain.m_LayerRules);
            foliage.m_NeedsRebuild = true;
        }

        void SetUp() override
        {
            RendererAttachedTest::SetUp();
            if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure())
                return;
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            m_PreviousShadows = settings;
            settings.Enabled = true;
            settings.VSM.Enabled = true;
            Renderer3D::GetShadowMap().SetSettings(settings);
            m_Restore = true;
        }

        void TearDown() override
        {
            Time::ClearMockTime();
            if (m_Restore)
                Renderer3D::GetShadowMap().SetSettings(m_PreviousShadows);
            RendererAttachedTest::TearDown();
        }

        // One frame per mock-clock step. The scene's animation clock (what the
        // wind reads) is CUMULATIVE: it adds each frame's clamped wall delta,
        // so it only moves forward and two runs of the same frames do not
        // land on the same wind phase. Every comparison below is therefore
        // made at ONE clock value, never across arms.
        void Step(const EditorCamera& camera, f32 advanceSeconds)
        {
            m_MockTime += advanceSeconds;
            Time::SetMockTime(m_MockTime);
            RunEditorFrames(camera, 1);
        }

        static void SetDebugMode(i32 mode)
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.VSM.DebugMode = mode;
            Renderer3D::GetShadowMap().SetSettings(settings);
        }

        struct ArmResult
        {
            std::vector<u8> Cached; // shadow factor as `mode` left it
            std::vector<u8> Fresh;  // the same clock, every page redrawn
            std::vector<u8> CachedBeauty;
            std::vector<u8> FreshBeauty;
            VSM::Statistics Pages; // the arm's steady page traffic, read back late
            // The LAST clip level the frame culled foliage for (the shadow
            // slot is reused level after level): groups and plants it kept.
            u32 LastLevelGroups = 0;
            u32 LastLevelPlants = 0;
        };

        // One arm: run `mode` while the wind moves, then STOP the clock and read
        // the shadow factor (VSM DebugMode 7, the term the lit pass receives)
        // as the arm left the cache; then, at the SAME clock, invalidate every
        // foliage page for a few frames and read it again. The difference is
        // exactly how stale the arm's pages were at that instant -- the plants
        // have not moved between the two reads. A beauty frame of each, with
        // the debug view off, is kept for a human to look at.
        void RunArm(const AngleCase& angle, FSI::Mode mode, ArmResult& out)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 3000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(angle.m_Eye, angle.m_Yaw, angle.m_Pitch);
            {
                // A fresh cache at a still clock, then the clock runs.
                const ScopedMode populate(FSI::Mode::EveryFrame);
                for (u32 i = 0; i < 8; ++i)
                    Step(camera, 0.0f);
            }
            {
                const ScopedMode arm(mode);
                SetDebugMode(0);
                for (u32 i = 0; i + 1 < kAdvanceFrames; ++i)
                    Step(camera, kFrameSeconds);
                out.Pages = Renderer3D::GetShadowMap().GetVirtualShadowMap().GetStatistics();
                Step(camera, kFrameSeconds);
                ReadSceneColor(out.CachedBeauty);
                if (auto* plants = Foliage())
                {
                    for (u32 layer = 0; layer < plants->GetLayerCount(); ++layer)
                    {
                        FoliageGPUCuller::Readback readback;
                        if (plants->ReadbackCull(layer, FoliageGPUCuller::ViewSlot::Shadow, readback))
                        {
                            out.LastLevelGroups += readback.GroupsVisible;
                            out.LastLevelPlants += readback.Submitted;
                        }
                    }
                }
                SetDebugMode(7);
                Step(camera, 0.0f);
                ReadSceneColor(out.Cached);
            }
            {
                const ScopedMode fresh(FSI::Mode::EveryFrame);
                for (u32 i = 0; i < 3; ++i)
                    Step(camera, 0.0f);
                ReadSceneColor(out.Fresh);
                SetDebugMode(0);
                Step(camera, 0.0f);
                ReadSceneColor(out.FreshBeauty);
            }
        }

        void ReadSceneColor(std::vector<u8>& out)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb);
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + static_cast<sizet>(y) * rowBytes;
                u8* bot = out.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        }

        static void WritePng(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       pixels.data(), static_cast<int>(kWidth) * 4),
                      0)
                << path;
        }

        static std::vector<u8> AmplifiedDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            std::vector<u8> out(a.size(), 255u);
            for (sizet i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
            {
                for (sizet c = 0; c < 3; ++c)
                    out[i + c] = static_cast<u8>(std::min(255, 8 * std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]))));
            }
            return out;
        }

        [[nodiscard]] FoliageRenderer* Foliage()
        {
            if (!m_TerrainEntity || !m_TerrainEntity.HasComponent<FoliageComponent>())
                return nullptr;
            return m_TerrainEntity.GetComponent<FoliageComponent>().m_Renderer.get();
        }

        Entity m_TerrainEntity;
        ShadowSettings m_PreviousShadows{};
        bool m_Restore = false;
        f32 m_MockTime = kStartTime;
    };

    TEST_F(FoliageVsmCacheVisualEvidenceTest, CachedCoarseLevelsMatchPerFrameInvalidationUnderWind)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(Renderer3D::GetShadowMap().IsVirtualShadowMapActive()) << "VSM did not come up; nothing to compare";
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        for (const AngleCase& angle : kAngles)
        {
            SCOPED_TRACE(angle.m_Name);
            ArmResult reference;
            ArmResult policy;
            ArmResult never;
            RunArm(angle, FSI::Mode::EveryFrame, reference);
            ASSERT_FALSE(HasFatalFailure());
            RunArm(angle, FSI::Mode::Policy, policy);
            ASSERT_FALSE(HasFatalFailure());
            RunArm(angle, FSI::Mode::Never, never);
            ASSERT_FALSE(HasFatalFailure());

            const std::string stem = std::string("FoliageVsmCache_GL_Deferred_") + angle.m_Name;
            WritePng(stem + "_Policy", policy.CachedBeauty);
            WritePng(stem + "_PolicyFresh", policy.FreshBeauty);
            WritePng(stem + "_Never", never.CachedBeauty);
            WritePng(stem + "_ShadowFactor_Policy", policy.Cached);
            WritePng(stem + "_ShadowFactor_PolicyDiffx8", AmplifiedDiff(policy.Cached, policy.Fresh));
            WritePng(stem + "_ShadowFactor_NeverDiffx8", AmplifiedDiff(never.Cached, never.Fresh));

            // EveryFrame against itself: the floor (anything a redraw at a
            // still clock changes on its own). Policy: how stale the shipped
            // cache is. Never: the negative control, every foliage page as it
            // was before the wind moved.
            const Diff noise = Compare(reference.Cached, reference.Fresh);
            const Diff shipped = Compare(policy.Cached, policy.Fresh);
            const Diff stale = Compare(never.Cached, never.Fresh);
            const Diff beautyShipped = Compare(policy.CachedBeauty, policy.FreshBeauty);
            // What the cache saves: pages redrawn per frame under wind.
            for (const auto& [name, arm] : { std::pair<const char*, const ArmResult*>{ "every-frame", &reference },
                                             { "policy", &policy },
                                             { "never", &never } })
            {
                std::printf("[foliage-vsm] %-8s %-11s pages drawn %u, allocated %u, requested %u, resident %u, failed %u; "
                            "last clip level culled kept %u groups, %u plants\n",
                            angle.m_Name, name, arm->Pages.PagesDrawn, arm->Pages.PagesAllocated,
                            arm->Pages.PagesRequested, arm->Pages.PagesResident, arm->Pages.PagesFailed,
                            arm->LastLevelGroups, arm->LastLevelPlants);
            }
            std::printf("[foliage-vsm] %-8s shadow factor vs a full redraw at the same clock: every-frame %.4f (%u px), "
                        "policy %.4f (%u px), never %.4f (%u px); beauty policy %.4f (%u px)\n",
                        angle.m_Name, noise.MeanAbs, noise.Differing, shipped.MeanAbs, shipped.Differing,
                        stale.MeanAbs, stale.Differing, beautyShipped.MeanAbs, beautyShipped.Differing);

            // The negative control must see the staleness, or nothing below means anything.
            EXPECT_GT(stale.Differing, std::max<u32>(4u * noise.Differing, 500u))
                << "never invalidating foliage changed nothing measurable: the A/B cannot see a stale shadow";
            // The shipped cache is within the floor plus a small fraction of
            // what never invalidating costs: the sub-texel lag the declared
            // bound allows in the coarse levels, sampled only far away.
            EXPECT_LE(shipped.Differing, std::max<u32>(2u * noise.Differing, 200u) + stale.Differing / 20u)
                << "the cached coarse levels changed the frame by more than the declared bound allows";
            EXPECT_LT(shipped.MeanAbs, 0.1 * stale.MeanAbs + 2.0 * noise.MeanAbs + 0.01);
        }
    }
} // namespace OloEngine::Tests
