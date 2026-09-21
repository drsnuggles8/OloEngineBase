#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// AnimalBudgetVisualEvidenceTest — issue #1258, on real pixels.
//
// Writes OloEditor/assets/tests/visual/AnimalBudget[Off]_GL_<Path>[_<Angle>].png
//
// The filename carries the {backend} x {path} cell it covers, including the
// backend even though it is always GL here: a reader counting files then cannot
// mistake a complete set of OpenGL captures for a complete verification matrix.
// Every Vulkan cell is live-only — these fixtures need a real GL 4.6 context and
// skip without one — and is evidenced in the PR body.
//
// WHAT A STILL FRAME CANNOT SHOW, AND WHY THIS FILE IS SHAPED AROUND IT.
//
//   * A budget that is working looks, at any one moment, exactly like a scene.
//     So every capture here is a PAIR — the same population, the same camera,
//     with RendererSettings::AnimalSchedulingEnabled the only thing moved — and
//     what is asserted is the RELATION between the two arms.
//
//   * "The hero was preserved" is the whole point and it is the easiest claim
//     to fake. It is asserted as a per-REGION pixel count: the hero occupies a
//     known band of the frame, and its coat pixel count must be unchanged
//     between the arms while the herd's falls. A whole-frame count would let a
//     softened hero hide behind a thinned herd.
//
//   * "No invisible distant coats" is asserted as a COVERAGE FLOOR per capture,
//     not as a difference. A difference assertion cannot catch an empty frame:
//     a herd that vanished entirely also "differs from the control", and would
//     pass the density comparison while being precisely the failure the floor
//     exists to prevent.
//
// The CPU contracts (AnimalSchedulerContractTest) are what pin the allocator;
// this file is what ties them to pixels the GPU actually produced.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/AnimalScheduler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        /// Pixels that are not the clear colour. The coat is much brighter than
        /// the background, so this counts COAT.
        ///
        /// THE FLOOR IS THE ONE THE NEIGHBOURING GROOM EVIDENCE TESTS USE, on
        /// purpose: three tests that disagree about what "coat" means are three
        /// tests nobody can compare.
        [[nodiscard]] u32 CountCoatPixels(const std::vector<u8>& frame, u32 width, u32 x0, u32 x1)
        {
            constexpr int kCoatFloor = 110;
            u32 count = 0;
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            const sizet rows = frame.size() / rowBytes;
            for (sizet y = 0; y < rows; ++y)
            {
                for (u32 x = x0; x < x1 && x < width; ++x)
                {
                    const sizet i = (y * rowBytes) + (static_cast<sizet>(x) * 4u);
                    if (i + 3 >= frame.size())
                    {
                        continue;
                    }
                    if (frame[i] > kCoatFloor && frame[i + 1] > kCoatFloor)
                    {
                        ++count;
                    }
                }
            }
            return count;
        }
    } // namespace

    /// A population: one HERO on the left of frame, close, and a HERD spread
    /// across the right, receding.
    ///
    /// SPLIT LEFT/RIGHT RATHER THAN NEAR/FAR IN DEPTH, and that is what makes
    /// the hero assertion possible at all. The claim is "the hero kept its
    /// quality while the herd lost some", and a hero standing in FRONT of the
    /// herd shares screen pixels with it — so a per-region count could not
    /// attribute a change to one or the other. Two disjoint horizontal bands
    /// can.
    class AnimalBudgetVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        static constexpr u32 kStrands = 12000;
        static constexpr u32 kPoints = 6;
        static constexpr f32 kRadius = 1.0f;
        static constexpr f32 kLength = 0.55f;
        // 5 cm on a 1-unit body: a quill rather than a hair, for
        // GroomLodVisualEvidenceTest's reason — a real hair is sub-pixel at
        // every framing and the OpaqueRibbon tier discards any fragment whose
        // widened alpha falls under the cutoff, which would make every ratio
        // below a ratio of noise.
        static constexpr f32 kStrandWidth = 0.05f;

        static constexpr u32 kHerdCount = 9;
        /// The frame column the hero band ends at / the herd band begins at.
        static constexpr u32 kHeroBandEnd = kWidth / 2u;

        Entity m_Hero;
        std::vector<Entity> m_Herd;
        Ref<GroomAsset> m_Groom;
        AssetHandle m_GroomHandle = 0;

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: AnimalBudgetEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 8.0f, 6.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A PRIMARY RUNTIME CAMERA, far behind the capture poses. Load-
            // bearing for the reason the neighbouring groom evidence fixtures
            // give: Scene::RenderRuntime returns without rendering when nothing
            // carries a primary CameraComponent, so PublishGroomStrandRequests
            // would never run and the budget would never be spent — the fixture
            // would capture a still, unbudgeted frame and pass.
            {
                Entity cameraEntity = scene.CreateEntity("RuntimeCamera");
                cameraEntity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.4f, 200.0f);
                auto& cameraComponent = cameraEntity.AddComponent<CameraComponent>();
                cameraComponent.Primary = true;
                cameraComponent.Camera.SetViewportSize(kWidth, kHeight);
            }

            m_Groom = BuildCoat();
            ASSERT_TRUE(m_Groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(m_Groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            // The hero: left of frame, close, full budget, PROTECTED.
            m_Hero = MakeAnimal("Hero", AnimalRole::Hero, glm::vec3(-5.0f, 0.0f, 0.0f));

            // The herd: right of frame, receding. Spread in depth so the
            // population occupies several rungs of the distance ladder at once,
            // which is the condition under which a population budget does
            // anything at all.
            // A 3x3 GRID RATHER THAN A LINE RECEDING TO 24 m. The first layout
            // put eight of the nine animals far enough away to be a handful of
            // pixels each, so the capture showed the hero and ONE herd member
            // and the word "population" was doing no work in the picture. The
            // grid keeps every animal between about 75 and 130 px — small
            // enough to be background, large enough that a reviewer can see
            // what the budget did to them.
            for (u32 i = 0; i < kHerdCount; ++i)
            {
                const f32 column = static_cast<f32>(i % 3u);
                const f32 row = static_cast<f32>(i / 3u);
                const glm::vec3 position(2.8f + (column * 2.9f), 0.0f, -1.0f - (row * 5.0f));
                m_Herd.push_back(MakeAnimal("Herd", AnimalRole::Background, position));
            }

            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }

        Entity MakeAnimal(const std::string& tag, AnimalRole role, const glm::vec3& position)
        {
            Scene& scene = GetScene();
            Entity entity = scene.CreateEntity(tag);
            entity.GetComponent<TransformComponent>().Translation = position;

            auto& groomComponent = entity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = kStrands;
            // OpaqueRibbon, not the stochastic default: a stochastic coat is a
            // different set of pixels every frame, and this file counts pixels
            // across the arms of an A/B. Holding the composition mode fixed is
            // what makes these numbers about the BUDGET.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            // THE DISTANCE LADDER IS ON, in both arms. The budget takes each
            // animal's ladder answer as its DESIRED step and only ever coarsens
            // from there, so a fixture without a ladder would measure the
            // budget against a workload no shipping scene submits — and the
            // control arm would be a straw man.
            auto& lod = entity.AddComponent<GroomLodComponent>();
            lod.m_Enabled = true;
            lod.m_CardPixelSize = 8.0f; // stay on strands: the tier hand-over is #1252's subject, not this one
            lod.m_MeshPixelSize = 4.0f;
            lod.m_Hysteresis = 0.15f;
            lod.m_HoldFrames = 4;
            // A LOW FULL-RATE THRESHOLD, and this is the fixture's one
            // non-obvious number. At the engine default of 512 px these animals
            // are small enough that the DISTANCE LADDER alone already takes
            // them to its maximum step — so the population costs almost
            // nothing, the budget never has to coarsen anybody, and both arms
            // of the A/B render the identical frame. That is exactly what the
            // first run of this fixture measured: `considered=10 coarsened=0
            // cost=383/1200 herdVisStep=4`, ten animals gathered and nothing to
            // take from any of them.
            //
            // 64 px puts the near herd at full rate, which is the only state
            // from which a BUDGET can be observed taking something away. The
            // ASSERT on AnimalsCoarsened below is what makes that a loud
            // failure rather than a silently vacuous pass.
            lod.m_VisibilityFullPixelSize = 64.0f;
            lod.m_VisibilitySteps = 4;
            lod.m_SimulationFullPixelSize = 48.0f;
            lod.m_ShadowFullPixelSize = 64.0f;
            lod.m_MaxWidthCompensation = 8.0f;

            auto& budget = entity.AddComponent<AnimalBudgetComponent>();
            budget.m_Role = static_cast<u8>(role);
            budget.m_Enabled = true;
            budget.m_FullRateMotionMetres = 0.001f;
            budget.m_MaxVisibilitySteps = 4;
            return entity;
        }

        static Ref<GroomAsset> BuildCoat()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr f32 kGoldenAngle = 2.39996323f;
            constexpr f32 kTwoPi = 6.28318530718f;

            for (u32 s = 0; s < kStrands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kStrands);
                const f32 cosTheta = 1.0f - t;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(kPoints);
                widths.reserve(kPoints);
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1u);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.55f * along * along;
                    points.push_back(point);
                    widths.push_back(kStrandWidth * (1.0f - (0.8f * along)));
                }

                const f32 wrappedU = phi / kTwoPi;
                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { wrappedU - std::floor(wrappedU), t };
                input.GroupId = group;
                input.IsGuide = (s % 16u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("PopulationCoat");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        /// The visibility step the last herd animal is actually running at.
        /// Read back from the Scene rather than predicted, so a disagreement
        /// between what the budget decided and what the coat was built with
        /// shows up here rather than as an unexplained pixel count.
        [[nodiscard]] u32 LastHerdVisibilityStep() const
        {
            if (m_Herd.empty())
            {
                return 0u;
            }
            const auto& schedules = GetScene().GetAnimalSchedules();
            const auto it = schedules.find(m_Herd.back().GetUUID());
            return it != schedules.end() ? it->second.Step[static_cast<sizet>(AnimalWorkAxis::Visibility)] : 0u;
        }

        /// The groom pass's own counters for the frame just captured. The
        /// STRAND COUNT is what the visibility budget actually spends, and it
        /// is the only quantity here that a width compensation cannot hide.
        [[nodiscard]] const GroomRenderStats& PassStats() const
        {
            if (const auto* pass = Renderer3D::GetGroomRenderPass())
            {
                return pass->GetStats();
            }
            static const GroomRenderStats kEmpty{};
            return kEmpty;
        }

        void SetBudget(bool enabled, f32 frameBudgetUnits)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.AnimalSchedulingEnabled = enabled;
            settings.AnimalProtectHero = true;
            settings.AnimalFrameBudgetUnits = frameBudgetUnits;
            settings.AnimalHoldFrames = 0u; // the hold is #1258's contract test; here it must not delay the arm
            settings.AnimalStarvationFrames = 8u;
            settings.AnimalMinVisibleStrands = 256u;
            settings.AnimalMaxPoseStepPixels = 1.0f;
        }

        /// The two capture poses. TWO ANGLES AND NOT ONE, per CLAUDE.md's
        /// rendering rule: a coat that reads correctly head-on can be wrong in
        /// silhouette, and the oblique pose is where a thinned coat's outline
        /// shows. Named in the filename, so an angle that was not captured is a
        /// file missing from the diff.
        struct CapturePose
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };
        // THE CONVENTION, WRITTEN DOWN, because the first oblique pose here was
        // captured with the yaw sign inverted and produced an empty frame that
        // "differed from the control" in every way a careless assertion would
        // have accepted. From EditorCamera::GetOrientation, which builds
        // euler(-pitch, -yaw, 0) and rotates (0, 0, -1):
        //
        //     forward = ( sin(yaw), -sin(pitch), -cos(yaw) )
        //
        // so yaw 0 looks -Z, POSITIVE yaw swings toward +X, and POSITIVE pitch
        // looks DOWN. To aim at a point, yaw = atan2(dx, -dz) and
        // pitch = atan2(-dy, |d_xz|).
        static constexpr CapturePose kFront{ "Front", glm::vec3(0.0f, 1.2f, 14.0f), 0.0f, 0.06f };
        // Eye (-7, 3.0, 12) aimed at roughly (5.0, 0.8, -5): d = (12, -2.2, -17),
        // |d_xz| = 20.8, so yaw = atan2(12, 17) = 0.615 and pitch = atan(2.2/20.8) = 0.105.
        static constexpr CapturePose kOblique{ "Oblique", glm::vec3(-7.0f, 3.0f, 12.0f), 0.615f, 0.105f };

        void Capture(const std::string& saveAs, std::vector<u8>& outPixels, const CapturePose& pose = kFront)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);

            // Several frames: the budget advances once per frame and the
            // ladders settle over the hold, so one frame would capture whatever
            // the previous arm left behind.
            RunEditorFrames(camera, 8);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidence(saveAs, outPixels, kWidth, kHeight);
            }
        }

        // EVIDENCE, not SSIM goldens. What is asserted is the contracts; the
        // PNGs exist so a reviewer can look at what the numbers describe.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels, u32 width, u32 height)
        {
            // "assets/...", NOT "OloEditor/assets/...": renderer initialisation
            // leaves the process's working directory in OloEditor/, so the
            // repo-relative form lands in OloEditor/OloEditor/ — where the write
            // SUCCEEDS, the test passes, and the evidence is somewhere nobody
            // looks. Every neighbouring evidence test writes the same path.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path file = dir / (name + ".png");
            stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
                           static_cast<int>(width * 4u));
        }
    };

    // =========================================================================
    // The budget removes WORK while the picture survives — on every path
    // =========================================================================

    TEST_F(AnimalBudgetVisualEvidenceTest, TheBudgetRemovesStrandWorkWhilePreservingTheCoatAndTheHero)
    {
        // WHAT THIS ASSERTS, AND WHY IT IS NOT "THE HERD GOT THINNER".
        //
        // The first version of this case asserted that the herd's coat pixel
        // count FELL when the budget engaged. It does not, and it must not: the
        // visibility budget spends a strand COUNT, and #1252's width
        // compensation widens each surviving strand by 1/k precisely so the
        // coat's covered area does not move. Measured here, the herd's coverage
        // went 6500 -> 6597 with fifteen sixteenths of its strands removed —
        // inside the 0.98–1.06 band #1252 measured for the compensated arm.
        //
        // So a coat pixel count cannot show the budget working. It shows the
        // COMPENSATION working, which is a different feature's claim. What this
        // file can honestly pin on real pixels is the pair of properties the
        // budget is actually for:
        //
        //   the WORK fell         — StrandsDrawn, straight off the pass
        //   the PICTURE survived  — the herd's coverage held, and the hero's
        //                           did not move at all
        //
        // That is also the coat-authoring rule (§5b) applied honestly: when an
        // A/B changes the geometry, the pixel set is part of the result and not
        // a fixed frame of reference.
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // ── Control arm: the budget off ──────────────────────────────
            SetBudget(false, 6000.0f);
            std::vector<u8> off;
            Capture(std::string("AnimalBudgetOff_GL_") + pathCase.Name + "_Front", off, kFront);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            const u32 heroOff = CountCoatPixels(off, kWidth, 0u, kHeroBandEnd);
            const u32 herdOff = CountCoatPixels(off, kWidth, kHeroBandEnd, kWidth);
            const u32 strandsOff = PassStats().StrandsDrawn;

            // A COVERAGE FLOOR ON THE CONTROL, before any comparison. A
            // difference assertion cannot catch an empty frame: if the fixture
            // drew nothing at all, every ratio below would be a ratio of noise
            // and would happily "prove" the budget worked.
            EXPECT_GT(heroOff, 2000u) << pathCase.Name << ": the control frame has no hero in it";
            EXPECT_GT(herdOff, 2000u) << pathCase.Name << ": the control frame has no herd in it";
            ASSERT_GT(strandsOff, 0u) << pathCase.Name << ": the control frame drew no strands at all";

            // ── Test arm: a budget the population cannot fit inside ──────
            SetBudget(true, 1200.0f);
            std::vector<u8> on;
            Capture(std::string("AnimalBudget_GL_") + pathCase.Name + "_Front", on, kFront);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            const u32 heroOn = CountCoatPixels(on, kWidth, 0u, kHeroBandEnd);
            const u32 herdOn = CountCoatPixels(on, kWidth, kHeroBandEnd, kWidth);
            const u32 strandsOn = PassStats().StrandsDrawn;

            // WHAT THE SCHEDULER ACTUALLY DID, read before any pixel claim is
            // made about it. Without this, "the budget worked" and "the
            // scheduler never ran" produce the same numbers — which is exactly
            // what this fixture's first run measured (considered=10,
            // coarsened=0), because the distance ladder had already taken the
            // herd to its maximum step and left the budget nothing to take.
            const AnimalSchedulerStats& stats = GetScene().GetAnimalSchedulerStats();
            std::printf("[animal-budget-evidence] %-12s hero %6u -> %6u  herd %6u -> %6u  strands %7u -> %7u | "
                        "considered=%u coarsened=%u capHeld=%u cost=%.0f/%.0f herdVisStep=%u\n",
                        pathCase.Name, heroOff, heroOn, herdOff, herdOn, strandsOff, strandsOn,
                        stats.AnimalsConsidered, stats.AnimalsCoarsened, stats.AnimalsCapHeld,
                        static_cast<f64>(stats.EstimatedCostUnits), static_cast<f64>(stats.FrameBudgetUnits),
                        LastHerdVisibilityStep());

            ASSERT_EQ(stats.AnimalsConsidered, kHerdCount + 1u)
                << pathCase.Name
                << ": the scheduler did not gather this population at all, so nothing below is about the budget";
            ASSERT_GT(stats.AnimalsCoarsened, 0u)
                << pathCase.Name
                << ": the budget was engaged and coarsened nobody, so every comparison below would be comparing a "
                   "frame with itself";

            // ── 1. THE WORK FELL. The budget's actual product. ───────────
            EXPECT_LT(strandsOn, strandsOff)
                << pathCase.Name << ": the budget coarsened animals and the pass still built as many strands";
            EXPECT_LT(strandsOn, strandsOff / 2u)
                << pathCase.Name
                << ": fewer than half the strands were removed despite the herd sitting at its visibility cap — "
                   "the step is being decided and not spent";

            // ── 2. THE HERO DID NOT MOVE. Not "close to": the hero is never
            //      a candidate while ProtectHero is set and its ladder answer
            //      is identical in both arms, so the only source of difference
            //      is renderer noise. The 2% band is that noise floor and
            //      nothing else — a hero that lost a halving would drop far
            //      more than that.
            const f64 heroRatio = static_cast<f64>(heroOn) / static_cast<f64>(std::max(heroOff, 1u));
            EXPECT_NEAR(heroRatio, 1.0, 0.02)
                << pathCase.Name << ": the hero's coat changed when the budget engaged — it is not protected";

            // ── 3. THE HERD'S COAT SURVIVED. This is "no invisible distant
            //      coats" as a floor rather than as a difference: a herd that
            //      vanished would satisfy any assertion about work falling,
            //      and it is the failure the whole MinVisibleStrands mechanism
            //      exists to prevent.
            const f64 herdRatio = static_cast<f64>(herdOn) / static_cast<f64>(std::max(herdOff, 1u));
            EXPECT_GT(herdRatio, 0.75)
                << pathCase.Name << ": the herd's coat lost more than a quarter of its coverage. The width "
                                    "compensation is meant to hold it within a few per cent while the strand count "
                                    "falls, so either the compensation is not being applied or the coats fell off "
                                    "the ladder entirely";
            EXPECT_LT(herdRatio, 1.25) << pathCase.Name
                                       << ": the herd's coat gained a quarter of its coverage, which a thinning "
                                          "budget cannot do — the compensation is over-widening";

            // ── The SECOND ANGLE, on the deferred path only ──────────────
            //
            // Abbreviated on purpose and said out loud: the oblique pair is
            // captured for Deferred alone rather than for all three paths,
            // because what the angle adds is a SILHOUETTE read of the thinned
            // coat and that is a property of the geometry, which every path
            // draws from the same build. The three-path sweep above is what
            // covers the lighting paths.
            if (pathCase.Path == RenderingPath::Deferred)
            {
                SetBudget(false, 6000.0f);
                std::vector<u8> obliqueOff;
                Capture("AnimalBudgetOff_GL_Deferred_Oblique", obliqueOff, kOblique);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                const u32 obliqueOffPixels = CountCoatPixels(obliqueOff, kWidth, 0u, kWidth);

                SetBudget(true, 1200.0f);
                std::vector<u8> obliqueOn;
                Capture("AnimalBudget_GL_Deferred_Oblique", obliqueOn, kOblique);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                const u32 obliqueOnPixels = CountCoatPixels(obliqueOn, kWidth, 0u, kWidth);

                std::printf("[animal-budget-evidence] Oblique      coat %6u -> %6u\n", obliqueOffPixels,
                            obliqueOnPixels);

                EXPECT_GT(obliqueOffPixels, 2000u) << "the oblique control frame has no coats in it";
                const f64 obliqueRatio =
                    static_cast<f64>(obliqueOnPixels) / static_cast<f64>(std::max(obliqueOffPixels, 1u));
                EXPECT_GT(obliqueRatio, 0.75) << "the population's coats lost more than a quarter of their coverage "
                                                 "from the oblique angle, where a thinned coat's silhouette reads";
                EXPECT_LT(obliqueRatio, 1.25) << "the population's coats gained a quarter of their coverage from the "
                                                 "oblique angle — the compensation is over-widening";
            }
        }
    }

    // =========================================================================
    // The budget's effect is REVERSIBLE — the control really is a control
    // =========================================================================

    TEST_F(AnimalBudgetVisualEvidenceTest, TurningTheBudgetOffRestoresTheOriginalFrame)
    {
        // The A/B control for every capture in this feature is "turn it off",
        // so off has to be the SAME PICTURE every time rather than whichever
        // allocation the population was last left in. The scheduler resets its
        // state when disabled for exactly this reason; this is what would catch
        // it if it stopped.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        SetBudget(false, 6000.0f);
        std::vector<u8> before;
        Capture("", before);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 herdBefore = CountCoatPixels(before, kWidth, kHeroBandEnd, kWidth);
        const u32 strandsBefore = PassStats().StrandsDrawn;
        ASSERT_GT(herdBefore, 2000u) << "the control frame has no herd in it";
        ASSERT_GT(strandsBefore, 0u) << "the control frame drew no strands at all";

        SetBudget(true, 1200.0f);
        std::vector<u8> squeezed;
        Capture("", squeezed);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 herdSqueezed = CountCoatPixels(squeezed, kWidth, kHeroBandEnd, kWidth);
        // THE SQUEEZE IS CONFIRMED ON THE STRAND COUNT, not on the pixels: the
        // width compensation holds the coat's coverage roughly constant while
        // the count falls, so a pixel assertion here would be asserting the
        // compensation rather than the squeeze. See the work/picture split in
        // TheBudgetRemovesStrandWorkWhilePreservingTheCoatAndTheHero above.
        const u32 strandsSqueezed = PassStats().StrandsDrawn;
        ASSERT_LT(strandsSqueezed, strandsBefore) << "the squeeze arm did nothing, so the restore proves nothing";

        SetBudget(false, 6000.0f);
        std::vector<u8> after;
        Capture("", after);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 herdAfter = CountCoatPixels(after, kWidth, kHeroBandEnd, kWidth);

        std::printf("[animal-budget-evidence] herd off=%u squeezed=%u restored=%u\n", herdBefore, herdSqueezed,
                    herdAfter);

        const f64 restoreRatio = static_cast<f64>(herdAfter) / static_cast<f64>(std::max(herdBefore, 1u));
        EXPECT_NEAR(restoreRatio, 1.0, 0.02)
            << "turning the budget off did not restore the original frame: the scheduler's state survived the "
               "toggle, so the A/B control depends on how long the budget had been on before it";
    }

    // =========================================================================
    // A population that FITS is left exactly alone
    // =========================================================================

    TEST_F(AnimalBudgetVisualEvidenceTest, AnAffordablePopulationIsIndistinguishableFromTheControl)
    {
        // Without this case, "the budget changed the frame" is indistinguishable
        // from "the budget always changes the frame". A scheduler that quietly
        // coarsened everything by one step whatever the pressure would pass
        // every assertion above.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        SetBudget(false, 6000.0f);
        std::vector<u8> off;
        Capture("", off);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 herdOff = CountCoatPixels(off, kWidth, kHeroBandEnd, kWidth);
        ASSERT_GT(herdOff, 2000u) << "the control frame has no herd in it";

        SetBudget(true, 1.0e7f); // an allowance nothing in this scene can exceed
        std::vector<u8> on;
        Capture("", on);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 herdOn = CountCoatPixels(on, kWidth, kHeroBandEnd, kWidth);

        std::printf("[animal-budget-evidence] affordable population: off=%u on=%u\n", herdOff, herdOn);

        const f64 ratio = static_cast<f64>(herdOn) / static_cast<f64>(std::max(herdOff, 1u));
        EXPECT_NEAR(ratio, 1.0, 0.02)
            << "a population well inside its budget was still coarsened, so the scheduler is degrading the frame "
               "whether or not there is any pressure to";
    }
} // namespace OloEngine::Tests
