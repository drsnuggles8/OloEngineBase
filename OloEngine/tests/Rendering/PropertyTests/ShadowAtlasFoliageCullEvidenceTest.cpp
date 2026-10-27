#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// ShadowAtlasFoliageCullEvidenceTest — issue #1590, the shadow atlas route.
//
// Writes
//   OloEditor/assets/tests/visual/ShadowAtlasFoliageCull_GL_<Path>.png
// and its A/B control, the same frame recorded the way it was before #1590
// (Levers::FaultShadowFoliageCullCap)
//   OloEditor/assets/tests/visual/ShadowAtlasFoliageCullOff_GL_<Path>.png
//
// THE FAULT. The foliage GPU cull has FoliageGPUCuller::kMaxShadowViews (4)
// shadow slots, and a cascade or atlas region was recorded in one batch, so
// every view past the fourth drew the whole island's foliage uncompacted: a
// point light's fifth and sixth cube faces and every face of the lights after
// it. In the #1259 acceptance scene that is where 0.1-0.9 s of the atlas
// route's GPU went. ShadowRenderPass now records such a region in batches of
// the slot count, culling each batch just before it draws.
//
// Two shadowed point lights over the island: twelve cube faces, three batches.
//
// What is checked:
//   1. No shadow view asks for a cull slot past the last
//      (FoliageRenderer::GetShadowViewsPastCullSlots stays put), and the fault
//      lever makes them do so again -- the negative control, which says the
//      check can see the fault.
//   2. The batched frame IS the single-batch frame: the cull is conservative,
//      so drawing every plant into a face and drawing that face's survivors
//      cast the same depth. A batch that culled into the wrong slot, or drew a
//      slot a later batch had already rewritten, would move shadows. Judged
//      against a control pair of the batched arm, the frame's own noise floor.
//      And against a POSITIVE control: the frame with no foliage in the shadow
//      maps must differ, or the frame holds no lamp-cast foliage shadow to move.
//   3. ShadowPass/Atlas, the region bracket #1590 added, timed in both arms back
//      to back and printed. This island is too light for the difference to be
//      a reliable assertion; the cost claim is measured on the acceptance scene.
//
// Cells: GL x {Forward, Forward+, Deferred}. The shadow pass runs before the
// path forks, so the three cells pin that nothing path-specific reaches it.
// Vulkan records the atlas in parallel, which is the same batching through
// RecordParallel; it is a live-editor cell, evidenced in the PR. MSAA and a
// non-native resolution are not cells: the atlas tiles have their own sizes.
//
// Classification: L8 / visual evidence. Skips cleanly without a GL 4.6
// context; never DISABLED_.
// =============================================================================

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        constexpr f32 kCaptureTime = 4.0f; // both arms see the same wind phase

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;
            ScopedMockTime(ScopedMockTime&&) = delete;
            ScopedMockTime& operator=(ScopedMockTime&&) = delete;
        };

        // Restored whatever the test does: a failed assertion must not leave
        // every later fixture recording its regions in one batch.
        struct ScopedCullCapFault
        {
            explicit ScopedCullCapFault(bool on)
                : m_Previous(Levers::FaultShadowFoliageCullCap())
            {
                Levers::SetFaultShadowFoliageCullCap(on);
            }
            ~ScopedCullCapFault()
            {
                Levers::SetFaultShadowFoliageCullCap(m_Previous);
            }
            ScopedCullCapFault(const ScopedCullCapFault&) = delete;
            ScopedCullCapFault& operator=(const ScopedCullCapFault&) = delete;
            ScopedCullCapFault(ScopedCullCapFault&&) = delete;
            ScopedCullCapFault& operator=(ScopedCullCapFault&&) = delete;

          private:
            bool m_Previous;
        };

        struct PixelDiff
        {
            u32 Count = 0;
            u32 FirstX = 0;
            u32 FirstY = 0;
            u32 MaxDelta = 0;
            // Pixels off by more than kNoiseLevels: what a moved shadow does.
            u32 Moved = 0;
        };
        // An 8-bit channel the same frame renders a level apart run to run (a
        // control pair of one arm shows it); a shadow that moved darkens or
        // lights its pixels by far more.
        constexpr u32 kNoiseLevels = 2u; // measured: a control pair differs by at most 1

        [[nodiscard]] PixelDiff DiffPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            PixelDiff diff;
            if (a.size() != b.size())
            {
                diff.Count = std::numeric_limits<u32>::max();
                return diff;
            }
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                u32 delta = 0;
                for (sizet c = 0; c < 3; ++c)
                    delta = std::max(delta, static_cast<u32>(std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]))));
                if (delta == 0)
                    continue;
                if (diff.Count++ == 0)
                {
                    diff.FirstX = static_cast<u32>((i / 4) % kWidth);
                    diff.FirstY = static_cast<u32>((i / 4) / kWidth);
                }
                diff.MaxDelta = std::max(diff.MaxDelta, delta);
                if (delta > kNoiseLevels)
                    ++diff.Moved;
            }
            return diff;
        }

        // The island's meadow, looking at the two lamps.
        const glm::vec3 kEye(128.0f, 34.0f, 162.0f);
        constexpr f32 kYaw = 0.0f;
        constexpr f32 kPitch = 0.25f;
        const std::array<glm::vec2, 2> kLampXZ{ glm::vec2(120.0f, 130.0f), glm::vec2(140.0f, 120.0f) };
        constexpr f32 kLampHeight = 3.0f;
        // The foliage-shadow-off frame must differ from the shipped one in at
        // least this many moved pixels, or the frame shows no foliage shadow
        // from the lamps and the comparison below could not see one move.
        constexpr u32 kMinFoliageShadowPixels = 1000u;
    } // namespace

    class ShadowAtlasFoliageCullEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                // Dim and unshadowed: the atlas is the subject, and a sun's
                // cascades would only add a second region to the frame.
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Intensity = 0.3f;
                dl.m_CastShadows = false;
            }

            m_TerrainEntity = scene.CreateEntity("Terrain");
            {
                // The island the other foliage evidence fixtures build.
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

            // Two shadowed lamps in front of the camera: twelve cube faces. Put
            // on the ground by PlaceLampsOnGround once the heightfield exists.
            for (const glm::vec2& xz : kLampXZ)
            {
                Entity lamp = scene.CreateEntity("Lamp");
                lamp.GetComponent<TransformComponent>().Translation = glm::vec3(xz.x, 30.0f, xz.y);
                auto& point = lamp.AddComponent<PointLightComponent>();
                point.m_Color = glm::vec3(1.0f, 0.85f, 0.6f);
                point.m_Intensity = 150.0f;
                point.m_Range = 25.0f;
                point.m_CastShadows = true;
                m_Lamps.push_back(lamp);
            }
        }

        // Each lamp kLampHeight over the ground under it, among the plants, so
        // its faces have foliage to cast and the frame shows those shadows.
        void PlaceLampsOnGround()
        {
            const auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
            ASSERT_TRUE(terrain.m_TerrainData) << "the terrain has no heightfield yet";
            for (Entity lamp : m_Lamps)
            {
                glm::vec3& at = lamp.GetComponent<TransformComponent>().Translation;
                at.y = terrain.m_TerrainData->GetHeightAt(at.x / terrain.m_WorldSizeX, at.z / terrain.m_WorldSizeZ) *
                           terrain.m_HeightScale +
                       kLampHeight;
            }
        }

        // Every layer in or out of the shadow maps; a regeneration applies it,
        // which the next RunEditorFrames finishes before returning.
        void SetFoliageCastsShadows(bool casts)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            for (auto& layer : foliage.m_Layers)
                layer.CastShadows = casts;
            foliage.m_NeedsRebuild = true;
        }

        [[nodiscard]] FoliageRenderer* Foliage()
        {
            if (!m_TerrainEntity || !m_TerrainEntity.HasComponent<FoliageComponent>())
                return nullptr;
            return m_TerrainEntity.GetComponent<FoliageComponent>().m_Renderer.get();
        }

        // Renders `frames` frames and returns the cheapest valid
        // ShadowPass/Atlas bracket among them (ms; infinity if none resolved).
        [[nodiscard]] f64 RenderAndTimeAtlas(u32 frames)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 3000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kEye, kYaw, kPitch);
            f64 best = std::numeric_limits<f64>::infinity();
            for (u32 i = 0; i < frames; ++i)
            {
                RunEditorFrames(camera, 1);
                const auto timings = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
                f64 atlas = 0.0;
                bool any = false;
                for (const auto& pass : timings.Passes)
                {
                    if (pass.IsSubPass && pass.Name == FString("ShadowPass/Atlas") && pass.IsValid())
                    {
                        atlas += pass.Sample.GpuMs;
                        any = true;
                    }
                }
                if (any)
                    best = std::min(best, atlas);
            }
            return best;
        }

        void ReadFrame(std::vector<u8>& out)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "no SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u; // GL readback is bottom-up
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bot = out.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        }

        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "failed to create '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       pixels.data(), static_cast<int>(kWidth) * 4),
                      0)
                << "stbi_write_png failed to write '" << path << "'";
        }

        Entity m_TerrainEntity;
        std::vector<Entity> m_Lamps;
    };

    TEST_F(ShadowAtlasFoliageCullEvidenceTest, EveryAtlasFaceDrawsItsOwnCulledFoliage)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        auto& shadowMap = Renderer3D::GetShadowMap();
        // Restored however the test leaves, so no later fixture inherits them.
        struct RestoreSettings
        {
            ShadowSettings Shadows = Renderer3D::GetShadowMap().GetSettings();
            RenderingPath Path = Renderer3D::GetRendererSettings().Path;
            ~RestoreSettings()
            {
                Renderer3D::GetShadowMap().SetSettings(Shadows);
                Renderer3D::GetRendererSettings().Path = Path;
                Renderer3D::ApplyRendererSettings();
            }
        } restore;
        ShadowSettings settings = restore.Shadows;
        settings.VSM.Enabled = false; // the atlas serves local lights only without VSM's local layers
        shadowMap.SetSettings(settings);

        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        constexpr std::array<PathCase, 3> paths{ { { "Forward", RenderingPath::Forward },
                                                   { "ForwardPlus", RenderingPath::ForwardPlus },
                                                   { "Deferred", RenderingPath::Deferred } } };
        for (const PathCase& path : paths)
        {
            SCOPED_TRACE(path.Name);
            Renderer3D::GetRendererSettings().Path = path.Path;
            Renderer3D::ApplyRendererSettings(); // a path written without this leaves Deferred's graph unbuilt

            // Warm up: the foliage generates, the atlas packs its faces, the
            // cull slots grow to their demand.
            (void)RenderAndTimeAtlas(12);
            if (path.Path == paths.front().Path)
            {
                ASSERT_NO_FATAL_FAILURE(PlaceLampsOnGround());
                (void)RenderAndTimeAtlas(8);

                // OLO_NO_SHADOWPASS_BRACKETS, the A/B lever for what the
                // brackets cost: no family bracket, the pass's own one kept.
                const bool previous = Levers::NoShadowPassBrackets();
                Levers::SetNoShadowPassBrackets(true);
                // Timings resolve a few frames late: the first frames under the
                // lever still report brackets opened before it.
                (void)RenderAndTimeAtlas(4);
                const f64 atlasWithout = RenderAndTimeAtlas(4);
                bool shadowPassTimed = false;
                for (const auto& pass : GPUPassTimerPool::GetInstance().GetLastFrameTimings().Passes)
                    shadowPassTimed = shadowPassTimed || (!pass.IsSubPass && pass.Name == FString("ShadowPass") && pass.IsValid());
                Levers::SetNoShadowPassBrackets(previous);
                EXPECT_FALSE(std::isfinite(atlasWithout)) << "OLO_NO_SHADOWPASS_BRACKETS still published ShadowPass/Atlas";
                EXPECT_TRUE(shadowPassTimed) << "OLO_NO_SHADOWPASS_BRACKETS took the pass's own bracket too";
                EXPECT_TRUE(std::isfinite(RenderAndTimeAtlas(4))) << "ShadowPass/Atlas did not come back with the lever off";
            }
            FoliageRenderer* foliage = Foliage();
            ASSERT_NE(foliage, nullptr) << "the island has no foliage renderer";
            ASSERT_GT(foliage->GetTotalInstanceCount(), 0u) << "the island generated no plants";
            ASSERT_GT(shadowMap.GetAtlasEntryCount(), FoliageGPUCuller::kMaxShadowViews)
                << "the fixture must give the atlas more faces than the cull has slots";

            // Interleaved so drift charges both arms: batched, fault, batched.
            const u64 before = foliage->GetShadowViewsPastCullSlots();
            const f64 batchedA = RenderAndTimeAtlas(8);
            std::vector<u8> batchedPixels;
            ASSERT_NO_FATAL_FAILURE(ReadFrame(batchedPixels));
            const u64 afterBatched = foliage->GetShadowViewsPastCullSlots();

            f64 single = 0.0;
            std::vector<u8> singlePixels;
            u64 afterFault = 0;
            {
                const ScopedCullCapFault fault(true);
                single = RenderAndTimeAtlas(8);
                ASSERT_NO_FATAL_FAILURE(ReadFrame(singlePixels));
                afterFault = foliage->GetShadowViewsPastCullSlots();
            }
            const f64 batchedB = RenderAndTimeAtlas(8);
            std::vector<u8> controlPixels; // the batched arm again: this frame's own noise floor
            ASSERT_NO_FATAL_FAILURE(ReadFrame(controlPixels));
            const f64 batched = std::min(batchedA, batchedB);

            // POSITIVE CONTROL: the same frame with no foliage in the shadow
            // maps. It must differ, or nothing below can see a shadow move.
            std::vector<u8> noFoliageShadowPixels;
            SetFoliageCastsShadows(false);
            (void)RenderAndTimeAtlas(12);
            ASSERT_NO_FATAL_FAILURE(ReadFrame(noFoliageShadowPixels));
            SetFoliageCastsShadows(true);
            (void)RenderAndTimeAtlas(12);

            WriteEvidence(std::string("ShadowAtlasFoliageCull_GL_") + path.Name, batchedPixels);
            WriteEvidence(std::string("ShadowAtlasFoliageCullOff_GL_") + path.Name, singlePixels);
            const PixelDiff arms = DiffPixels(batchedPixels, singlePixels);
            const PixelDiff control = DiffPixels(batchedPixels, controlPixels);
            const PixelDiff foliageShadows = DiffPixels(batchedPixels, noFoliageShadowPixels);
            std::printf("[atlas-foliage] %s: %u atlas faces; ShadowPass/Atlas %.3f ms batched vs %.3f ms in one "
                        "batch; %llu views past the cull slots under the fault; arms differ in %u px (first at "
                        "%u,%u, max delta %u), the batched control pair in %u px (max delta %u); the lamps' foliage "
                        "shadows move %u px\n",
                        path.Name, shadowMap.GetAtlasEntryCount(), batched, single,
                        static_cast<unsigned long long>(afterFault - afterBatched), arms.Count, arms.FirstX,
                        arms.FirstY, arms.MaxDelta, control.Count, control.MaxDelta, foliageShadows.Moved);

            EXPECT_EQ(afterBatched, before) << "a batched region still asked for a cull slot past the last";
            EXPECT_GT(afterFault, afterBatched) << "NEGATIVE CONTROL: the fault did not put views past the cull "
                                                   "slots, so the check above cannot see it";
            EXPECT_LE(control.MaxDelta, kNoiseLevels) << "the batched arm is not repeatable; the comparison means nothing";
            EXPECT_GE(foliageShadows.Moved, kMinFoliageShadowPixels)
                << "POSITIVE CONTROL: the frame shows no foliage shadow from the lamps, so a moved one would go unseen";
            EXPECT_EQ(arms.Moved, 0u) << "batching moved the shadows; the cull is conservative, so it must not";
            ASSERT_TRUE(std::isfinite(batched) && std::isfinite(single)) << "the atlas region was never timed";
        }
    }
} // namespace OloEngine::Tests
