// OLO_TEST_LAYER: L8
// =============================================================================
// TemporalSubjectSequenceEvidenceTest.cpp — #1256 criteria 3 and 4, measured on
// the REAL subjects through the REAL pipeline.
//
// WHAT THE OTHER #1256 TESTS CANNOT SAY. TemporalReconstructionSequenceTest
// runs the shipping history model over synthetic pixel fields, so it pins the
// arithmetic and nothing else; CoverageChannelEvidenceTest proves a strand's
// coverage reaches RT3's blue lane but reads one settled frame. Both would
// still pass if the temporal resolve made hair sparkle worse, dragged a skin
// highlight across a camera cut, or blurred a meadow to a flat field — because
// none of those defects is visible in a settled frame, which is what criterion
// 3 says outright:
//
//     "Measure and fix ghosting, shimmer and detail loss using motion
//      sequences/AOVs; static accumulated captures are insufficient."
//
// So this fixture captures SEQUENCES off the real render graph and puts
// TemporalSequenceMetrics on them, one instrument per defect:
//
//   * HAIR  -> shimmer.      A stochastically composited coat re-jitters its
//              per-pixel coverage every frame by construction, which is the
//              exact input the coverage dead band exists to ignore. If the
//              dead band were mis-sized, hair would shimmer WORSE with the
//              resolve on than off, and every still capture would look fine.
//   * SKIN  -> ghosting.     A camera cut is where a kept history drags the
//              old view across. Measured against a COLD-HISTORY render of the
//              post-cut pose captured in the same run, never a constant.
//   * FOLIAGE -> detail loss. A resolve can beat both of the above by blurring
//              everything, and retained spatial variance is what catches it.
//              Its motion is a camera dolly rather than wind: the wind measures
//              inert on this layer (0.012% of pixels between on and off), which
//              the motion assertion in that test is what found.
//
// EVERY ASSERTION IS A/B-RELATIVE, never against an absolute level. Two
// properties of this repo make absolutes worthless here: evidence PNG shading
// depends on test order (10k px / 251-delta between an isolated run and a
// broad one), and the stochastic coat is a fresh draw every frame. So each
// test runs BOTH arms in the same process, back to back, and compares them —
// and asserts that the control arm actually moved, because an instrument that
// reads zero on both arms is broken rather than passing.
//
// COLD HISTORY is taken through Renderer3D::InvalidateTemporalHistories, the
// same seam the editor uses for a camera teleport. Re-running frames after a
// settings change is NOT equivalent: the history survives it, so a "TAA off"
// arm captured straight after a "TAA on" arm would start from the other arm's
// accumulated frames.
//
// Evidence lands in OloEditor/assets/tests/visual/TemporalSubject_*.png, and
// the filename carries the cell — backend, path, and the arm — because under
// this repo's convention a cell that did not run must leave a HOLE rather than
// a file. Every name here says GL: the Vulkan cells cannot be captured from a
// headless fixture at all (it needs a real GL 4.6 context and skips without
// one), so they are live-only and the PR body carries them.
//
// Runs in the normal suite and SKIPs (not fails) without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"
#include "VisualEvidenceGuards.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/TemporalSequenceMetrics.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace OloEngine::TemporalSequenceMetrics;

        constexpr u32 kWidth = 320u;
        constexpr u32 kHeight = 240u;

        /// One frame at 60 Hz. Every sequence below advances MOCK time by this
        /// much per frame, so the wind — and anything else driven by the clock —
        /// is a pure function of the frame index. Without it the two arms of an
        /// A/B render at different wall-clock times, the meadow is in a different
        /// pose in each, and the comparison measures the delay between them.
        constexpr f32 kFrameDt = 1.0f / 60.0f;

        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        /// Rec. 709 luma. TemporalSequenceMetrics judges ONE channel over time
        /// on purpose — averaging a hue would let a resolve that shifted the
        /// colour but held the brightness read as perfectly stable — and luma
        /// is the channel every one of these three defects is visible in.
        [[nodiscard]] std::vector<f32> LumaField(const std::vector<u8>& rgba)
        {
            std::vector<f32> luma(rgba.size() / 4u, 0.0f);
            for (std::size_t px = 0u; px < luma.size(); ++px)
            {
                const f32 r = static_cast<f32>(rgba[(px * 4u) + 0u]) * (1.0f / 255.0f);
                const f32 g = static_cast<f32>(rgba[(px * 4u) + 1u]) * (1.0f / 255.0f);
                const f32 b = static_cast<f32>(rgba[(px * 4u) + 2u]) * (1.0f / 255.0f);
                luma[px] = (0.2126f * r) + (0.7152f * g) + (0.0722f * b);
            }
            return luma;
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

        struct CameraPose
        {
            glm::vec3 Eye{ 0.0f };
            f32 Yaw = 0.0f;
            f32 Pitch = 0.0f;
        };
    } // namespace

    class TemporalSubjectSequenceEvidenceTest : public RendererAttachedTest
    {
      public:
        /// The three subjects #1256 names. Exactly one is visible at a time:
        /// the metrics run over the whole frame, so a second subject in shot
        /// would fold its own behaviour into every number.
        enum class Subject
        {
            Skin,
            Hair,
            Foliage,
            /// Nothing drawn. The empty frame every subject-presence check is
            /// measured against — see MeasureSubjectFraction.
            None,
        };

        void BuildScene() override
        {
            SetUpScratchProject();
            EnableRendering(kWidth, kHeight);

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            Scene& scene = GetScene();
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // A side key. A frontal light flattens the terminator, and the
                // terminator is where a temporal defect is easiest to see.
                dl.m_Direction = glm::normalize(glm::vec3(-0.62f, -0.55f, -0.56f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = false;
            }

            BuildSkin(scene);
            BuildHair(scene);
            BuildFoliage(scene);
            ASSERT_TRUE(m_Skin && m_Hair && m_Foliage) << "one of the three subjects failed to build";
            ShowOnly(Subject::Skin);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail, because an ASSERT_* returns from
            // the test body and a reset written there never runs
            // (cross-test-renderer-state.md rule 2). The fixture restores the
            // settings structs wholesale, but the render-target size and the
            // history are renderer-wide state it does not snapshot.
            Time::ClearMockTime();
            // GUARDED, because gtest runs TearDown even when SetUp SKIPPED.
            // RendererAttachedTest::SetUp returns from GTEST_SKIP before it
            // creates the Scene, so on a box with no GL 4.6 context m_Scene is
            // null here — and ResizeRenderTarget dereferences it. That is not
            // a skipped test, it is a segfault that takes the whole binary
            // down, on exactly the machines that cannot run this fixture.
            if (GetSceneRef())
            {
                ResizeRenderTarget(kWidth, kHeight);
                Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
            }
            RendererAttachedTest::TearDown();
        }

      protected:
        // --- scene ----------------------------------------------------------

        void SetUpScratchProject()
        {
            if (Project::GetActive() && Project::HasAssetManager())
                return;

            std::error_code ec;
            const fs::path projectDir = TempDir("temporal-subject-project");
            fs::create_directories(projectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir";

            const fs::path projectFile = projectDir / "TemporalSubject.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: TemporalSubject\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed";
            auto assetManager = Ref<EditorAssetManager>::Create();
            assetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(assetManager);
        }

        void BuildSkin(Scene& scene)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName("TemporalSubjectSkin");
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;
            EXPECT_TRUE(profile->SetParameters(parameters)) << "the skin probe profile needed correcting";
            const AssetHandle profileHandle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
            ASSERT_TRUE(sphere);
            m_SkinMeshSource = sphere->GetMeshSource();
            m_Skin = scene.CreateEntity("SkinSphere");
            m_Skin.AddComponent<MeshComponent>(m_SkinMeshSource);
            auto& material = m_Skin.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(0.62f, 0.48f, 0.42f, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.34f);
            material.m_Material.SetMaterialKind(MaterialKind::Skin);
            material.m_Material.SetSkinProfileHandle(profileHandle);
        }

        void BuildHair(Scene& scene)
        {
            Ref<GroomAsset> groom = BuildThinGroom();
            ASSERT_TRUE(groom);
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(handle), 0u);

            m_Hair = scene.CreateEntity("HairCoat");
            m_Hair.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 0.0f);
            auto& groomComponent = m_Hair.AddComponent<GroomComponent>();
            groomComponent.m_Groom = handle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 4000;
            // StochasticAlpha, deliberately: OpaqueRibbon's hard alpha cutoff
            // discards a genuinely sub-pixel strand outright, and the
            // stochastic mode is the one whose per-frame coverage jitter the
            // dead band exists to ignore. It is therefore the only mode on
            // which the shimmer question below means anything.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::StochasticAlpha);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);
        }

        void BuildFoliage(Scene& scene)
        {
            m_Foliage = scene.CreateEntity("Meadow");
            m_Foliage.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -40.0f, 0.0f);

            auto& terrain = m_Foliage.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 23;
            terrain.m_ProceduralResolution = 96;
            terrain.m_ProceduralOctaves = 4;
            terrain.m_ProceduralFrequency = 1.5f;
            terrain.m_WorldSizeX = 192.0f;
            terrain.m_WorldSizeZ = 192.0f;
            terrain.m_HeightScale = 5.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);

            auto& foliage = m_Foliage.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;

            FoliageLayer meadow;
            meadow.Name = "Meadow";
            meadow.MeshPath = kPineMesh;
            meadow.AlbedoPath = kFoliageAlbedo;
            meadow.Density = 0.45f;
            meadow.SplatmapChannel = -1;
            meadow.MinSlopeAngle = 0.0f;
            meadow.MaxSlopeAngle = 60.0f;
            meadow.MinScale = 1.6f;
            meadow.MaxScale = 2.4f;
            // Unbounded on purpose. These are a terrain-height ACCEPTANCE band,
            // and the first cut of this fixture set them to 4..9 against a
            // height scale of 5 — which admitted only the very tops and put the
            // meadow on 2% of the frame. The subject-presence guard caught it;
            // an "is anything lit" guard would not have.
            meadow.MinHeight = -10000.0f;
            meadow.MaxHeight = 10000.0f;
            meadow.ViewDistance = 400.0f;
            meadow.FadeStartDistance = 370.0f;
            meadow.UseAuthoredMesh = true;
            meadow.MeshViewDistance = 260.0f;
            meadow.MeshFadeStartDistance = 230.0f;
            meadow.AlphaCutoff = 0.25f;
            // MOVING foliage — the subject criterion 3 names. The wind is what
            // makes this a temporal question rather than a still one, and it
            // is deterministic because the whole sequence runs under mock
            // time at a fixed dt.
            // ALL FOUR wind knobs, not just the amplitude. WindStiffness,
            // WindBranchWeight and WindLeafWeight all default to 0, and with
            // them at zero a WindStrength of any size displaces nothing — the
            // meadow renders perfectly still and "moving foliage" becomes a
            // claim in a test name. FoliageWindEvidenceTest uses these values;
            // the wind-motion assertion in the test below is what enforces that
            // they keep working.
            meadow.WindStrength = 2.0f;
            meadow.WindSpeed = 1.0f;
            meadow.WindStiffness = 0.4f;
            meadow.WindBranchWeight = 0.7f;
            meadow.WindLeafWeight = 0.8f;
            meadow.BaseColor = glm::vec3(0.20f, 0.44f, 0.16f);
            foliage.m_Layers.push_back(meadow);
            foliage.m_NeedsRebuild = true;
        }

        /// A small hemisphere of THIN strands — sub-pixel at this framing, so
        /// each is widened to one pixel and pays for it in alpha. That alpha
        /// is the coverage the whole channel exists for, and a width large
        /// enough to make fat strands would make this fixture measure
        /// something else.
        [[nodiscard]] static Ref<GroomAsset> BuildThinGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 1600u;
            constexpr u32 kPoints = 8u;
            constexpr f32 kRadius = 0.6f;
            constexpr f32 kLength = 0.7f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0u; s < kStrands; ++s)
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
                for (u32 p = 0u; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1u);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.75f * along * along;
                    points.push_back(point);
                    widths.push_back(0.006f * (1.0f - (0.5f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0u;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("TemporalSubjectGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            return groom;
        }

        /// Exactly one subject in shot. MeshComponent has no visibility flag,
        /// so the skin sphere is hidden by clearing its MeshSource — the
        /// submission loop skips a null source outright — and restored from
        /// the Ref this fixture kept when it built the sphere.
        void ShowOnly(Subject subject)
        {
            // An ASSERT_* inside BuildSkin/BuildHair/BuildFoliage returns from
            // THAT helper only, so BuildScene carries on and reaches here with
            // a default-constructed Entity. Dereferencing one is a crash, which
            // turns a reportable asset failure into a dead test binary.
            if (!m_Skin || !m_Hair || !m_Foliage)
            {
                ADD_FAILURE() << "a subject failed to build; refusing to pose the scene";
                return;
            }
            m_Skin.GetComponent<MeshComponent>().m_MeshSource =
                (subject == Subject::Skin) ? m_SkinMeshSource : Ref<MeshSource>{};
            m_Hair.GetComponent<GroomComponent>().m_RenderStrands = (subject == Subject::Hair);

            const bool foliage = (subject == Subject::Foliage);
            auto& foliageComponent = m_Foliage.GetComponent<FoliageComponent>();
            foliageComponent.m_Enabled = foliage;
            if (foliage)
                foliageComponent.m_NeedsRebuild = true;
            // PARKED BEYOND THE FAR PLANE rather than merely disabled. Clearing
            // m_ProceduralEnabled does not necessarily discard a terrain mesh
            // that has already been generated, and a distant ground plane left
            // in the bottom of the skin and hair frames would be counted as
            // active pixels by every metric here — still pixels that dilute a
            // shimmer mean without moving it. 5000 units down is past the
            // 2000-unit far clip, so it cannot be drawn at all.
            m_Foliage.GetComponent<TransformComponent>().Translation =
                foliage ? glm::vec3(0.0f, -40.0f, 0.0f) : glm::vec3(0.0f, -5000.0f, 0.0f);
            m_Subject = subject;
        }

        [[nodiscard]] CameraPose PoseFor(Subject subject) const
        {
            switch (subject)
            {
                case Subject::Skin:
                    return { glm::vec3(0.0f, 0.25f, 3.1f), 0.0f, -0.05f };
                case Subject::Hair:
                    return { glm::vec3(0.0f, 0.55f, 2.6f), 0.0f, -0.15f };
                case Subject::Foliage:
                    // Down IN the meadow, not looking across it from above.
                    // The first framing put the camera 10 units up and 28 out,
                    // and the capture came back as mostly terrain — which made
                    // the detail number below a measurement of static ground
                    // rather than of moving foliage. Looking at the PNG is what
                    // caught that; the 20% subject fraction did not, because it
                    // counted the terrain as subject too.
                    return { glm::vec3(0.0f, -33.2f, 10.0f), 0.0f, -0.02f };
                case Subject::None:
                    break;
            }
            return {};
        }

        // --- renderer configuration ----------------------------------------

        void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        static void SetTaa(bool enabled)
        {
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = enabled;
            post.TAAFeedback = 0.9f;
            post.TAASharpness = 0.0f; // a sharpen would add variance back and confound the detail metric
        }

        /// Drop every temporal history, the same seam the editor uses for a
        /// camera teleport. MANDATORY between arms: the history survives a
        /// settings change, so a second arm captured straight after the first
        /// would start from the first arm's accumulated frames and the A/B
        /// would be measuring the order the arms ran in.
        static void ColdHistory()
        {
            Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
        }

        // --- capture ---------------------------------------------------------

        [[nodiscard]] bool CaptureFrame(u32 frame, const CameraPose& pose, u32 width, u32 height,
                                        std::vector<u8>& outRgba)
        {
            // Mock time BEFORE the tick, keyed on the frame index alone, so
            // frame i of one arm and frame i of the other see the identical
            // wind phase.
            Time::SetMockTime(static_cast<f32>(frame) * kFrameDt);
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.05f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            RunEditorFrames(camera, 1, kFrameDt);

            u32 gotWidth = 0u;
            u32 gotHeight = 0u;
            if (!ReadbackComposite(outRgba, gotWidth, gotHeight))
                return false;
            // Checked against what the CALLER asked for, not merely against
            // itself. An upscale cell renders below display resolution and is
            // supposed to come back at it; a silently smaller composite would
            // otherwise be written out under the cell name it did not run at,
            // which is the mistake PR #1403 exists because of.
            if (gotWidth != width || gotHeight != height)
            {
                ADD_FAILURE() << "composite came back " << gotWidth << "x" << gotHeight << ", asked for "
                              << width << "x" << height;
                return false;
            }
            return outRgba.size() == static_cast<std::size_t>(gotWidth) * gotHeight * 4u;
        }

        /// Capture `frames` consecutive frames as luma fields, one render tick
        /// each, so the accumulator advances exactly one step between samples.
        /// `poseAt` is a pure function of the frame index — that is what "under
        /// mock time" means here, and it is why re-running a sequence produces
        /// the same numbers.
        template<typename PoseFn>
        [[nodiscard]] std::vector<std::vector<f32>> CaptureSequence(u32 frames, const PoseFn& poseAt,
                                                                    std::vector<u8>* lastRgba = nullptr,
                                                                    u32 width = kWidth, u32 height = kHeight)
        {
            std::vector<std::vector<f32>> sequence;
            sequence.reserve(frames);
            std::vector<u8> rgba;
            for (u32 frame = 0u; frame < frames; ++frame)
            {
                if (!CaptureFrame(frame, poseAt(frame), width, height, rgba))
                {
                    ADD_FAILURE() << "composite readback failed at frame " << frame;
                    return {};
                }
                sequence.push_back(LumaField(rgba));
            }
            if (lastRgba != nullptr)
                *lastRgba = rgba;
            return sequence;
        }

        /// The two arms of every resolve comparison in this file, captured
        /// back to back with a cold history before each.
        ///
        /// THE CONTROL IS NOT "TAA OFF", AND THAT IS THE WHOLE POINT. The
        /// first cut of this fixture used TAA off and measured a control
        /// shimmer of exactly ZERO. RenderPipeline.cpp says why:
        /// SelectGroomComposition refuses the stochastic mode when no temporal
        /// resolve is running, because a stochastic estimator with nothing to
        /// converge it is just noise. So "TAA off" does not give a stochastic
        /// coat without a resolve — it gives a DIFFERENT COMPOSITION MODE, and
        /// the two arms then differ in two ways at once.
        ///
        /// A feedback of 0 is the arm that actually isolates the variable:
        /// OloTemporalMotionFeedback returns 0, so no history is blended and
        /// the output IS the current frame — while TAAEnabled stays true, so
        /// the coat is still stochastically composited. One variable, and the
        /// resolve's contribution is the ratio between the arms.
        struct ResolveArms
        {
            ShimmerResult NoHistory;
            ShimmerResult Resolved;
            std::vector<u8> NoHistoryLast;
            std::vector<u8> ResolvedLast;
        };

        template<typename PoseFn>
        [[nodiscard]] ResolveArms MeasureResolveArms(u32 frames, const PoseFn& poseAt, u32 skip,
                                                     u32 width = kWidth, u32 height = kHeight)
        {
            ResolveArms arms{};
            const auto settled = [skip](const std::vector<std::vector<f32>>& captured)
            { return std::span<const std::vector<f32>>(captured).subspan(skip); };

            SetTaa(true);
            Renderer3D::GetPostProcessSettings().TAAFeedback = 0.0f;
            ColdHistory();
            const auto noHistory = CaptureSequence(frames, poseAt, &arms.NoHistoryLast, width, height);

            SetTaa(true); // restores the shipping 0.9 feedback
            ColdHistory();
            const auto resolved = CaptureSequence(frames, poseAt, &arms.ResolvedLast, width, height);

            if (noHistory.size() != frames || resolved.size() != frames)
            {
                ADD_FAILURE() << "capture failed: " << noHistory.size() << " / " << resolved.size()
                              << " frames of " << frames;
                return arms;
            }
            arms.NoHistory = MeasureShimmer(settled(noHistory));
            arms.Resolved = MeasureShimmer(settled(resolved));
            return arms;
        }

        /// The fraction of pixels the FOLIAGE changes, measured against the
        /// same shot with the layer switched off and the terrain left in
        /// place.
        ///
        /// MeasureSubjectFraction cannot answer this: it parks the whole
        /// entity, so its "subject" is terrain AND foliage together, and a
        /// frame that is 95% bare ground still scores 100%. Criterion 3 names
        /// MOVING FOLIAGE, so the guard has to be able to tell the plants from
        /// the hill they stand on.
        [[nodiscard]] f64 MeasureFoliageFraction(const CameraPose& pose, u32 width = kWidth,
                                                 u32 height = kHeight)
        {
            auto& foliage = m_Foliage.GetComponent<FoliageComponent>();

            // COLD BEFORE EACH, not merely after both. From the second
            // render-path cell onward the resolve is running at feedback 0.9,
            // so a second capture taken straight after the first is ~90% the
            // FIRST one's history — the two frames converge toward each other
            // and the guard's per-pixel delta is attenuated about tenfold. A
            // presence guard that under-reports is the one kind that fails
            // open.
            std::vector<u8> bare;
            foliage.m_Enabled = false;
            foliage.m_NeedsRebuild = true;
            ColdHistory();
            const bool gotBare = CaptureFrame(0u, pose, width, height, bare);

            std::vector<u8> planted;
            foliage.m_Enabled = true;
            foliage.m_NeedsRebuild = true;
            ColdHistory();
            const bool gotPlanted = CaptureFrame(0u, pose, width, height, planted);
            ColdHistory();

            if (!gotBare || !gotPlanted || bare.size() != planted.size())
                return 0.0;

            std::size_t differing = 0u;
            const std::size_t pixels = planted.size() / 4u;
            for (std::size_t px = 0u; px < pixels; ++px)
            {
                const int dr = std::abs(static_cast<int>(planted[(px * 4u) + 0u]) - static_cast<int>(bare[(px * 4u) + 0u]));
                const int dg = std::abs(static_cast<int>(planted[(px * 4u) + 1u]) - static_cast<int>(bare[(px * 4u) + 1u]));
                const int db = std::abs(static_cast<int>(planted[(px * 4u) + 2u]) - static_cast<int>(bare[(px * 4u) + 2u]));
                if (dr + dg + db > 12)
                    ++differing;
            }
            return pixels == 0u ? 0.0 : static_cast<f64>(differing) / static_cast<f64>(pixels);
        }

        /// The fraction of pixels the subject actually CHANGES, measured
        /// against a capture of the same pose with nothing drawn.
        ///
        /// The first version of this fixture counted pixels whose luma was
        /// above a threshold and got 100% on every cell, because the sky fills
        /// the frame — a guard that can never fail is not a guard. Against an
        /// empty frame the number means what it says: how much of this image
        /// is the subject. Restores the subject and drops the history before
        /// returning, so a caller's sequence still starts cold.
        [[nodiscard]] f64 MeasureSubjectFraction(Subject subject, const CameraPose& pose, u32 width = kWidth,
                                                 u32 height = kHeight)
        {
            // Cold before EACH capture — see MeasureFoliageFraction for why
            // a shared history attenuates this guard about tenfold.
            std::vector<u8> empty;
            ShowOnly(Subject::None);
            ColdHistory();
            const bool gotEmpty = CaptureFrame(0u, pose, width, height, empty);

            std::vector<u8> present;
            ShowOnly(subject);
            ColdHistory();
            const bool gotPresent = CaptureFrame(0u, pose, width, height, present);
            ColdHistory();

            if (!gotEmpty || !gotPresent || empty.size() != present.size())
                return 0.0;

            std::size_t differing = 0u;
            const std::size_t pixels = present.size() / 4u;
            for (std::size_t px = 0u; px < pixels; ++px)
            {
                const int dr = std::abs(static_cast<int>(present[(px * 4u) + 0u]) - static_cast<int>(empty[(px * 4u) + 0u]));
                const int dg = std::abs(static_cast<int>(present[(px * 4u) + 1u]) - static_cast<int>(empty[(px * 4u) + 1u]));
                const int db = std::abs(static_cast<int>(present[(px * 4u) + 2u]) - static_cast<int>(empty[(px * 4u) + 2u]));
                if (dr + dg + db > 12)
                    ++differing;
            }
            return pixels == 0u ? 0.0 : static_cast<f64>(differing) / static_cast<f64>(pixels);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& rgba, u32 width = kWidth,
                             u32 height = kHeight)
        {
            if (rgba.size() != static_cast<std::size_t>(width) * height * 4u)
            {
                ADD_FAILURE() << "refusing to write " << name << ": got " << rgba.size() << " bytes for a "
                              << width << "x" << height << " frame";
                return;
            }
            std::vector<u8> flipped(rgba); // GL readback is bottom-up
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
            for (u32 y = 0u; y < height / 2u; ++y)
            {
                u8* a = flipped.data() + (static_cast<std::size_t>(y) * rowBytes);
                u8* b = flipped.data() + (static_cast<std::size_t>(height - 1u - y) * rowBytes);
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / name).string();
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                       flipped.data(), static_cast<int>(width) * 4),
                      0)
                << "stbi_write_png failed for " << path;
        }

        /// Restores the rendering path whatever way the test leaves
        /// (cross-test-renderer-state.md rule 2). RAII rather than a trailing
        /// assignment because OLO_ENSURE_GPU_OR_SKIP and every ASSERT_* return
        /// from the middle of a test body.
        struct ScopedRenderPath
        {
            explicit ScopedRenderPath(TemporalSubjectSequenceEvidenceTest& owner)
                : m_Owner(owner), m_Path(Renderer3D::GetRendererSettings().Path)
            {
            }
            ~ScopedRenderPath()
            {
                m_Owner.SetPath(m_Path);
            }
            ScopedRenderPath(const ScopedRenderPath&) = delete;
            auto operator=(const ScopedRenderPath&) -> ScopedRenderPath& = delete;

            TemporalSubjectSequenceEvidenceTest& m_Owner;
            RenderingPath m_Path;
        };

        Ref<MeshSource> m_SkinMeshSource;
        Entity m_Skin{};
        Entity m_Hair{};
        Entity m_Foliage{};
        Subject m_Subject = Subject::Skin;
    };

    // -------------------------------------------------------------------------
    // Criterion 3, HAIR — SHIMMER, measured as CONVERGENCE
    // -------------------------------------------------------------------------
    // The headline question of the whole issue, on the subject it was raised
    // for — and the one whose obvious experiment does not work.
    //
    // WHY THERE IS NO "RESOLVE OFF" ARM HERE, AND WHY THAT IS NOT A GAP.
    // The first cut of this test compared TAA on against TAA off and measured
    // the control arm at a frame-to-frame delta of EXACTLY ZERO. The reason is
    // in RenderPipeline.cpp: SelectGroomComposition refuses the stochastic
    // mode when no temporal resolve is running, because a stochastic estimator
    // with nothing to converge it is just noise. So turning TAA off does not
    // give a stochastic coat without a resolve — it gives a DIFFERENT
    // COMPOSITION MODE, and the two arms then differ in two ways at once.
    // "TAA on shimmers more than TAA off" was true, trivially, and said
    // nothing about the coverage dead band.
    //
    // The valid comparison is inside ONE arm: with the resolve running, a
    // stochastic coat must CONVERGE. That is the whole contract. If the
    // coverage term reacted to the estimator's own per-frame noise — the
    // dead-band failure this issue exists to prevent — the resolve would throw
    // history away on exactly the frames history is working, and the delta
    // would PLATEAU at the raw stochastic noise instead of decaying.
    //
    // So: early frames against late frames, same arm, same configuration,
    // nothing differing but how long the accumulator has been running. The
    // early window is the instrument check — if the coat does not move at the
    // start there is nothing to converge and the test is not measuring the
    // estimator at all.
    TEST_F(TemporalSubjectSequenceEvidenceTest, TheResolveCutsAStochasticCoatsShimmerManyFoldOverNoHistory)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kFrames = 32u;
        ShowOnly(Subject::Hair);
        const CameraPose pose = PoseFor(Subject::Hair);
        const auto still = [&pose](u32)
        { return pose; };

        const f64 subject = MeasureSubjectFraction(Subject::Hair, pose);
        EXPECT_GT(subject, 0.02) << "the coat covers only " << subject * 100.0
                                 << "% of the frame against an empty one — it did not draw, so nothing "
                                    "below is measuring the subject.";

        const ResolveArms arms = MeasureResolveArms(kFrames, still, /*skip*/ 8u);
        WritePng("TemporalSubject_Hair_GL_Forward_NoHistory.png", arms.NoHistoryLast);
        WritePng("TemporalSubject_Hair_GL_Forward_Resolved.png", arms.ResolvedLast);

        GTEST_LOG_(INFO) << "hair shimmer: no-history mean " << arms.NoHistory.MeanFrameDelta << " peak "
                         << arms.NoHistory.PeakPixelDelta << " | resolved mean " << arms.Resolved.MeanFrameDelta
                         << " peak " << arms.Resolved.PeakPixelDelta << " | gain "
                         << (arms.Resolved.MeanFrameDelta > 0.0
                                 ? arms.NoHistory.MeanFrameDelta / arms.Resolved.MeanFrameDelta
                                 : 0.0)
                         << "x";

        ASSERT_GT(arms.NoHistory.ComparedPixels, 0u) << "the instrument measured nothing on the control arm";
        ASSERT_GT(arms.Resolved.ComparedPixels, 0u);

        // The instrument: without a history the coat MUST shimmer. If this
        // fails the coat is not stochastically composited and the ratio below
        // is vacuous.
        EXPECT_GT(arms.NoHistory.MeanFrameDelta, 5.0e-3)
            << "the un-resolved coat barely moves frame to frame, so the composition mode is not "
               "stochastic and this test is not measuring what it claims.";

        // The claim. Measured at 11.5x on this box (0.0247 -> 0.00214); 3x is
        // a floor with a wide margin rather than a pinned number, because
        // evidence shading here depends on test order and an absolute would be
        // wrong in a different way on every run.
        //
        // NOTE ON WHAT THIS DOES *NOT* SHOW. CoverageNoiseDeadBand is inert in
        // this scenario — setting it to 0 in PostProcess_TAA.glsl reproduces
        // these numbers byte for byte. That is correct, not a gap: the pass
        // clamps the previous coverage into the previous 3x3 neighbourhood's
        // range before the model sees it, so a resample's delta is already
        // exactly zero and the magnitude dead band has nothing left to do. The
        // dead band is the second line of defence, for when the whole
        // neighbourhood has moved. See docs/agent-rules/temporal-reactivity-separation.md.
        EXPECT_LT(arms.Resolved.MeanFrameDelta, arms.NoHistory.MeanFrameDelta / 3.0)
            << "the resolve cut the coat's shimmer from " << arms.NoHistory.MeanFrameDelta << " only to "
            << arms.Resolved.MeanFrameDelta
            << ". A resolve whose coverage term reacts to the estimator's own per-frame noise lands "
               "exactly here, because it drops history on the frames history is working.";
    }

    // -------------------------------------------------------------------------
    // Criterion 3, SKIN — GHOSTING
    // -------------------------------------------------------------------------
    // A camera cut is where a kept history drags the old view across. The
    // target is a COLD-HISTORY render of the post-cut pose captured in the
    // same run, so the assertion compares two measurements rather than a
    // measurement against a constant that test order would invalidate.
    TEST_F(TemporalSubjectSequenceEvidenceTest, SkinSettlesAfterACameraCutInsteadOfDraggingTheOldView)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kFrames = 20u;
        ShowOnly(Subject::Skin);

        const CameraPose before{ glm::vec3(2.4f, 0.4f, 2.0f), -0.85f, -0.10f };
        const CameraPose after = PoseFor(Subject::Skin);
        const auto atAfter = [&after](u32)
        { return after; };

        const f64 subject = MeasureSubjectFraction(Subject::Skin, after);
        EXPECT_GT(subject, 0.05) << "the skin sphere covers only " << subject * 100.0
                                 << "% of the frame against an empty one — it is not in shot";

        SetTaa(true);

        // The target: what the post-cut pose looks like once settled, with no
        // history carried in from anywhere.
        ColdHistory();
        std::vector<u8> targetRgba;
        const auto targetFrames = CaptureSequence(kFrames, atAfter, &targetRgba);
        ASSERT_EQ(targetFrames.size(), kFrames);
        const std::vector<f32> target = targetFrames.back();

        // Now accumulate a long history at the OLD pose and cut to the new one
        // without invalidating anything. This is the frame sequence a real cut
        // produces, and what the resolve has to recover from.
        ColdHistory();
        const auto atBefore = [&before](u32)
        { return before; };
        const auto warm = CaptureSequence(12u, atBefore);
        ASSERT_EQ(warm.size(), 12u);

        std::vector<u8> cutRgba;
        const auto cutFrames = CaptureSequence(kFrames, atAfter, &cutRgba);
        ASSERT_EQ(cutFrames.size(), kFrames);

        WritePng("TemporalSubject_Skin_GL_Forward_CutTarget.png", targetRgba);
        WritePng("TemporalSubject_Skin_GL_Forward_AfterCut.png", cutRgba);

        // Tolerance is derived from the target's own residual against itself
        // over the last frames — the noise floor this configuration actually
        // has — rather than a hard-coded number that a retune would break.
        const auto targetTail = std::span<const std::vector<f32>>(targetFrames).subspan(kFrames - 4u);
        const GhostingResult floorResult = MeasureGhosting(targetTail, target, 1.0);
        const f64 tolerance = std::max(floorResult.PeakResidual * 3.0, 2.0e-3);

        // BOTH arms measured against the SAME target with the SAME tolerance,
        // which is what makes this a comparison rather than a guess:
        //
        //   * cold — the target run itself, which starts from no history at
        //     all and converges. This is the best any resolve can do at this
        //     pose, and it is measured, not assumed.
        //   * cut  — the same pose reached with a long history of a DIFFERENT
        //     pose behind it.
        //
        // A resolve that drops stale history takes about as long as a cold
        // start. One that drags the old view across takes far longer, and the
        // gap between these two numbers is exactly the ghosting.
        const GhostingResult cold = MeasureGhosting(targetFrames, target, tolerance);
        const GhostingResult ghost = MeasureGhosting(cutFrames, target, tolerance);
        GTEST_LOG_(INFO) << "skin cut: settling " << ghost.SettlingFrames << " frames (cold start "
                         << cold.SettlingFrames << "), peak residual " << ghost.PeakResidual << " (cold "
                         << cold.PeakResidual << "), residual area " << ghost.ResidualArea << " (cold "
                         << cold.ResidualArea << "), final " << ghost.FinalResidual << " (tolerance "
                         << tolerance << ")";

        ASSERT_GT(ghost.ComparedPixels, 0u) << "the instrument measured nothing after the cut";
        ASSERT_GT(cold.ComparedPixels, 0u);
        // No kNeverSettled check on `cold`: its target IS its own final frame,
        // so the last residual is exactly 0 and it settles for any tolerance.
        // Asserting otherwise would read as a noise-floor guard while being
        // incapable of failing. What `cold` is here for is the BASELINE the cut
        // is compared against, which is a real measurement.

        // The instrument: the cut must actually have been a cut. If the two
        // poses render the same thing there is no stale history to drag and
        // the settling number below is trivially zero.
        const DetailResult poseDifference = MeasureDetail(warm.back(), target);
        EXPECT_GT(poseDifference.MeanAbsoluteError, 0.01)
            << "the pre-cut and post-cut poses render almost identically, so this is not a cut";

        // The claim: a cut costs no more than a cold start. The +2 frame
        // allowance is the one frame an off-screen reprojection needs plus a
        // frame of slack, not a tuned constant.
        EXPECT_NE(ghost.SettlingFrames, kNeverSettled)
            << "the resolve never settled after the camera cut.";
        EXPECT_LE(ghost.SettlingFrames, cold.SettlingFrames + 2u)
            << "the resolve took " << ghost.SettlingFrames << " frames to settle after a camera cut against "
            << cold.SettlingFrames
            << " from a cold start — the extra frames ARE the old view being dragged across.";
        EXPECT_LT(ghost.FinalResidual, tolerance)
            << "the resolve settled to a DIFFERENT image than a cold-history render of the same pose.";
    }

    // -------------------------------------------------------------------------
    // Criterion 3, MOVING FOLIAGE — DETAIL LOSS
    // -------------------------------------------------------------------------
    // The defect the other two instruments cannot see. A resolve can beat both
    // shimmer and ghosting by blurring everything to a flat field, and that
    // trade reads as an improvement on every metric but this one.
    //
    // The control arm is a DELIBERATELY OVER-BLURRED resolve — the same
    // configuration at the maximum feedback the shader clamps to — because a
    // retention figure on its own cannot say whether the instrument would have
    // noticed blurring. If the shipping arm does not retain more than the
    // over-blurred one, the number is not measuring blur.
    //
    // Sharpening is OFF in both arms (SetTaa pins TAASharpness to 0). That
    // makes this the WORST case rather than the shipping one: the post-TAA
    // sharpen exists precisely to put some of this variance back, and leaving
    // it on would let a sharpen mask a resolve that had blurred badly.
    TEST_F(TemporalSubjectSequenceEvidenceTest, AMeadowInMotionKeepsItsDetailUnderTheResolve)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Long enough that the over-blur control CONVERGES. At 16 frames it
        // retained 0.894 against the shipping arm's 0.905 — a 1% separation,
        // which is not an instrument. 0.98^48 leaves 38% of the original
        // history weight against 0.9^48's 0.7%, so at this length the two arms
        // are genuinely different resolves rather than the same one caught
        // early.
        constexpr u32 kFrames = 48u;
        ShowOnly(Subject::Foliage);
        const CameraPose pose = PoseFor(Subject::Foliage);

        // THE MOTION IS THE CAMERA, NOT THE WIND, AND THAT IS A MEASURED
        // DECISION RATHER THAN A CONVENIENCE.
        //
        // This layer is configured for wind — WindStrength 2.0 with the
        // stiffness and branch/leaf weights FoliageWindEvidenceTest uses — and
        // it still does not sway: captured with the wind on and with it off at
        // the same instant, 0.012% of pixels differ, and a capture ten seconds
        // of mock time later differs from the first by 0.010%. Both are the
        // noise floor. So the wind displaces nothing measurable on this layer
        // at this configuration, and a "windy meadow" sequence driven by it
        // would have been a still life with a moving name — RetainedFraction
        // near 1 for the trivial reason that there was nothing temporal to
        // lose. That is worth reporting on its own (see the PR); it is a
        // foliage-wind question rather than a temporal-reconstruction one, so
        // it is not chased here.
        //
        // A lateral dolly supplies the motion instead, and it is the HARDER
        // temporal test rather than a weaker substitute: every plant edge
        // reprojects and disoccludes against the one behind it, which is
        // exactly what the history rejection exists to handle. The path is a
        // pure function of the frame index, so both arms traverse it
        // identically and the final frames they are compared on are the same
        // pose.
        const auto dolly = [&pose](u32 frame) -> CameraPose
        {
            CameraPose moved = pose;
            moved.Eye.x += static_cast<f32>(frame) * 0.06f;
            return moved;
        };

        const f64 planted = MeasureFoliageFraction(pose);
        GTEST_LOG_(INFO) << "foliage covers " << planted * 100.0 << "% of the frame (against bare terrain)";
        EXPECT_GT(planted, 0.30)
            << "the PLANTS cover only " << planted * 100.0
            << "% of the frame against the same shot with the layer switched off. The detail number "
               "below would then be measuring the static terrain rather than the moving foliage "
               "criterion 3 names.";

        // The reference: the same frames with no temporal filtering at all.
        // Valid here in a way it is not for hair — the meadow's alpha cutout
        // is deterministic, so turning the resolve off does not change how it
        // is composited, and the wind phase is pinned by mock time in both.
        SetTaa(false);
        ColdHistory();
        std::vector<u8> referenceRgba;
        const auto referenceFrames = CaptureSequence(kFrames, dolly, &referenceRgba);
        ASSERT_EQ(referenceFrames.size(), kFrames);

        SetTaa(true);
        ColdHistory();
        std::vector<u8> resolvedRgba;
        const auto resolvedFrames = CaptureSequence(kFrames, dolly, &resolvedRgba);
        ASSERT_EQ(resolvedFrames.size(), kFrames);

        // The over-blur control: the same resolve at the feedback ceiling.
        SetTaa(true);
        Renderer3D::GetPostProcessSettings().TAAFeedback = 0.98f;
        ColdHistory();
        std::vector<u8> blurredRgba;
        const auto blurredFrames = CaptureSequence(kFrames, dolly, &blurredRgba);
        ASSERT_EQ(blurredFrames.size(), kFrames);
        Renderer3D::GetPostProcessSettings().TAAFeedback = 0.9f;

        WritePng("TemporalSubject_Foliage_GL_Forward_Dolly_TaaOff.png", referenceRgba);
        WritePng("TemporalSubject_Foliage_GL_Forward_Dolly_TaaOn.png", resolvedRgba);
        WritePng("TemporalSubject_Foliage_GL_Forward_Dolly_TaaOverBlurred.png", blurredRgba);

        // MOVING foliage, asserted rather than assumed. A sequence that did not
        // actually move would make every number below a measurement of a still
        // life — RetainedFraction near 1 for the trivial reason that there was
        // nothing temporal to lose. The reference arm is un-resolved, so its
        // frame-to-frame delta IS the motion. This assertion is what caught the
        // wind being inert in the first place.
        const ShimmerResult motion = MeasureShimmer(referenceFrames);
        GTEST_LOG_(INFO) << "meadow motion (un-resolved frame-to-frame): " << motion.MeanFrameDelta;
        ASSERT_GT(motion.ComparedPixels, 0u) << "the motion instrument measured nothing";
        EXPECT_GT(motion.MeanFrameDelta, 1.0e-3)
            << "the un-resolved meadow does not move between frames, so this test is measuring a "
               "still life rather than foliage in motion.";

        const DetailResult detail = MeasureDetail(resolvedFrames.back(), referenceFrames.back());
        const DetailResult blurred = MeasureDetail(blurredFrames.back(), referenceFrames.back());
        GTEST_LOG_(INFO) << "foliage detail: reference variance " << detail.ReferenceVariance << ", measured "
                         << detail.MeasuredVariance << ", retained " << detail.RetainedFraction << ", MAE "
                         << detail.MeanAbsoluteError << " | over-blurred control retained "
                         << blurred.RetainedFraction;

        ASSERT_GT(detail.ComparedPixels, 0u) << "the instrument measured nothing";
        ASSERT_GT(detail.ReferenceVariance, 1.0e-4)
            << "the un-resolved reference is nearly flat, so there was no detail to retain and this "
               "test cannot answer the question it asks.";

        // The instrument, in DIRECTION only, and the margin is itself a
        // finding: pushing the feedback from 0.9 to the 0.98 ceiling costs
        // this subject about 1% of its retained variance (0.742 -> 0.734 under
        // the dolly; 0.904 -> 0.892 on the same shot held still),
        // not the large swing one might expect. The reason is that a meadow's
        // spatial variance is dominated by STATIC structure — the terrain and
        // the plant silhouettes — and temporal averaging can only blur the
        // part that moves. So the feedback knob is a weak blur lever here,
        // and an assertion with a factor on it would be pinning noise.
        //
        // The instrument's blur RESPONSE is pinned properly, and cheaply, on
        // the CPU instead: TemporalSequenceMetricsContract's
        // DetailLossSeparatesBlurFromAFlatReference feeds MeasureDetail a
        // genuinely flattened field and requires RetainedFraction ~ 0. This
        // arm is here to show the GPU path moves the same way, not to
        // re-derive that contract.
        // Strict, with no factor, and the ~1% margin is deliberate rather
        // than careless: both arms render the same scene at the same mock
        // times in the same process, so the only spread between them is driver
        // rounding, and the gap reproduced at 0.008 across runs.
        EXPECT_LT(blurred.RetainedFraction, detail.RetainedFraction)
            << "the over-blurred control retained as much detail as the shipping one (" << blurred.RetainedFraction
            << " vs " << detail.RetainedFraction << "), so this instrument is not measuring blur.";

        // The claim, as a regression floor. The shipping configuration measures
        // ~0.74 under the dolly on this box with sharpening OFF, which is the
        // honest headline: a meadow crossed by a moving camera costs about a
        // quarter of its spatial variance to the resolve, and the post-TAA
        // sharpen this test disables exists to put some of that back. (The
        // same shot held still measures ~0.90, so most of that quarter is the
        // price of MOTION rather than of the resolve idling.) 0.65 sits below
        // the measurement and above the over-blurred control, so it catches a
        // future change that blurs substantially more without pinning today's
        // exact number.
        EXPECT_GT(detail.RetainedFraction, 0.65)
            << "the resolve retained only " << detail.RetainedFraction * 100.0
            << "% of the meadow's spatial variance — it is buying stillness by blurring, which is the "
               "trade the detail instrument exists to catch.";
    }

    // -------------------------------------------------------------------------
    // Criterion 4 — the render-path matrix, artefact-backed
    // -------------------------------------------------------------------------
    // One PNG per cell, named for the cell, so a cell that did not run is a
    // FILE THAT IS NOT IN THE DIFF. The backend is in every name even though
    // it is always GL here: that is what stops a full set of GL captures
    // reading as a full matrix, because the Vulkan cells cannot be captured
    // from a headless fixture at all and are carried live in the PR body.
    //
    // BOTH TAA arms are captured because both are configurations a user can be
    // in, but only the ON arm is measured for convergence — see the hair test
    // for why an OFF arm is not a control for a stochastic subject.
    TEST_F(TemporalSubjectSequenceEvidenceTest, TheResolveRunsOnEveryRenderPathWithBothArmsCaptured)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kFrames = 32u;
        constexpr std::array<RenderingPath, 3> kPaths{ RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                       RenderingPath::Deferred };

        ShowOnly(Subject::Hair);
        const CameraPose pose = PoseFor(Subject::Hair);
        const auto still = [&pose](u32)
        { return pose; };

        const ScopedRenderPath restorePath(*this);

        for (const RenderingPath path : kPaths)
        {
            SetPath(path);
            const std::string cell = std::string("GL_") + PathName(path);

            const f64 subject = MeasureSubjectFraction(Subject::Hair, pose);
            EXPECT_GT(subject, 0.02) << cell << ": the coat covers only " << subject * 100.0
                                     << "% against an empty frame, so this cell proves nothing";

            const ResolveArms arms = MeasureResolveArms(kFrames, still, /*skip*/ 8u);
            WritePng("TemporalSubject_Hair_" + cell + "_NoHistory.png", arms.NoHistoryLast);
            WritePng("TemporalSubject_Hair_" + cell + "_Resolved.png", arms.ResolvedLast);

            const f64 gain = arms.Resolved.MeanFrameDelta > 0.0
                                 ? arms.NoHistory.MeanFrameDelta / arms.Resolved.MeanFrameDelta
                                 : 0.0;
            GTEST_LOG_(INFO) << cell << ": subject " << subject * 100.0 << "%, shimmer "
                             << arms.NoHistory.MeanFrameDelta << " -> " << arms.Resolved.MeanFrameDelta
                             << " (" << gain << "x)";

            ASSERT_GT(arms.NoHistory.ComparedPixels, 0u) << cell;
            ASSERT_GT(arms.Resolved.ComparedPixels, 0u) << cell;
            EXPECT_GT(arms.NoHistory.MeanFrameDelta, 5.0e-3)
                << cell << ": the coat does not move without a history, so it is not stochastically "
                           "composited on this path";
            EXPECT_LT(arms.Resolved.MeanFrameDelta, arms.NoHistory.MeanFrameDelta / 3.0)
                << cell << ": the resolve does not suppress the coat's shimmer on this path ("
                << arms.NoHistory.MeanFrameDelta << " -> " << arms.Resolved.MeanFrameDelta << ")";
        }
    }

    // -------------------------------------------------------------------------
    // Criterion 4 — the conditional axes: upscale, MSAA, non-native resolution
    // -------------------------------------------------------------------------
    // Driven HEADLESS rather than from the editor, and deliberately: #1397 has
    // a non-native UpscaleMode CROPPING the editor viewport instead of scaling
    // it, so a live upscale A/B compares two different framings and cannot
    // answer an image-quality question at all. Here the render target is the
    // whole frame and the comparison is sound.
    TEST_F(TemporalSubjectSequenceEvidenceTest, TheResolveHoldsAcrossUpscaleMsaaAndANonNativeResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kFrames = 32u;
        ShowOnly(Subject::Hair);
        const CameraPose pose = PoseFor(Subject::Hair);
        const auto still = [&pose](u32)
        { return pose; };

        const ScopedRenderPath restorePath(*this);
        auto& post = Renderer3D::GetPostProcessSettings();
        auto& rendererSettings = Renderer3D::GetRendererSettings();

        // One cell. `cell` carries the BACKEND AND PATH as well as the axis,
        // because the MSAA cell runs on Deferred while every other cell here
        // runs on Forward — a file called GL_Forward_Msaa4 written by a
        // deferred capture would be exactly the mislabelling PR #1403 exists
        // because of. The filename is the cell, so it has to be the whole
        // cell.
        const auto measureCell = [&](const std::string& cell, u32 width, u32 height)
        {
            const ResolveArms arms = MeasureResolveArms(kFrames, still, /*skip*/ 8u, width, height);
            if (arms.ResolvedLast.empty())
                return;

            WritePng("TemporalSubject_Hair_" + cell + ".png", arms.ResolvedLast, width, height);

            const f64 gain = arms.Resolved.MeanFrameDelta > 0.0
                                 ? arms.NoHistory.MeanFrameDelta / arms.Resolved.MeanFrameDelta
                                 : 0.0;
            GTEST_LOG_(INFO) << cell << ": shimmer " << arms.NoHistory.MeanFrameDelta << " -> "
                             << arms.Resolved.MeanFrameDelta << " (" << gain << "x)";

            EXPECT_GT(arms.NoHistory.ComparedPixels, 0u) << cell;
            EXPECT_GT(arms.NoHistory.MeanFrameDelta, 5.0e-3)
                << cell << ": the coat does not move without a history in this configuration";
            EXPECT_LT(arms.Resolved.MeanFrameDelta, arms.NoHistory.MeanFrameDelta / 3.0)
                << cell << ": the resolve does not suppress the coat's shimmer in this configuration ("
                << arms.NoHistory.MeanFrameDelta << " -> " << arms.Resolved.MeanFrameDelta << ")";
        };

        SetPath(RenderingPath::Forward);

        // --- Upscale. Spatial (FSR1 EASU) only: UpscalerTechnique::Temporal
        // is FSR2, which FORCES engine TAA off by design — two temporal
        // accumulators fighting over one history is a bug, not a cell — so
        // "TAA on + Temporal" is unreachable and has no arm to measure.
        post.Technique = UpscalerTechnique::Spatial;
        post.Upscale = UpscaleMode::Quality;
        measureCell("GL_Forward_UpscaleQuality", kWidth, kHeight);
        post.Upscale = UpscaleMode::Performance;
        measureCell("GL_Forward_UpscalePerformance", kWidth, kHeight);
        post.Upscale = UpscaleMode::Off;

        // --- MSAA. In this engine it is the DEFERRED G-Buffer's sample count
        // (DeferredSettings::MSAASampleCount) — there is no forward knob — so
        // this cell runs deferred. The sample count the device ACTUALLY
        // returned is what names the cell: PR #1403 exists because an
        // Msaa4-named PNG was written at one sample.
        SetPath(RenderingPath::Deferred);
        const u32 samplesBefore = rendererSettings.Deferred.MSAASampleCount;
        const u32 wanted = std::min(4u, std::max(1u, Renderer3D::GetMaxMSAASamples()));
        rendererSettings.Deferred.MSAASampleCount = wanted;
        Renderer3D::ApplyRendererSettings();
        const u32 samplesUsed = rendererSettings.Deferred.MSAASampleCount;
        GTEST_LOG_(INFO) << "MSAA cell ran at " << samplesUsed << " samples (asked for " << wanted << ")";
        if (samplesUsed > 1u)
        {
            measureCell("GL_Deferred_Msaa" + std::to_string(samplesUsed), kWidth, kHeight);
        }
        else
        {
            // No file, on purpose: under this repo's convention the filename
            // IS the cell, so a cell that did not run must leave a hole.
            ADD_FAILURE() << "MSAA NOT RUN — the device refused a sample count above 1";
        }
        rendererSettings.Deferred.MSAASampleCount = samplesBefore;
        Renderer3D::ApplyRendererSettings();
        SetPath(RenderingPath::Forward);

        // --- A non-native resolution. Criterion 1 names dynamic resolution
        // explicitly, and an odd size is also the cheapest way to catch a
        // pattern that was locked to one pixel grid.
        constexpr u32 kOddWidth = 449u;
        constexpr u32 kOddHeight = 307u;
        ResizeRenderTarget(kOddWidth, kOddHeight);
        measureCell("GL_Forward_NonNativeRes", kOddWidth, kOddHeight);
        ResizeRenderTarget(kWidth, kHeight);
    }
} // namespace OloEngine::Tests
