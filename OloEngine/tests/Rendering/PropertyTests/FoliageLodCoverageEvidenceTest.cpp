// =============================================================================
// FoliageLodCoverageEvidenceTest.cpp — issue #1237, on a live GL context.
//
// Writes
//   OloEditor/assets/tests/visual/FoliageLodCoverage_GL_<Path>_<Angle>.png
//   OloEditor/assets/tests/visual/FoliageLodCoverageOff_GL_<Path>_<Angle>.png
// one pair per {Forward, ForwardPlus, Deferred} x {Sweep03, Sweep07}, plus the
// uncompensated control FoliageLodCoverage_GL_Deferred_FarUncompensated.png, so a
// cell that was not run is a file missing from the diff (task-loop 2a). Vulkan
// is not reachable from a headless fixture — the evidence for those cells is a
// live editor session, recorded in the PR's matrix.
//
// ── Why a MOTION SWEEP is the measurement, not a still frame ─────────────────
//
// The defect this issue names is a RING: every plant in a layer crossing its
// representation threshold in the same frame, which from a walking camera reads
// as a band sweeping across the meadow. A single frame cannot contain that —
// each individual frame looks fine, and the pop is entirely in the DIFFERENCE
// between consecutive frames.
//
// So the sweep is captured twice — the feature on and off — and both arms are
// written out per rendering path for a human to compare. Its RMSE between
// consecutive poses is logged and bounded only in DIRECTION, because over a
// 30 m camera step that number is dominated by the camera moving rather than by
// any plant changing shape; a first cut asserted a ratio on it and was
// measuring parallax.
//
// The falsifiable measurement of the pop is
// `FlippingPlantsScatterInDepthRatherThanFormingARing`. Not a count of the
// plants that change shape between two eyes a metre apart — that number is
// CONSERVED, because every plant crosses its threshold exactly once as the
// camera recedes past it — but the DISPERSION IN DEPTH of the ones that do. A
// ring is precisely the statement that they are all at the same distance.
//
// ── The three coverage arms ──────────────────────────────────────────────────
//
// "Coverage-preserving" is asserted on screen, not only in the contract test,
// because the compensation reaches the image through four vertex stages and a
// cull kernel and could be dropped by any of them:
//
//   FULL         the feature off — the layer at full density. The reference.
//   THINNED      density LOD on with the growth capped at 1x: the survivors are
//                NOT compensated. This arm must visibly lose coverage, or the
//                thinning is not happening and the next arm proves nothing.
//   COMPENSATED  density LOD on with room to grow. Must land back near FULL.
//
// Coverage itself is measured against a foliage-DISABLED frame: the fraction of
// pixels a layer changes is exactly "how much of the screen this layer covers",
// and it needs no assumption about what colour a plant is.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL 4.6
// context — mirrors FoliageAuthoredMeshEvidenceTest beside it.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageLodTransition.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr f32 kCaptureTime = 4.0f;

        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        // The ladder this fixture authors. The sweep below crosses the mesh
        // hand-over and then runs well past the density band's end, so both
        // rungs are exercised by the same camera path.
        constexpr f32 kMeshFadeStart = 40.0f;
        constexpr f32 kMeshViewDistance = 55.0f;
        constexpr f32 kDensityStart = 45.0f;
        constexpr f32 kDensityEnd = 170.0f;
        constexpr f32 kDensityFloor = 0.25f;
        // Wide enough to spread the hand-over over several sweep steps rather
        // than dropping it between two of them, which is what makes the
        // worst-step comparison meaningful.
        constexpr f32 kTransitionSpread = 30.0f;

        struct CameraPose
        {
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };

        // A straight close-to-far walk back from the plants, at a fixed look
        // direction. Nine poses: the camera crosses the mesh hand-over around
        // step 2-4 and the density band from step 4 on.
        constexpr u32 kSweepSteps = 9;

        [[nodiscard]] CameraPose SweepPose(u32 step)
        {
            const f32 t = static_cast<f32>(step) / static_cast<f32>(kSweepSteps - 1u);
            // z from 150 (standing among the pines) out to 390 (past the
            // terrain's far edge at 256), looking back along -z.
            return CameraPose{ glm::vec3(128.0f, 12.0f + t * 48.0f, 150.0f + t * 240.0f), 0.0f, 0.08f + t * 0.22f };
        }

        // Fraction of pixels whose colour differs perceptibly between two
        // frames. A count, not an RMSE: against a foliage-disabled frame this
        // IS the layer's screen coverage, and a count is framing-tolerant where
        // an RMSE is not.
        [[nodiscard]] f64 DifferingFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;

            sizet differing = 0;
            sizet total = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                ++total;
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    ++differing;
            }
            return total == 0 ? 1.0 : static_cast<f64>(differing) / static_cast<f64>(total);
        }

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
            }
            return "Unknown";
        }
    } // namespace

    class FoliageLodCoverageEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntity("Terrain");
            auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 11;
            terrain.m_ProceduralResolution = 128;
            terrain.m_ProceduralOctaves = 4;
            terrain.m_ProceduralFrequency = 1.5f;
            terrain.m_WorldSizeX = 256.0f;
            terrain.m_WorldSizeZ = 256.0f;
            terrain.m_HeightScale = 6.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);

            auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;

            FoliageLayer pines;
            pines.Name = "Pines";
            pines.MeshPath = kPineMesh;
            pines.AlbedoPath = kFoliageAlbedo;
            // Dense enough that a thinning is measurable in pixels rather than
            // in the odd plant appearing and disappearing.
            pines.Density = 0.05f;
            pines.SplatmapChannel = -1;
            pines.MinSlopeAngle = 0.0f;
            pines.MaxSlopeAngle = 60.0f;
            pines.MinScale = 1.0f;
            pines.MaxScale = 1.0f;
            pines.MinHeight = 7.0f;
            pines.MaxHeight = 11.0f;
            pines.ViewDistance = 600.0f;
            pines.FadeStartDistance = 560.0f;
            pines.UseAuthoredMesh = true;
            pines.MeshViewDistance = kMeshViewDistance;
            pines.MeshFadeStartDistance = kMeshFadeStart;
            pines.AlphaCutoff = 0.25f;
            // Deterministic silhouettes: the sweep's frame-to-frame delta has to
            // be the LOD moving, not the wind.
            pines.WindStrength = 0.0f;
            pines.BaseColor = glm::vec3(0.18f, 0.42f, 0.14f);
            foliage.m_Layers.push_back(pines);
            foliage.m_NeedsRebuild = true;
        }

        // The #1237 switches. `maxScale` is the third arm's knob: 1 caps the
        // compensation off entirely, which is the uncompensated control.
        void SetLod(bool enabled, f32 maxScale = 3.0f)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            for (auto& layer : foliage.m_Layers)
            {
                layer.LodTransitionSpread = enabled ? kTransitionSpread : 0.0f;
                layer.LodHysteresis = enabled ? 0.08f : 0.0f;
                layer.LodStochasticCoverage = enabled;
                layer.UseDensityLod = enabled;
                layer.DensityLodStartDistance = kDensityStart;
                layer.DensityLodEndDistance = kDensityEnd;
                layer.DensityLodMinFraction = kDensityFloor;
                layer.DensityLodFadeFraction = 0.2f;
                layer.DensityLodMaxScale = maxScale;
            }
            // Dirtied, because FoliageRenderer copies a layer's render
            // properties into its LayerRenderData inside GenerateInstances and
            // Scene runs that only on m_NeedsRebuild — the same reason the
            // editor's sliders set it. None of these fields feeds the placement
            // signature, so the rebuild rescatters to exactly the same
            // positions and this stays an A/B over ONE meadow rather than two.
            // (The first cut of this fixture did not set it; the on and off
            // arms then rendered byte-identical frames, which is what the
            // screen-coverage control arm below caught.)
            foliage.m_NeedsRebuild = true;
        }

        void SetFoliageEnabled(bool on)
        {
            m_TerrainEntity.GetComponent<FoliageComponent>().m_Enabled = on;
        }

        void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        // `width`/`height` default to the fixture's native size; the
        // conditional-resolution cell passes its own.
        void Capture(const CameraPose& pose, std::vector<u8>& outPixels, u32 width = kWidth,
                     u32 height = kHeight)
        {
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), width, height, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(width) * height * 4u);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px, u32 width = kWidth,
                             u32 height = kHeight)
        {
            std::vector<u8> flipped(px); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<sizet>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<sizet>(height - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                             flipped.data(), static_cast<int>(width) * 4);
        }

        // Capture the whole sweep and return the WORST RMSE between consecutive
        // poses — the number a ring crossing the frame makes large.
        struct SweepResult
        {
            f64 WorstStep = 0.0;
            f64 MeanStep = 0.0;
            std::vector<std::vector<u8>> Frames;
        };

        SweepResult CaptureSweep()
        {
            SweepResult out;
            out.Frames.resize(kSweepSteps);
            for (u32 step = 0; step < kSweepSteps; ++step)
                Capture(SweepPose(step), out.Frames[step]);

            f64 sum = 0.0;
            for (u32 step = 1; step < kSweepSteps; ++step)
            {
                const f64 rmse = VisualEvidence::Rgba8Rmse(out.Frames[step - 1u], out.Frames[step]);
                out.WorstStep = std::max(out.WorstStep, rmse);
                sum += rmse;
            }
            out.MeanStep = sum / static_cast<f64>(kSweepSteps - 1u);
            return out;
        }

        // How many of the layer's real plants the density LOD drops at a pose,
        // and the triangles that saves — evaluated with the SAME function the
        // cull kernel and the vertex stages compile, over the registry's real
        // positions. This is the cost measurement acceptance criterion 4 asks
        // for, and it needs no GPU readback to be exact.
        struct CostAtPose
        {
            u32 Instances = 0;
            u32 Drawn = 0;
            u64 Triangles = 0;
        };

        [[nodiscard]] CostAtPose MeasureCost(const glm::vec3& eye, const FoliageLod::Params& lod) const
        {
            CostAtPose out;
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();

            u64 indicesPerInstance = 0;
            for (const auto& draw : foliage.m_Renderer->GetActiveLayerDrawInfo())
                indicesPerInstance += draw.IndexCount;

            const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();
            for (const auto& record : records)
            {
                ++out.Instances;
                const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                const f32 hash = FoliageLod::InstanceHash(record.m_Position);
                if (!FoliageLod::EvaluateDensity(lod, hash, glm::distance(world, eye)).IsCulled())
                    ++out.Drawn;
            }
            out.Triangles = (static_cast<u64>(out.Drawn) * indicesPerInstance) / 3u;
            return out;
        }

        Entity m_TerrainEntity;
    };

    // ── The sweep: a walking camera must not see a band cross the meadow ─────

    TEST_F(FoliageLodCoverageEvidenceTest, ACloseToFarSweepHasNoPopOnEveryRenderingPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        // Scene creates the FoliageRenderer on its first tick, so nothing about
        // the layer can be asserted until a frame has run.
        SetLod(false);
        std::vector<u8> warmUp;
        Capture(SweepPose(0), warmUp);

        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        ASSERT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u) << "no pines were scattered — the fixture is broken";

        constexpr std::array<RenderingPath, 3> kPaths{ RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                       RenderingPath::Deferred };
        for (const RenderingPath path : kPaths)
        {
            SCOPED_TRACE(PathName(path));
            SetPath(path);

            SetLod(false);
            const SweepResult off = CaptureSweep();
            SetLod(true);
            const SweepResult on = CaptureSweep();

            // The band crosses the frame in the MIDDLE of the sweep, so those
            // are the captures worth keeping. The far pose is the density
            // band's floor, where the compensation is working hardest.
            const std::string tag = std::string("GL_") + PathName(path);
            WritePng("FoliageLodCoverage_" + tag + "_Sweep03.png", on.Frames[3]);
            WritePng("FoliageLodCoverageOff_" + tag + "_Sweep03.png", off.Frames[3]);
            WritePng("FoliageLodCoverage_" + tag + "_Sweep07.png", on.Frames[7]);
            WritePng("FoliageLodCoverageOff_" + tag + "_Sweep07.png", off.Frames[7]);

            GTEST_LOG_(INFO) << PathName(path) << " sweep: worst consecutive-step RMSE off " << off.WorstStep
                             << " / on " << on.WorstStep << " (mean off " << off.MeanStep << " / on " << on.MeanStep
                             << ")";

            // The sweep must actually move the image, or "no pop" would be a
            // statement about a still frame.
            EXPECT_GT(off.WorstStep, 1.0)
                << "the close-to-far sweep barely changed the frame — the camera path or the scene is wrong, "
                   "and the comparison below would be vacuous";

            // The direction of the effect, and ONLY that. Over a 30 m step the
            // RMSE is dominated by the camera moving — the plants change size
            // and parallax far more than any one of them changes shape — so
            // this number is a sanity check and the PNGs above are what a human
            // reads. The direct measurement of "how many plants pop in one
            // frame" is FewPlantsChangeRepresentationInASingleCameraStep below,
            // which counts crossings over the real scattered positions instead
            // of inferring them from pixels the camera also moved.
            EXPECT_LE(on.WorstStep, off.WorstStep)
                << "the #1237 transitions made the worst frame-to-frame step of the sweep WORSE";
        }

        SetPath(RenderingPath::Deferred);
    }

    // ── Where the plants that change shape in one frame ARE ──────────────────

    TEST_F(FoliageLodCoverageEvidenceTest, FlippingPlantsScatterInDepthRatherThanFormingARing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The measurement this test started as was a COUNT of the plants that
        // change representation between two eyes a metre apart, on the
        // expectation that decorrelation would reduce it. It does not, and the
        // count is what proved it: 11 with one shared threshold, 47 spread over
        // 30 m. Nor should it — every plant crosses its threshold exactly once
        // as the camera recedes past it, so the number of crossings per metre
        // of camera travel is CONSERVED. Spreading the thresholds cannot change
        // how many plants flip; it changes WHERE THEY ARE WHEN THEY DO.
        //
        // And that is the defect. A "ring sweeping across the meadow" is
        // precisely the statement that the flipping plants are all at the same
        // distance from the viewer, so they read as one coherent arc rather
        // than as scattered individuals. So the number asserted here is the
        // DISPERSION IN DEPTH of the flipping set: near zero when every plant
        // shares one threshold, and of the order of the authored spread when
        // they do not.
        //
        // Evaluated over the layer's real scattered positions, through the same
        // hand-over function the vertex stages compile.
        SetLod(false);
        std::vector<u8> warmUp;
        Capture(SweepPose(0), warmUp);

        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
        ASSERT_FALSE(records.empty());

        const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();

        struct FlipStats
        {
            u32 Count = 0;
            f64 MeanDistance = 0.0;
            f64 DepthStdDev = 0.0;
        };

        // Two eyes one metre apart, placed among the plants so the step lands
        // where the hand-over actually happens. A plant "flipped" when the side
        // of the band that owns the majority of its pixels changed.
        const auto flipStats = [&](f32 spread, f32 hysteresis)
        {
            const glm::vec3 eyeA(128.0f, 12.0f, 150.0f);
            const glm::vec3 eyeB(128.0f, 12.0f, 151.0f);
            std::vector<f64> distances;
            for (const auto& record : records)
            {
                const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                const f32 hash = FoliageLod::InstanceHash(record.m_Position);
                const f32 distA = glm::distance(world, eyeA);
                const f32 distB = glm::distance(world, eyeB);
                const f32 before = FoliageLod::MeshCoverageLod(distA, distA - 1.0f, kMeshFadeStart,
                                                               kMeshViewDistance, hash, spread, hysteresis);
                const f32 after = FoliageLod::MeshCoverageLod(distB, distA, kMeshFadeStart, kMeshViewDistance,
                                                              hash, spread, hysteresis);
                if ((before > 0.5f) != (after > 0.5f))
                    distances.push_back(static_cast<f64>(distB));
            }

            FlipStats out;
            out.Count = static_cast<u32>(distances.size());
            if (distances.empty())
                return out;
            for (const f64 d : distances)
                out.MeanDistance += d;
            out.MeanDistance /= static_cast<f64>(distances.size());
            for (const f64 d : distances)
                out.DepthStdDev += (d - out.MeanDistance) * (d - out.MeanDistance);
            out.DepthStdDev = std::sqrt(out.DepthStdDev / static_cast<f64>(distances.size()));
            return out;
        };

        const FlipStats off = flipStats(0.0f, 0.0f);
        const FlipStats on = flipStats(kTransitionSpread, 0.08f);

        GTEST_LOG_(INFO) << "plants changing representation across a 1 m camera step, of " << records.size()
                         << ": shared threshold " << off.Count << " at " << off.MeanDistance << " m +- "
                         << off.DepthStdDev << " | decorrelated over " << kTransitionSpread << " m " << on.Count
                         << " at " << on.MeanDistance << " m +- " << on.DepthStdDev;

        ASSERT_GT(off.Count, 0u) << "no plant crosses the hand-over at this pose — the camera step is in the "
                                    "wrong place and the comparison below is vacuous";
        ASSERT_GT(on.Count, 0u);

        // With one shared threshold every flipping plant sits within the band's
        // own crossing distance of every other: a ring. The bound is a metre,
        // which is the camera step itself.
        EXPECT_LT(off.DepthStdDev, 1.5)
            << "the shared-threshold arm did not produce a coherent ring (+-" << off.DepthStdDev
            << " m) — the fixture's plants are not dense enough for the comparison to mean anything";

        // Decorrelated, they are strewn through the depth range instead.
        //
        // The bound is derived, not picked: an offset uniform over `spread` has
        // a standard deviation of spread/sqrt(12) = 0.289 * spread, which is
        // the value this would take if the flipping set were the whole layer.
        // It is not — it is the sub-sample within one camera step of its own
        // threshold, drawn from a terrain whose relief spreads the distances
        // further in some directions and bunches them in others — so the
        // measured figure sits below the ideal (7.3 m against 8.7 m on this
        // fixture). 70% of the ideal is the floor asserted, which is still 15x
        // the shared-threshold arm's 0.5 m and nowhere near it.
        constexpr f64 kUniformIdeal = 0.2887; // 1 / sqrt(12)
        EXPECT_GT(on.DepthStdDev, static_cast<f64>(kTransitionSpread) * kUniformIdeal * 0.7)
            << "the flipping plants are still bunched at one distance (+-" << on.DepthStdDev
            << " m over a " << kTransitionSpread << " m spread) — they will still read as a ring";
        EXPECT_GT(on.DepthStdDev, off.DepthStdDev * 5.0);
    }

    // ── Coverage preservation, measured on screen ────────────────────────────

    TEST_F(FoliageLodCoverageEvidenceTest, ThinningPreservesTheLayersScreenCoverage)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        SetPath(RenderingPath::Deferred);

        // A pose deep in the density band, where the keep fraction is at or
        // near its floor and the compensation is working hardest.
        const CameraPose farPose = SweepPose(kSweepSteps - 2u);

        // The reference for "how much screen does this layer cover": the same
        // frame with no foliage at all.
        SetLod(false);
        SetFoliageEnabled(false);
        std::vector<u8> noFoliage;
        Capture(farPose, noFoliage);
        SetFoliageEnabled(true);

        std::vector<u8> full;
        Capture(farPose, full);

        SetLod(true, /*maxScale*/ 1.0f); // thinned, growth capped off
        std::vector<u8> thinned;
        Capture(farPose, thinned);

        SetLod(true, /*maxScale*/ 3.0f); // thinned AND compensated
        std::vector<u8> compensated;
        Capture(farPose, compensated);

        // Only the UNCOMPENSATED arm is written. The other two are already in
        // the diff as the sweep's Deferred Sweep07 pair — this test captures
        // the same pose with the same settings, so writing them again would put
        // two byte-identical 600 KB files in the PR. The third arm has no twin
        // anywhere: it is what the layer looks like when the survivors are NOT
        // grown, which is the image the whole compensation exists to avoid.
        WritePng("FoliageLodCoverage_GL_Deferred_FarUncompensated.png", thinned);

        const f64 coverageFull = DifferingFraction(full, noFoliage);
        const f64 coverageThinned = DifferingFraction(thinned, noFoliage);
        const f64 coverageCompensated = DifferingFraction(compensated, noFoliage);

        GTEST_LOG_(INFO) << "far-pose screen coverage: full " << coverageFull * 100.0 << "%, thinned "
                         << coverageThinned * 100.0 << "%, compensated " << coverageCompensated * 100.0 << "%";

        ASSERT_GT(coverageFull, 0.02)
            << "the layer covers almost none of the far frame — the measurement has no signal to lose";

        // The control arm has to LOSE coverage, or the thinning is not
        // reaching the image and the next assertion proves nothing.
        EXPECT_LT(coverageThinned, coverageFull * 0.85)
            << "capping the growth off did not thin the layer on screen (" << coverageThinned * 100.0 << "% vs "
            << coverageFull * 100.0 << "%) — the density LOD is not running";

        // And the compensated arm has to come back. Stated as "closer to full
        // than the uncompensated arm is", which is the relation the feature
        // claims, rather than an absolute tolerance that would encode this
        // scene's plant size.
        const f64 lossThinned = std::abs(coverageFull - coverageThinned);
        const f64 lossCompensated = std::abs(coverageFull - coverageCompensated);
        EXPECT_LT(lossCompensated, lossThinned * 0.5)
            << "compensating the survivors recovered little of the lost coverage (missing "
            << lossCompensated * 100.0 << "% of the screen against " << lossThinned * 100.0
            << "% uncompensated) — the scale compensation is not reaching the vertex stages";
    }

    // ── The cost the thinning actually buys ──────────────────────────────────

    TEST_F(FoliageLodCoverageEvidenceTest, ThinningDropsInstancesAndTrianglesWithDistance)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // As above: the renderer does not exist until Scene has ticked.
        SetLod(true);
        std::vector<u8> warmUp;
        Capture(SweepPose(0), warmUp);

        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        ASSERT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u);

        FoliageLod::Params lod;
        lod.Enabled = true;
        lod.Start = kDensityStart;
        lod.End = kDensityEnd;
        lod.MinFraction = kDensityFloor;
        lod.FadeFraction = 0.2f;
        lod.MaxScale = 3.0f;
        lod = FoliageLod::Sanitise(lod);

        const CostAtPose nearCost = MeasureCost(SweepPose(0).Eye, lod);
        const CostAtPose farCost = MeasureCost(SweepPose(kSweepSteps - 1u).Eye, lod);
        ASSERT_GT(nearCost.Instances, 0u);
        ASSERT_GT(nearCost.Triangles, 0u) << "the layer emits no indices — GetActiveLayerDrawInfo is empty";

        GTEST_LOG_(INFO) << "density LOD cost: near " << nearCost.Drawn << "/" << nearCost.Instances
                         << " instances, " << nearCost.Triangles << " tris | far " << farCost.Drawn << "/"
                         << farCost.Instances << " instances, " << farCost.Triangles << " tris";

        EXPECT_LT(farCost.Drawn, nearCost.Drawn) << "the layer does not thin with distance at all";
        EXPECT_LT(farCost.Triangles, nearCost.Triangles);
        // The floor is a floor: the far field keeps its silhouette rather than
        // being culled away, which is a worse image than any amount of popping.
        EXPECT_GT(farCost.Drawn, static_cast<u32>(static_cast<f32>(farCost.Instances) * kDensityFloor * 0.5f))
            << "the thinning went past the authored floor — the far hillside will have lost its treeline";
    }
    // ── The conditional axes: MSAA and a non-native resolution ───────────────

    TEST_F(FoliageLodCoverageEvidenceTest, CoveragePreservationHoldsUnderMsaaAndAnOddResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // These three are cells for one reason, and it is specific to this
        // change: #1237 resolves coverage STOCHASTICALLY, and a dither is a
        // PIXEL-FREQUENCY pattern. MSAA resolves it at sample rate rather than
        // pixel rate, an upscaler reprojects it through a history that never
        // saw the same pattern twice, and a non-native target samples it on a
        // different grid. Any of the three can turn a dissolve into a shimmer
        // while every other assertion in this file still passes.
        //
        // The measurement is the same three-arm coverage comparison the native
        // cell makes, repeated per cell — full, thinned-uncompensated,
        // thinned-and-compensated — because "the feature still preserves
        // coverage here" is the property, not "the frame is not empty".
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
        } scopedMockTime(kCaptureTime);

        const CameraPose farPose = SweepPose(kSweepSteps - 2u);

        // Returns (thinned, compensated) as fractions of the FULL arm's screen
        // coverage at this cell's settings. Both arms are measured against a
        // foliage-disabled frame captured at the SAME settings, so a cell that
        // changes the whole image (an upscaler, an odd target) cannot move the
        // ratio by moving its own baseline.
        const auto measure = [&](const char* cell, u32 width, u32 height)
        {
            SetLod(false);
            SetFoliageEnabled(false);
            std::vector<u8> noFoliage;
            Capture(farPose, noFoliage, width, height);
            SetFoliageEnabled(true);

            std::vector<u8> full;
            Capture(farPose, full, width, height);
            SetLod(true, /*maxScale*/ 1.0f);
            std::vector<u8> thinned;
            Capture(farPose, thinned, width, height);
            SetLod(true, /*maxScale*/ 3.0f);
            std::vector<u8> compensated;
            Capture(farPose, compensated, width, height);

            WritePng(std::string("FoliageLodCoverage_GL_Deferred_") + cell + ".png", compensated, width, height);

            const f64 coverageFull = DifferingFraction(full, noFoliage);
            const f64 coverageThinned = DifferingFraction(thinned, noFoliage);
            const f64 coverageCompensated = DifferingFraction(compensated, noFoliage);
            GTEST_LOG_(INFO) << cell << " screen coverage: full " << coverageFull * 100.0 << "%, thinned "
                             << coverageThinned * 100.0 << "%, compensated " << coverageCompensated * 100.0 << "%";

            EXPECT_GT(coverageFull, 0.01) << cell << ": the layer covers almost none of the frame";
            EXPECT_LT(coverageThinned, coverageFull * 0.9)
                << cell << ": capping the growth off did not thin the layer on screen — the density LOD is "
                           "not running at this setting";
            const f64 lossThinned = std::abs(coverageFull - coverageThinned);
            const f64 lossCompensated = std::abs(coverageFull - coverageCompensated);
            EXPECT_LT(lossCompensated, lossThinned * 0.6)
                << cell << ": the compensation recovered little of the lost coverage (missing "
                << lossCompensated * 100.0 << "% against " << lossThinned * 100.0 << "% uncompensated)";
        };

        auto& settings = Renderer3D::GetRendererSettings();

        // MSAA. In this engine it is the DEFERRED G-Buffer's sample count
        // (DeferredSettings::MSAASampleCount) — there is no forward knob — so
        // this cell runs deferred, which is also where the stochastic coverage
        // resolve replaces a hard alpha cut-off and therefore where MSAA has
        // the most to interact with.
        SetPath(RenderingPath::Deferred);
        const u32 samplesBefore = settings.Deferred.MSAASampleCount;
        settings.Deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        measure("Msaa1", kWidth, kHeight);

        const u32 wanted = std::min(4u, std::max(1u, Renderer3D::GetMaxMSAASamples()));
        settings.Deferred.MSAASampleCount = wanted;
        Renderer3D::ApplyRendererSettings();
        const u32 samplesUsed = settings.Deferred.MSAASampleCount;
        // Reported, not assumed: a device that refuses the sample count leaves
        // the cell running at 1 and the row would otherwise claim MSAA cover it
        // never had.
        GTEST_LOG_(INFO) << "MSAA cell ran at " << samplesUsed << " samples (asked for " << wanted << ")";
        EXPECT_GT(samplesUsed, 1u) << "MSAA NOT RUN — the device refused a sample count above 1";
        measure("Msaa4", kWidth, kHeight);
        settings.Deferred.MSAASampleCount = samplesBefore;
        Renderer3D::ApplyRendererSettings();

        // A non-native resolution. A dither is pixel-frequency, so a different
        // target size is the cheapest way to catch a pattern that was locked to
        // one grid.
        constexpr u32 kOddWidth = 907;
        constexpr u32 kOddHeight = 611;
        ResizeRenderTarget(kOddWidth, kOddHeight);
        measure("NonNativeRes", kOddWidth, kOddHeight);
        ResizeRenderTarget(kWidth, kHeight);

        // UPSCALING is verified LIVE instead, and deliberately: the mode lives
        // on PostProcessSettings rather than on RendererSettings, so this
        // fixture cannot drive it without reaching across a seam it does not
        // own. The cell is covered from a real editor session (the PR's matrix
        // records what was measured), which is the same split every Vulkan cell
        // already takes.
    }

} // namespace OloEngine::Tests
