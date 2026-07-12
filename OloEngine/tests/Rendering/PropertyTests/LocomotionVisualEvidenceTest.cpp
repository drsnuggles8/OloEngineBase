// OLO_TEST_LAYER: L7
// =============================================================================
// LocomotionVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the locomotion overhaul (issue #631): a character
// whose ENTITY is moved exclusively by extracted root motion walks across a
// physics floor and up onto a raised step, with FootIKComponent pulling the
// feet onto the ground it raycasts. Foot skating and wrong ground contact are
// invisible to math tests — this captures the frames a reviewer judges.
//
// The character is the same programmatic biped the headless capstone test
// (HumanoidLocomotionWalkTest) drives, made VISIBLE with proxy geometry: a
// bright-orange torso box on the root-motion-driven entity and bright-magenta
// foot markers positioned every frame from the REAL post-IK skeleton globals.
// Nothing else in the scene wears those colours, so "the character crossed the
// frame" and "the feet climbed the step" are driver-independent pixel checks.
//
// PNGs land in OloEditor/assets/tests/visual/Locomotion_*.png:
//   Side_Before / Side_After (fixed camera — root-motion displacement),
//   Front, TopDown (multi-angle per the CLAUDE.md rendering rules).
// Runs in the normal suite; SKIPs cleanly when no GL 4.6 context exists.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512;
        constexpr f32 kStepTopY = 0.15f;

        fs::path VisualOutputPath(const char* filename)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / filename;
        }

        void WritePng(const fs::path& out, const std::vector<u8>& px, u32 w, u32 h)
        {
            // The composite readback is GL bottom-up; flip rows so the PNG a
            // reviewer opens is top-down (matches WaterVisualEvidenceTest).
            std::vector<u8> flipped(px.size());
            const std::size_t rowBytes = static_cast<std::size_t>(w) * 4u;
            for (u32 y = 0; y < h; ++y)
            {
                std::copy_n(px.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(h - 1 - y) * rowBytes),
                            static_cast<std::ptrdiff_t>(rowBytes),
                            flipped.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * rowBytes));
            }
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(w),
                                               static_cast<int>(h), 4, flipped.data(), static_cast<int>(w) * 4);
            EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        }

        struct ColorStats
        {
            std::size_t Count = 0;
            f32 CentroidX = 0.0f;
        };

        // Count + horizontal centroid of pixels matching a colour predicate.
        template<typename Pred>
        ColorStats StatsOf(const std::vector<u8>& px, u32 w, Pred&& pred)
        {
            ColorStats stats;
            f64 sumX = 0.0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                if (pred(px[i], px[i + 1], px[i + 2]))
                {
                    ++stats.Count;
                    sumX += static_cast<f64>((i / 4) % w);
                }
            }
            if (stats.Count > 0)
            {
                stats.CentroidX = static_cast<f32>(sumX / static_cast<f64>(stats.Count));
            }
            return stats;
        }

        bool IsOrange(u8 r, u8 g, u8 b)
        {
            // Lighting-tolerant: the lit torso ranges from bright orange to
            // shaded brown; hue (r dominant over g over b) is what's distinct.
            return r > 90 && r > g + 25 && g > b + 10 && b < 110;
        }

        // Magenta like the cloth cape's evidence colour — the physics collider
        // debug wireframes are green, so the feet need a colour of their own.
        bool IsMagenta(u8 r, u8 g, u8 b)
        {
            return r > 100 && b > 100 && r > g + 45 && b > g + 45;
        }
    } // namespace

    class LocomotionVisualScene : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kLeftFoot = 3;
        static constexpr u32 kRightFoot = 6;

        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Primary camera for the runtime frames (captures use posed editor
            // cameras below).
            Entity camera = scene.CreateEntity("Camera");
            {
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 2.5f, -4.0f };
            }
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 3.0f, 8.0f, 2.0f };
            auto& dir = light.AddComponent<DirectionalLightComponent>();
            dir.m_Direction = glm::normalize(glm::vec3(-0.35f, -1.0f, 0.25f));
            dir.m_Color = { 1.0f, 0.97f, 0.9f };
            dir.m_Intensity = 2.4f;

            // Ground plane (visible) + physics floor for the foot raycasts.
            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -0.5f, 0.0f };
                tc.Scale = { 30.0f, 1.0f, 30.0f };
                if (Ref<Mesh> plane = MeshPrimitives::CreateCube(); plane)
                    ground.AddComponent<MeshComponent>(plane->GetMeshSource());
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.16f, 0.17f, 0.20f, 1.0f });
                auto& rb = ground.AddComponent<Rigidbody3DComponent>();
                rb.m_Type = BodyType3D::Static;
                ground.AddComponent<BoxCollider3DComponent>();
            }

            // The raised step across the walk path (visible + collidable).
            Entity step = scene.CreateEntity("Step");
            {
                auto& tc = step.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, kStepTopY - 0.5f, 2.2f };
                tc.Scale = { 4.0f, 1.0f, 1.4f };
                if (Ref<Mesh> box = MeshPrimitives::CreateCube(); box)
                    step.AddComponent<MeshComponent>(box->GetMeshSource());
                step.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.35f, 0.36f, 0.40f, 1.0f });
                auto& rb = step.AddComponent<Rigidbody3DComponent>();
                rb.m_Type = BodyType3D::Static;
                step.AddComponent<BoxCollider3DComponent>();
            }

            // ── The character: root-motion-driven entity + real skeleton ──────
            m_Character = scene.CreateEntity("Character");
            {
                auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(7));
                skeleton->m_BoneNames = { "Hips", "LeftUpLeg", "LeftLeg", "LeftFoot",
                                          "RightUpLeg", "RightLeg", "RightFoot" };
                skeleton->m_ParentIndices = { -1, 0, 1, 2, 0, 4, 5 };
                const glm::vec3 locals[7] = {
                    { 0.0f, 1.05f, 0.0f },
                    { 0.1f, -0.05f, 0.0f },
                    { 0.0f, -0.45f, 0.02f },
                    { 0.0f, -0.45f, -0.02f },
                    { -0.1f, -0.05f, 0.0f },
                    { 0.0f, -0.45f, 0.02f },
                    { 0.0f, -0.45f, -0.02f },
                };
                for (sizet i = 0; i < 7; ++i)
                    skeleton->m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), locals[i]);
                for (sizet i = 0; i < 7; ++i)
                {
                    const int p = skeleton->m_ParentIndices[i];
                    skeleton->m_GlobalTransforms[i] = (p >= 0)
                                                          ? skeleton->m_GlobalTransforms[static_cast<sizet>(p)] * skeleton->m_LocalTransforms[i]
                                                          : skeleton->m_LocalTransforms[i];
                }
                skeleton->SetBindPose();
                m_Character.AddComponent<SkeletonComponent>(skeleton);
            }

            // Walk clip: 1 m/s of pure root motion on the hips.
            auto clip = Ref<AnimationClip>::Create();
            clip->Name = "Walk";
            clip->Duration = 1.0f;
            {
                BoneAnimation hips;
                hips.BoneName = "Hips";
                hips.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.05f, 0.0f) });
                hips.PositionKeys.push_back({ 1.0, glm::vec3(0.0f, 1.05f, 1.0f) });
                clip->BoneAnimations.push_back(std::move(hips));
            }
            clip->InitializeBoneCache();
            clip->RootMotion.ExtractRootMotion = true;
            clip->RootMotion.RootBoneIndex = 0;

            auto& animState = m_Character.AddComponent<AnimationStateComponent>();
            animState.m_AvailableClips.push_back(clip);
            animState.m_CurrentClip = clip;
            animState.m_IsPlaying = true;

            auto& footIK = m_Character.AddComponent<FootIKComponent>();
            footIK.LeftFootBone = kLeftFoot;
            footIK.RightFootBone = kRightFoot;
            footIK.ChainLength = 3;
            footIK.PelvisBone = 0;
            footIK.FootHeight = 0.1f;
            footIK.RaycastUp = 0.6f;
            footIK.RaycastDown = 1.2f;
            footIK.FootLock = false;
            footIK.AlignFootToSlope = false;

            // Visible proxy geometry: an orange torso on the entity itself (so
            // root motion moves it), and green foot markers repositioned every
            // frame from the post-IK skeleton globals.
            Entity torso = scene.CreateEntity("Torso");
            {
                torso.SetParent(m_Character);
                auto& tc = torso.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 1.35f, 0.0f };
                tc.Scale = { 0.45f, 0.7f, 0.3f };
                if (Ref<Mesh> box = MeshPrimitives::CreateCube(); box)
                    torso.AddComponent<MeshComponent>(box->GetMeshSource());
                torso.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 1.0f, 0.45f, 0.05f, 1.0f });
            }

            auto makeFootMarker = [&](const char* name) -> Entity
            {
                Entity marker = scene.CreateEntity(name);
                marker.SetParent(m_Character);
                auto& tc = marker.GetComponent<TransformComponent>();
                tc.Scale = { 0.2f, 0.22f, 0.34f };
                if (Ref<Mesh> box = MeshPrimitives::CreateCube(); box)
                    marker.AddComponent<MeshComponent>(box->GetMeshSource());
                marker.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.9f, 0.05f, 0.85f, 1.0f });
                return marker;
            };
            m_LeftMarker = makeFootMarker("LeftFootMarker");
            m_RightMarker = makeFootMarker("RightFootMarker");

            EnableRendering(kSize, kSize);
            scene.OnPhysics3DStart();
        }

        // Position the foot markers from the REAL skeleton pose (post-IK
        // globals, in the character's local space — the markers are children).
        void SyncFootMarkers()
        {
            const auto& skeleton = m_Character.GetComponent<SkeletonComponent>().m_Skeleton;
            m_LeftMarker.GetComponent<TransformComponent>().Translation = glm::vec3(skeleton->m_GlobalTransforms[kLeftFoot][3]);
            m_RightMarker.GetComponent<TransformComponent>().Translation = glm::vec3(skeleton->m_GlobalTransforms[kRightFoot][3]);
        }

        void WalkFor(f32 seconds)
        {
            const u32 frames = static_cast<u32>(seconds * 60.0f);
            for (u32 f = 0; f < frames; ++f)
            {
                RunFrames(1);
                SyncFootMarkers();
            }
        }

        // Frozen-pose still from an explicitly posed editor camera. Playback
        // pauses for the capture so the pose is fully static and the temporal
        // pipeline converges (no ghost duplicates of the small foot markers).
        std::vector<u8> Capture(const glm::vec3& position, f32 yaw, f32 pitch)
        {
            auto& animState = m_Character.GetComponent<AnimationStateComponent>();
            const bool wasPlaying = animState.m_IsPlaying;
            animState.m_IsPlaying = false;

            EditorCamera cam(60.0f, 1.0f, 0.05f, 1000.0f);
            cam.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
            cam.SetPose(position, yaw, pitch);
            RunEditorFrames(cam, 8);
            std::vector<u8> px;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(px, w, h))
            {
                ADD_FAILURE() << "ReadbackComposite failed";
            }
            animState.m_IsPlaying = wasPlaying;
            return px;
        }

        Entity m_Character;
        Entity m_LeftMarker;
        Entity m_RightMarker;
    };

    TEST_F(LocomotionVisualScene, RootMotionWalkWithGroundedFeetReadsOnScreen)
    {
        // Fixed side camera: the walk axis (+Z) maps onto screen X.
        const glm::vec3 sidePos{ 7.5f, 1.6f, 1.2f };
        const f32 sideYaw = -glm::half_pi<f32>(); // from +X looking back at the walk line (cloth-test convention)
        const f32 sidePitch = -0.08f;

        // Settle one frame, then capture the "before" still on the flat floor.
        WalkFor(0.3f);
        std::vector<u8> before = Capture(sidePos, sideYaw, sidePitch);
        WritePng(VisualOutputPath("Locomotion_Side_Before.png"), before, kSize, kSize);

        // Walk onto the step (root motion covers ~2 m in 2 s).
        WalkFor(2.0f);
        std::vector<u8> after = Capture(sidePos, sideYaw, sidePitch);
        WritePng(VisualOutputPath("Locomotion_Side_After.png"), after, kSize, kSize);

        // Multi-angle stills of the final on-step pose.
        const f32 z = m_Character.GetComponent<TransformComponent>().Translation.z;
        std::vector<u8> front = Capture({ 0.0f, 1.4f, z + 4.5f }, 0.0f, -0.05f); // yaw 0 looks -Z, back along the walk line
        WritePng(VisualOutputPath("Locomotion_Front.png"), front, kSize, kSize);
        std::vector<u8> top = Capture({ 0.0f, 8.0f, z }, 0.0f, -glm::half_pi<f32>() * 0.94f);
        WritePng(VisualOutputPath("Locomotion_TopDown.png"), top, kSize, kSize);

        // ── Driver-independent pixel contracts ────────────────────────────────
        const ColorStats orangeBefore = StatsOf(before, kSize, IsOrange);
        const ColorStats orangeAfter = StatsOf(after, kSize, IsOrange);
        const std::size_t total = static_cast<std::size_t>(kSize) * kSize;

        // The character rendered in both stills...
        EXPECT_GT(orangeBefore.Count, total / 4000) << "orange torso missing in the before still";
        EXPECT_GT(orangeAfter.Count, total / 4000) << "orange torso missing in the after still";

        // ...and root motion visibly displaced it across the fixed frame.
        EXPECT_GT(std::abs(orangeAfter.CentroidX - orangeBefore.CentroidX), 40.0f)
            << "root motion did not move the character on screen (torso centroid shift "
            << std::abs(orangeAfter.CentroidX - orangeBefore.CentroidX) << " px)";

        // The feet rendered and stayed under the body in BOTH stills (a foot
        // left behind / flying off would pull the magenta centroid away from
        // the orange torso centroid).
        const ColorStats feetBefore = StatsOf(before, kSize, IsMagenta);
        const ColorStats feetAfter = StatsOf(after, kSize, IsMagenta);
        EXPECT_GT(feetBefore.Count, total / 20000) << "magenta foot markers missing in the before still";
        EXPECT_GT(feetAfter.Count, total / 20000) << "magenta foot markers missing in the after still";
        EXPECT_LT(std::abs(feetBefore.CentroidX - orangeBefore.CentroidX), 120.0f)
            << "feet are not under the body in the before still";
        EXPECT_LT(std::abs(feetAfter.CentroidX - orangeAfter.CentroidX), 120.0f)
            << "feet are not under the body in the after still";
        // And the feet travelled with the body across the fixed frame.
        EXPECT_GT(std::abs(feetAfter.CentroidX - feetBefore.CentroidX), 40.0f)
            << "feet did not travel with the body on screen";

        // Behaviour cross-check against scene state: the entity really is on
        // the step span and its feet adapted onto the step top.
        ASSERT_GT(z, 1.6f);
        ASSERT_LT(z, 2.8f);
        const auto& skeleton = m_Character.GetComponent<SkeletonComponent>().m_Skeleton;
        const glm::mat4 world = m_Character.GetComponent<TransformComponent>().GetTransform();
        const f32 leftFootWorldY = glm::vec3(world * skeleton->m_GlobalTransforms[kLeftFoot][3]).y;
        EXPECT_NEAR(leftFootWorldY, kStepTopY + 0.1f, 0.06f)
            << "left foot is not resting FootHeight above the step top";
    }
} // namespace OloEngine::Tests
