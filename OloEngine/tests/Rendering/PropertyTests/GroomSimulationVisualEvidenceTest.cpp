#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomSimulationVisualEvidenceTest — issue #1250, all four acceptance criteria.
//
// Writes OloEditor/assets/tests/visual/GroomSimulation[Off]_GL_<Path>[_<Angle>].png
//
// The filename carries the {backend} x {path} cell it covers, deliberately
// including the backend even though it is always GL here: a reader counting
// files then cannot mistake a complete set of OpenGL captures for a complete
// verification matrix. Every Vulkan cell is live-only — these fixtures need a
// real GL 4.6 context and skip without one — and is evidenced in the PR body.
//
// A STILL FRAME PROVES NOTHING ABOUT A SOLVER, which is the whole reason this
// file is shaped the way it is. At rest, a simulated coat and an un-simulated
// one are the SAME PICTURE by construction — that is a guarantee of the feature,
// not a weakness of the test — so every capture here is taken AFTER a motion
// sequence, and the A/B control is "simulation off", the same coat on the same
// moving body with the solver switched off.
//
// The motion is driven through Scene::OnUpdateRuntime (RunFrames), because the
// simulation deliberately does not advance in edit mode (a scene must not change
// just from being open). The multi-ANGLE captures then go through
// RunEditorFrames, which advances no simulation time — so all three angles show
// the identical solved pose rather than three poses a few frames apart.
//
// The contracts asserted beyond the PNGs are the ones that can be wrong while
// the picture still looks like fur:
//
//   1. Motion CHANGES the coat, on all three rendering paths, measured against
//      the same motion with the solver off.
//   2. Length is preserved within the DECLARED tolerance for the whole run, not
//      just at the end — GroomRenderStats::WorstStretchRatio against
//      DeclaredStretchTolerance.
//   3. The frame rate does not change the answer: the same motion integrated at
//      30, 60 and 144 Hz lands in the same place.
//   4. A teleport RE-SEEDS rather than stretching.
//   5. The body proxy actually catches the coat (SimulationContacts), and
//      turning collision off lets it through.
//   6. The pass REPORTS the simulation, so a frame that changed for some other
//      reason cannot be mistaken for one that moved.
//
// WHAT THESE CAPTURES DO NOT SHOW: the BODY. Same fixture limit as
// GroomBindingVisualEvidenceTest — a runtime-built primitive MeshSource with a
// Skeleton is not picked up by the animated submission path — and the same
// mitigation: the numbers are measured on the CPU from the state the frame was
// built from, and the "coat on a visible animating body" picture comes from the
// live editor, which the Vulkan cells need anyway.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomGuideSimulation.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstddef>
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

        // The same floor GroomBindingVisualEvidenceTest and
        // GroomStrandVisualEvidenceTest use, on purpose: three neighbouring
        // evidence tests that disagree about what "changed" means are three
        // tests nobody can compare.
        constexpr int kMovedPixelThreshold = 12;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 differing = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(frame[i]) - static_cast<int>(baseline[i]));
                const int dg = std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(baseline[i + 1]));
                const int db = std::abs(static_cast<int>(frame[i + 2]) - static_cast<int>(baseline[i + 2]));
                if (dr > kMovedPixelThreshold || dg > kMovedPixelThreshold || db > kMovedPixelThreshold)
                {
                    ++differing;
                }
            }
            return differing;
        }
        /// Pixels that are not the clear colour. The coat is much brighter than
        /// the background, so this counts coat.
        ///
        /// THE ASSERTION THIS EXISTS FOR: an empty frame differs from a full one
        /// by far more than any "these two angles differ" threshold, so a
        /// difference test alone passes happily when the camera is pointing at
        /// nothing. That is exactly what happened here -- the first Side pose
        /// had the yaw sign wrong and looked away from the coat, and the angle
        /// test passed on a frame with two dozen stray strands in it. A floor on
        /// COVERAGE is what makes a mis-aimed camera a failure.
        [[nodiscard]] u32 CountCoatPixels(const std::vector<u8>& frame)
        {
            // The clear colour is a flat mid grey around 85/255; the coat is
            // authored at 0.72/0.60/0.46 and lit, so it lands well above it. 110
            // is comfortably between the two and nowhere near either.
            constexpr int kCoatFloor = 110;
            u32 count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                if (frame[i] > kCoatFloor && frame[i + 1] > kCoatFloor)
                {
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    class GroomSimulationVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        Entity m_BodyEntity;
        Entity m_GroomEntity;
        Ref<Skeleton> m_Skeleton;
        Ref<MeshSource> m_BodySurface;
        Ref<GroomAsset> m_Groom;
        Ref<GroomBindingAsset> m_Binding;
        AssetHandle m_GroomHandle = 0;
        AssetHandle m_BindingHandle = 0;

        // A LONG coat, because the thing being evidenced is swing: a 3 cm pelt
        // moves by less than a strand width and no capture of it could show the
        // difference between a correct solver and none at all.
        static constexpr u32 kStrands = 2400;
        static constexpr u32 kPoints = 12;
        static constexpr f32 kLength = 1.30f;
        static constexpr f32 kStrandWidth = 0.012f;
        static constexpr u32 kGuideEvery = 12;

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
                            "  Name: GroomSimulationEvidence\n"
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
            // OFF: the debug views are a diagnostic, and a capture whose subject
            // is where a silhouette moved to must not have guide polylines drawn
            // over it. That the flag turns them off is itself part of what this
            // file evidences.
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A PRIMARY RUNTIME CAMERA, and it is load-bearing rather than
            // decoration: Scene::RenderRuntime returns without rendering when no
            // entity carries a primary CameraComponent, so RenderScene3D never
            // runs, PublishGroomStrandRequests is never called, and the solver
            // never steps. The motion this whole file is about is driven through
            // RunFrames, so without this every capture would be of a coat
            // standing exactly at its groomed shape -- which is also what a
            // correct "simulation off" control looks like, so the A/B would pass
            // while measuring nothing at all.
            {
                Entity cameraEntity = scene.CreateEntity("RuntimeCamera");
                // FAR BEHIND the capture poses, and that is the point. The editor
                // draws a gizmo for a camera entity that ShowComponentGizmos does
                // NOT gate, so a runtime camera sitting where the capture camera
                // sits puts an L-shaped icon in the middle of every evidence
                // frame. It was there, it moved the pixel counts, and it is not
                // in #1249's captures next door -- which is how it was caught.
                // Sixty units back still looks down -Z at the coat, so the
                // runtime frames render it and the solver still steps.
                cameraEntity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.9f, 60.0f);
                auto& cameraComponent = cameraEntity.AddComponent<CameraComponent>();
                cameraComponent.Primary = true;
                cameraComponent.Camera.SetViewportSize(kWidth, kHeight);
            }

            BuildSkinnedBody(scene);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            ASSERT_TRUE(m_BodySurface);
            ASSERT_TRUE(m_Skeleton);
            ASSERT_TRUE(m_BodyEntity);

            InstallCoat();
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }

        // The same hinged sphere GroomBindingVisualEvidenceTest uses, and for
        // the same reason: a hard bone split makes "did the strand go where its
        // triangle went" a question about the binding rather than about a blend.
        void BuildSkinnedBody(Scene& scene)
        {
            Ref<Mesh> mesh = MeshPrimitives::CreateSphere();
            ASSERT_TRUE(mesh);
            m_BodySurface = mesh->GetMeshSource();
            ASSERT_TRUE(m_BodySurface);

            m_Skeleton = Ref<Skeleton>::Create();
            m_Skeleton->m_BoneNames = { "root", "upper" };
            m_Skeleton->m_ParentIndices = { -1, 0 };
            m_Skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
            m_Skeleton->m_PrevFinalBoneMatrices = { glm::mat4(1.0f), glm::mat4(1.0f) };
            m_BodySurface->SetSkeleton(m_Skeleton);

            auto& influences = m_BodySurface->GetBoneInfluences();
            const auto& vertices = m_BodySurface->GetVertices();
            // RESIZED, not asserted equal — see the long note in
            // GroomBindingVisualEvidenceTest: CreateSphere leaves the influence
            // array at its pre-allocated size, and a partial stream reads as
            // UNSKINNED, which would silently make this whole fixture measure
            // nothing.
            influences.SetNum(vertices.Num());
            for (i32 i = 0; i < vertices.Num(); ++i)
            {
                BoneInfluence influence;
                influence.m_BoneIDs[0] = vertices[i].Position.y > 0.0f ? 1u : 0u;
                influence.m_Weights[0] = 1.0f;
                influences[i] = influence;
            }
            ASSERT_EQ(influences.Num(), vertices.Num());
            m_BodySurface->Build();

            m_BodyEntity = scene.CreateEntity("Body");
            m_BodyEntity.GetComponent<TransformComponent>().Scale = glm::vec3(0.98f);
            auto& mc = m_BodyEntity.AddComponent<MeshComponent>();
            mc.m_MeshSource = m_BodySurface;
            auto& mat = m_BodyEntity.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.10f, 0.09f, 0.09f, 1.0f));
            m_BodyEntity.AddComponent<SkeletonComponent>(m_Skeleton);
        }

        void InstallCoat()
        {
            Scene& scene = GetScene();
            m_Groom = BuildCoat();
            ASSERT_TRUE(m_Groom);
            ASSERT_GT(m_Groom->GetGuideCount(), 0u) << "a coat with no guides cannot be simulated at all";

            std::vector<u8> bytes;
            GroomBindingBuildStats buildStats;
            std::string reason;
            ASSERT_TRUE(GroomBindingCooker::CookPair(*m_Groom, BodyView(), "EvidenceBody",
                                                     GroomBindingBuildSettings{}, bytes, m_Binding, buildStats,
                                                     reason))
                << reason;
            Ref<GroomBindingAsset> loaded;
            ASSERT_TRUE(GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, reason))
                << reason;
            m_Binding = loaded;

            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(m_Groom);
            m_BindingHandle = AssetManager::AddMemoryOnlyAsset<GroomBindingAsset>(m_Binding);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);
            ASSERT_NE(static_cast<u64>(m_BindingHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = kStrands;
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            auto& binding = m_GroomEntity.AddComponent<GroomBindingComponent>();
            binding.m_Binding = m_BindingHandle;
            binding.m_TargetEntity = m_BodyEntity.GetUUID();
            binding.m_Enabled = true;

            auto& simulation = m_GroomEntity.AddComponent<GroomSimulationComponent>();
            simulation.m_Enabled = true;
            // A DELIBERATELY SOFT coat. The defaults are tuned for fur that holds
            // its groom, and a coat that holds its groom is a coat whose motion a
            // capture cannot show. This is the authoring choice a long tail plume
            // would make, and it is what makes the A/B pair below differ by tens
            // of thousands of pixels instead of by hundreds.
            simulation.m_Stiffness = 18.0f;
            simulation.m_Damping = 2.5f;
            simulation.m_Collide = true;
            simulation.m_ColliderPadding = 0.01f;
        }

        static Ref<GroomAsset> BuildCoat()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr f32 kRadius = 1.0f;
            constexpr f32 kGoldenAngle = 2.39996323f;

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
                    point.y -= kLength * 0.75f * along * along;
                    points.push_back(point);
                    widths.push_back(kStrandWidth * (1.0f - (0.8f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % kGuideEvery) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("SimulatedLongCoat");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        [[nodiscard]] GroomSurfaceView BodyView() const
        {
            GroomSurfaceView view;
            const auto& vertices = m_BodySurface->GetVertices();
            const auto& indices = m_BodySurface->GetIndices();
            view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
            view.PositionStride = static_cast<u32>(sizeof(Vertex));
            view.VertexCount = static_cast<u32>(vertices.Num());
            view.Indices = indices.GetData();
            view.IndexCount = static_cast<u32>(indices.Num());
            view.BoneCount = static_cast<u32>(m_Skeleton->m_FinalBoneMatrices.size());
            u64 hash = 1469598103934665603ull;
            for (const auto& name : m_Skeleton->m_BoneNames)
            {
                for (const char c : name)
                {
                    hash ^= static_cast<u64>(static_cast<unsigned char>(c));
                    hash *= 1099511628211ull;
                }
                hash ^= 0xFFull;
                hash *= 1099511628211ull;
            }
            view.SkeletonNameHash = hash;
            return view;
        }

        void SetBodyPose(f32 degrees)
        {
            glm::mat4 bend = glm::rotate(glm::mat4(1.0f), glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
            m_Skeleton->m_PrevFinalBoneMatrices = m_Skeleton->m_FinalBoneMatrices;
            m_Skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f), bend };
        }

        /// The benchmark motion: the body swings through +-35 degrees at about
        /// 1 Hz. One shape, driven at whatever frame rate the caller asks for,
        /// so the frame-rate-independence case below is comparing the same
        /// physical motion and not two different ones.
        ///
        /// What a motion run measured.
        ///
        /// The stats are the LAST MOTION FRAME'S, captured inside the loop and
        /// returned, NOT read from the pass afterwards. A Capture() is a
        /// zero-step editor frame, so every per-step counter the pass reports
        /// after one -- contacts resolved, steps taken -- is zero. Reading them
        /// after a capture was the first shape of this helper and it made the
        /// collision assertion below measure nothing while looking like it
        /// measured everything.
        struct MotionResult
        {
            GroomRenderStats Last;
            /// The worst |stretch - 1| over the WHOLE run. A solver that is
            /// inextensible only on the last frame is not inextensible.
            f32 WorstStretch = 1.0f;
            /// Summed over the run, for the same reason: a settled coat
            /// resolves nothing on the frame you happen to look at.
            u32 ContactsEver = 0;
        };

        MotionResult RunBenchmarkMotion(f32 seconds, f32 hz)
        {
            MotionResult result;
            const f32 dt = 1.0f / hz;
            const u32 frames = static_cast<u32>(seconds * hz);
            for (u32 frame = 0; frame < frames; ++frame)
            {
                const f32 time = static_cast<f32>(frame) * dt;
                SetBodyPose(35.0f * std::sin(time * 6.2831853f));
                RunFrames(1, dt);
                result.Last = PassStats();
                result.ContactsEver += result.Last.SimulationContacts;
                if (std::abs(result.Last.WorstStretchRatio - 1.0f) > std::abs(result.WorstStretch - 1.0f))
                {
                    result.WorstStretch = result.Last.WorstStretchRatio;
                }
            }
            return result;
        }

        /// Reset the simulation to a known start: the groomed pose, zero
        /// velocity, the body upright. Used between arms of an A/B so one arm
        /// cannot inherit the other's momentum — the trap
        /// backend-ab-needs-an-identical-camera names, in the time dimension.
        void ResetMotion()
        {
            SetBodyPose(0.0f);
            ++m_GroomEntity.GetComponent<GroomSimulationComponent>().m_ResetKey;
            RunFrames(1, 1.0f / 60.0f);
        }

        void SetSimulationEnabled(bool enabled)
        {
            m_GroomEntity.GetComponent<GroomSimulationComponent>().m_Enabled = enabled;
        }

        [[nodiscard]] const GroomRenderStats& PassStats() const
        {
            if (const auto* pass = Renderer3D::GetGroomRenderPass())
            {
                return pass->GetStats();
            }
            static const GroomRenderStats kEmpty{};
            return kEmpty;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // EDITOR frames, which advance NO simulation time (see the file
            // header). Every angle of one capture set therefore shows the
            // identical solved pose, rather than three poses a few frames apart.
            RunEditorFrames(camera, 2);

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
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               pixels.data(), static_cast<int>(width) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }
    };

    // -------------------------------------------------------------------------
    // Criterion 1 + 3: the coat moves, on every path, and it does not stretch
    // -------------------------------------------------------------------------

    TEST_F(GroomSimulationVisualEvidenceTest, SimulationMovesTheCoatOnEveryRenderPath)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const PathCase paths[] = { { "Forward", RenderingPath::Forward },
                                   { "ForwardPlus", RenderingPath::ForwardPlus },
                                   { "Deferred", RenderingPath::Deferred } };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // The control arm FIRST, so the simulated arm cannot inherit a
            // partly-solved pose from it.
            SetSimulationEnabled(false);
            ResetMotion();
            const MotionResult offMotion = RunBenchmarkMotion(1.0f, 60.0f);
            EXPECT_EQ(offMotion.Last.GroomsSimulated, 0u)
                << pathCase.Name << ": the control arm must not be simulated at all";
            std::vector<u8> off;
            Capture(std::string("GroomSimulationOff_GL_") + pathCase.Name, glm::vec3(0.0f, 0.9f, 4.6f), 0.0f,
                    0.10f, off);

            SetSimulationEnabled(true);
            ResetMotion();
            const MotionResult onMotion = RunBenchmarkMotion(1.0f, 60.0f);
            std::vector<u8> on;
            Capture(std::string("GroomSimulation_GL_") + pathCase.Name, glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f,
                    on);

            const u32 differing = CountDifferingPixels(on, off);
            std::printf("[groom-sim] %-12s  %u px moved  guides %u  points %u  strands %u (unguided %u)  "
                        "contacts %u  worst |stretch-1| %.6f (tolerance %.4f)  worst rest deviation %.4f m\n",
                        pathCase.Name, differing, onMotion.Last.GuidesSimulated, onMotion.Last.GuidePointsSimulated,
                        onMotion.Last.StrandsSimulated, onMotion.Last.StrandsUnguided, onMotion.ContactsEver,
                        static_cast<f64>(std::abs(onMotion.WorstStretch - 1.0f)),
                        static_cast<f64>(onMotion.Last.DeclaredStretchTolerance),
                        static_cast<f64>(onMotion.Last.WorstRestDeviation));

            // 1. The pass REPORTS it, so a frame that changed for some other
            //    reason cannot be mistaken for one that moved.
            EXPECT_EQ(onMotion.Last.GroomsSimulated, 1u) << pathCase.Name;
            EXPECT_GT(onMotion.Last.GuidesSimulated, 0u) << pathCase.Name;
            EXPECT_GT(onMotion.Last.StrandsSimulated, 0u)
                << pathCase.Name << ": rendered strands must take their motion from the guides";
            EXPECT_EQ(onMotion.Last.StrandsUnguided, 0u)
                << pathCase.Name << ": every strand of a single-group coat has guides";

            // 2. The picture actually differs. A long coat swinging through 70
            //    degrees moves a large fraction of its own silhouette; 2000 px
            //    is far below that and far above any dithering floor.
            EXPECT_GT(differing, 2000u)
                << pathCase.Name << ": simulation on and off produced the same picture";

            // 3. Criterion 1, as a number, over the WHOLE run.
            EXPECT_LE(std::abs(onMotion.WorstStretch - 1.0f), onMotion.Last.DeclaredStretchTolerance)
                << pathCase.Name << ": worst stretch ratio " << onMotion.WorstStretch;

            // 4. It really moved, rather than merely being declared simulated.
            EXPECT_GT(onMotion.Last.WorstRestDeviation, 0.0f) << pathCase.Name;

            EXPECT_FALSE(onMotion.Last.SimulationStepsClamped)
                << pathCase.Name << ": a fixed 60 Hz run must never be in arrears";
        }

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
    }

    // -------------------------------------------------------------------------
    // Criterion 1: the frame rate does not change the answer
    // -------------------------------------------------------------------------

    // The failure this is written against: an integrator fed a raw variable dt
    // makes the coat's stiffness a function of the frame rate, so the same asset
    // hangs differently on a fast machine. That is invisible in any single
    // capture, and it is the reason the solver runs on a fixed step.
    TEST_F(GroomSimulationVisualEvidenceTest, TheFrameRateDoesNotChangeTheAnswer)
    {
        struct Arm
        {
            f32 Hz;
            const char* Suffix;
        };
        const Arm arms[] = { { 30.0f, "Hz30" }, { 60.0f, "Hz60" }, { 144.0f, "Hz144" } };

        std::vector<u8> reference;
        f32 referenceDeviation = 0.0f;
        for (const Arm& arm : arms)
        {
            ResetMotion();
            const MotionResult motion = RunBenchmarkMotion(1.0f, arm.Hz);
            std::vector<u8> frame;
            Capture(std::string("GroomSimulation_GL_Deferred_") + arm.Suffix, glm::vec3(0.0f, 0.9f, 4.6f), 0.0f,
                    0.10f, frame);
            const GroomRenderStats& stats = motion.Last;

            std::printf("[groom-sim] %6.0f Hz  worst |stretch-1| %.6f  worst rest deviation %.4f m  steps %u\n",
                        static_cast<f64>(arm.Hz), static_cast<f64>(std::abs(motion.WorstStretch - 1.0f)),
                        static_cast<f64>(stats.WorstRestDeviation), stats.SimulationSteps);

            EXPECT_LE(std::abs(motion.WorstStretch - 1.0f), stats.DeclaredStretchTolerance) << arm.Hz << " Hz";

            if (reference.empty())
            {
                reference = frame;
                referenceDeviation = stats.WorstRestDeviation;
                continue;
            }

            // The motion ENDS in the same place, within a tolerance that is
            // about the sampling of the driving pose rather than about the
            // solver: 30 Hz samples the body's swing eight times less finely
            // than 144 Hz does, so the poses the coat is solved against are
            // genuinely not identical. A relative bound on the rest deviation
            // is the honest comparison; equality of the two pictures is not.
            const f32 relative = std::abs(stats.WorstRestDeviation - referenceDeviation) /
                                 std::max(referenceDeviation, 1.0e-4f);
            EXPECT_LT(relative, 0.35f)
                << arm.Hz << " Hz: rest deviation " << stats.WorstRestDeviation << " vs " << referenceDeviation
                << " at 30 Hz -- the coat's stiffness must not be a function of the frame rate";
        }
    }

    // -------------------------------------------------------------------------
    // Criterion 1: teleport and pause
    // -------------------------------------------------------------------------

    TEST_F(GroomSimulationVisualEvidenceTest, ATeleportReseedsRatherThanStretching)
    {
        ResetMotion();
        (void)RunBenchmarkMotion(0.6f, 60.0f);
        ASSERT_GT(PassStats().WorstRestDeviation, 0.0f) << "the coat must be genuinely off its groom first";

        std::vector<u8> before;
        Capture("GroomSimulation_GL_Deferred_PreTeleport", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f, before);

        // A hundred metres in one frame, which is far past the default
        // one-metre threshold.
        m_GroomEntity.GetComponent<TransformComponent>().Translation += glm::vec3(100.0f, 0.0f, 0.0f);
        m_BodyEntity.GetComponent<TransformComponent>().Translation += glm::vec3(100.0f, 0.0f, 0.0f);
        RunFrames(1, 1.0f / 60.0f);

        const GroomRenderStats stats = PassStats();
        EXPECT_EQ(stats.SimulationReseeds, 1u) << "a teleport must re-seed";
        // Re-seeded means AT the groomed shape, exactly: the frame draws the
        // groomed coat and emits zero motion rather than dragging the tips a
        // hundred metres through the level.
        EXPECT_EQ(stats.WorstRestDeviation, 0.0f);
        EXPECT_LE(std::abs(stats.WorstStretchRatio - 1.0f), stats.DeclaredStretchTolerance);

        std::vector<u8> after;
        Capture("GroomSimulation_GL_Deferred_PostTeleport", glm::vec3(100.0f, 0.9f, 4.6f), 0.0f, 0.10f, after);
        // The coat is back at its groomed shape, so the post-teleport frame is
        // NOT the pre-teleport one. Asserted because "it reset" and "it did
        // nothing" produce the same counters if the re-seed never reached the
        // geometry.
        EXPECT_GT(CountDifferingPixels(after, before), 1000u);
    }

    // A paused frame holds the pose it paused on. Driven through the same
    // pause gate the editor's pause button uses, not through a zero dt, so what
    // is tested is the thing that actually happens.
    TEST_F(GroomSimulationVisualEvidenceTest, PauseHoldsTheSolvedPose)
    {
        ResetMotion();
        (void)RunBenchmarkMotion(0.6f, 60.0f);
        const f32 deviationBefore = PassStats().WorstRestDeviation;
        ASSERT_GT(deviationBefore, 0.0f);

        std::vector<u8> paused;
        Capture("GroomSimulation_GL_Deferred_Paused", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f, paused);

        GetScene().SetPaused(true);
        for (u32 frame = 0; frame < 30u; ++frame)
        {
            RunFrames(1, 1.0f / 60.0f);
            const GroomRenderStats stats = PassStats();
            EXPECT_EQ(stats.SimulationSteps, 0u) << "a paused frame must take no steps";
            EXPECT_EQ(stats.SimulationReseeds, 0u) << "a pause is not a discontinuity";
        }
        EXPECT_EQ(PassStats().WorstRestDeviation, deviationBefore) << "the pose must be held exactly";
        GetScene().SetPaused(false);
    }

    // -------------------------------------------------------------------------
    // Criterion 2: body collision
    // -------------------------------------------------------------------------

    TEST_F(GroomSimulationVisualEvidenceTest, TheBodyProxyCatchesTheCoat)
    {
        auto& simulation = m_GroomEntity.GetComponent<GroomSimulationComponent>();

        simulation.m_Collide = false;
        ResetMotion();
        const MotionResult openMotion = RunBenchmarkMotion(1.0f, 60.0f);
        EXPECT_EQ(openMotion.ContactsEver, 0u) << "collision off must resolve nothing";
        std::vector<u8> through;
        Capture("GroomSimulationOff_GL_Deferred_Collision", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f, through);

        simulation.m_Collide = true;
        // The proxy is fitted from the body's own vertices, so the lever that
        // makes it catch a long coat is the radius, not a placement.
        simulation.m_ColliderRadiusScale = 1.15f;
        ResetMotion();
        const MotionResult caughtMotion = RunBenchmarkMotion(1.0f, 60.0f);
        std::vector<u8> caught;
        Capture("GroomSimulation_GL_Deferred_Collision", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f, caught);

        std::printf("[groom-sim] collision  %u contacts  %u px changed  worst |stretch-1| %.6f\n",
                    caughtMotion.ContactsEver, CountDifferingPixels(caught, through),
                    static_cast<f64>(std::abs(caughtMotion.WorstStretch - 1.0f)));

        EXPECT_GT(caughtMotion.ContactsEver, 0u)
            << "the fitted proxy must actually catch a coat swinging against the body";
        EXPECT_GT(CountDifferingPixels(caught, through), 500u)
            << "collision must change the picture, not only a counter";
        // The declared trade: collision is resolved BEFORE the length
        // projection, so length is still exact while contacts are being
        // resolved. This is the assertion that would fail if the order were
        // ever inverted.
        EXPECT_LE(std::abs(caughtMotion.WorstStretch - 1.0f),
                  caughtMotion.Last.DeclaredStretchTolerance);
    }

    // -------------------------------------------------------------------------
    // Multiple angles and resolutions
    // -------------------------------------------------------------------------

    // Strand width is sub-pixel, so a lower-resolution capture genuinely changes
    // coverage — the resolution axis is a real one here, not a formality.
    TEST_F(GroomSimulationVisualEvidenceTest, CapturesFromSeveralAnglesAndResolutions)
    {
        ResetMotion();
        (void)RunBenchmarkMotion(1.0f, 60.0f);

        struct Angle
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        // EditorCamera's orientation is euler(-pitch, -yaw, 0), so with yaw 0 the
        // forward direction is -Z. Looking at the origin from +X therefore needs
        // yaw = -pi/2, NOT +pi/2 -- the positive quarter turn points the camera
        // away from the coat, which is how the first version of this test
        // captured an almost empty frame and still passed.
        const Angle angles[] = { { "Side", glm::vec3(4.6f, 0.9f, 0.0f), -1.5708f, 0.10f },
                                 { "Front", glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f },
                                 { "Above", glm::vec3(0.0f, 4.2f, 2.6f), 0.0f, 1.016f } };

        std::vector<u8> first;
        for (const Angle& angle : angles)
        {
            std::vector<u8> frame;
            Capture(std::string("GroomSimulation_GL_Deferred_") + angle.Name, angle.Position, angle.Yaw,
                    angle.Pitch, frame);
            ASSERT_FALSE(frame.empty()) << angle.Name;

            // EVERY angle must actually contain the coat. 20 000 px is about
            // 2 % of the frame -- an order of magnitude below what any of these
            // poses really captures, and an order of magnitude above the stray
            // strands a mis-aimed camera catches at the edge.
            const u32 coverage = CountCoatPixels(frame);
            std::printf("[groom-sim] angle %-6s %u coat px\n", angle.Name, coverage);
            EXPECT_GT(coverage, 20000u)
                << angle.Name << ": the camera is not pointing at the coat";

            if (first.empty())
            {
                first = frame;
            }
            else
            {
                // Three angles that are the same picture would mean the camera
                // never moved, which is how a multi-angle set silently becomes
                // one angle captured three times.
                EXPECT_GT(CountDifferingPixels(frame, first), 5000u) << angle.Name;
            }
        }

        // Half resolution. The coat is the same; the coverage is not.
        ResizeRenderTarget(kWidth / 2u, kHeight / 2u);
        RunFrames(1, 1.0f / 60.0f);
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth / 2u), static_cast<f32>(kHeight / 2u));
            camera.SetPose(glm::vec3(0.0f, 0.9f, 4.6f), 0.0f, 0.10f);
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            ASSERT_TRUE(fb);
            std::vector<u8> half;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth / 2u, kHeight / 2u, half);
            ASSERT_EQ(half.size(), static_cast<sizet>(kWidth / 2u) * (kHeight / 2u) * 4u);
            const sizet rowBytes = static_cast<sizet>(kWidth / 2u) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < (kHeight / 2u) / 2u; ++y)
            {
                u8* top = half.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bot = half.data() + (static_cast<sizet>((kHeight / 2u) - 1u - y) * rowBytes);
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
            WriteEvidence("GroomSimulation_GL_Deferred_HalfRes", half, kWidth / 2u, kHeight / 2u);
        }
        ResizeRenderTarget(kWidth, kHeight);
    }

    // -------------------------------------------------------------------------
    // Criterion 4: the reset control and the budget
    // -------------------------------------------------------------------------

    TEST_F(GroomSimulationVisualEvidenceTest, TheResetControlAndTheGuideBudgetAreLive)
    {
        auto& simulation = m_GroomEntity.GetComponent<GroomSimulationComponent>();

        ResetMotion();
        (void)RunBenchmarkMotion(0.6f, 60.0f);
        ASSERT_GT(PassStats().WorstRestDeviation, 0.0f);
        const u32 fullBudgetGuides = PassStats().GuidesSimulated;
        ASSERT_GT(fullBudgetGuides, 4u);

        // The reset control: a COUNTER, not a flag the scene clears.
        ++simulation.m_ResetKey;
        RunFrames(1, 1.0f / 60.0f);
        EXPECT_EQ(PassStats().SimulationReseeds, 1u);
        EXPECT_EQ(PassStats().WorstRestDeviation, 0.0f) << "a reset puts the coat back on its groom, exactly";

        // The per-role budget. This coat is Unassigned (its group has no coat
        // table), so that is the lever it answers to — and a budget that took a
        // PREFIX rather than a stride would still report the right count here,
        // which is why the stride itself is asserted in the unit tests and this
        // case only asserts that the budget is live.
        simulation.m_MaxGuidesUnassigned = 4u;
        RunFrames(1, 1.0f / 60.0f);
        const GroomRenderStats budgeted = PassStats();
        EXPECT_LE(budgeted.GuidesSimulated, 4u) << "the per-role budget must actually bind";
        EXPECT_GT(budgeted.GuidesSimulated, 0u);
        EXPECT_LT(budgeted.GuidesSimulated, fullBudgetGuides);

        // A zero budget is "this role is not simulated at all", which is a
        // legitimate authoring choice and NOT a stride of one.
        simulation.m_MaxGuidesUnassigned = 0u;
        RunFrames(1, 1.0f / 60.0f);
        EXPECT_EQ(PassStats().GroomsSimulated, 0u);
        EXPECT_EQ(PassStats().GuidesSimulated, 0u);
    }
} // namespace OloEngine::Tests
