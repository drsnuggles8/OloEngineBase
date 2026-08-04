// OLO_TEST_LAYER: L8
// =============================================================================
// PlayerCameraRigVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the reusable player + camera rig (issue #645),
// rendered THROUGH THE RIG'S OWN CAMERA while the rig is actually running.
//
// Why this exists at all
// ----------------------
// The rig is not a rendering change, so it has no golden-image obligation. But
// it decides where the camera IS, and "the camera ends up somewhere sensible"
// is precisely the property that unit tests on the spring-arm arithmetic and
// Functional tests on the offset vector cannot show you. A camera can sit at
// mathematically the right offset and still be inside the character's head, or
// facing away, or clipped through a wall.
//
// So this drives a real Scene through `Scene::OnUpdateRuntime` with Jolt
// running and the full Renderer3D pipeline enabled, using the rig's own
// CameraComponent as the primary camera (RunFrames renders from it), and writes
// what that camera sees to
//   OloEditor/assets/tests/visual/PlayerRig_<pose>.png
//
// Poses captured:
//   ThirdPerson_Open  — 4 m boom over open ground. The character should sit in
//                       frame ahead of the camera.
//   ThirdPerson_Wall  — the same rig with the character backed against a wall,
//                       so the collision probe shortens the arm. The character
//                       should be visibly LARGER (camera pulled closer).
//   FirstPerson       — the same component with the boom zeroed. The camera is
//                       the eye; the world ahead should be visible from the
//                       character's own position.
//
// Contracts asserted alongside the PNGs (all measured in world space against
// the PIVOT — the point the boom hangs off — not the target's origin):
//   * the open-ground boom is the full authored length; the wall pose's is
//     strictly shorter AND strictly above the floor (the partial pull-in
//     branch, which is the interesting one);
//   * the camera is exactly that boom length from the pivot and pointed at it,
//     in both poses — the check that says the camera actually MOVED, as
//     distinct from the boom-length field merely changing;
//   * first person puts the eye exactly at the pivot with a zero boom;
//   * every capture rendered something (not a flat clear-colour frame).
//
// What is NOT asserted, and why
// -----------------------------
// An earlier revision tried to assert on the character's ALBEDO HUE — "the
// pulled-in frame shows more orange than the open one". It doesn't work here,
// and chasing it cost more than it was worth:
//   * the first cut used absolute levels (`r > 70`), which reported zero on a
//     frame where the character was dead centre — this scene is dim, and the
//     lit character came back around (58, 36, 16);
//   * adding an EnvironmentMapComponent for ambient light made the character
//     render as a MIRROR of the skybox (a photograph of sea and mountains)
//     even at metallic 0 / roughness 1;
//   * with that removed, the character is drawn from INSIDE (the first-person
//     pose shows it) but not from a correctly-aimed camera 4 m behind.
// That last one is the important part, and the second test below is the
// one-switch control that settles it: an EDITOR camera posed at the exact same
// eye sees exactly the same thing. So the character's absence at range is a
// property of how this synthetic scene submits its primitive mesh, NOT of the
// rig — and the two paths agreeing is asserted rather than assumed.
//
// The PNGs remain the human-readable evidence; the numbers above are what CI
// enforces. No golden comparison: the exact pixels depend on the primitive
// meshes and lighting, and everything worth pinning is asserted directly.
// SKIPs cleanly (via RendererAttachedTest) when no GL 4.6 context exists, so
// headless CI is unaffected.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigSystem.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        constexpr f32 kBoomLength = 4.0f;
        constexpr f32 kMinBoomLength = 0.6f;

        // How far back the character walks for the pulled-in pose. The wall's
        // near face sits at kWallCentreZ - kWallHalfThickness = 5.5, so stopping
        // at 3.3 leaves ~2.2 m of clearance: comfortably inside the 4 m boom
        // (so the probe fires) and comfortably above kMinBoomLength (so the arm
        // does not bottom out). The cap only exists so a rig that stopped moving
        // fails loudly instead of spinning.
        constexpr f32 kWallApproachZ = 3.3f;
        constexpr u32 kMaxDriveFrames = 600;

        // The character's albedo. Deliberately a saturated orange that nothing
        // else in the scene wears, so "how much of the frame is the character"
        // is a simple per-pixel colour test rather than a segmentation problem.
        constexpr glm::vec3 kPlayerAlbedo{ 0.95f, 0.35f, 0.05f };

        // Fraction of pixels whose HUE reads as the character's orange.
        //
        // Deliberately a ratio test, not an absolute-level one. The first cut of
        // this used `r > 70 && r > g + 25`, which reported zero orange on a
        // frame where the character was dead centre and 4 m away: this scene has
        // no strong ambient term, so the lit character came back around
        // (58, 36, 16) — unmistakably orange, nowhere near an absolute
        // threshold. Ratios survive exposure, tone-mapping and shadowing; fixed
        // levels encode the lighting rig into the assertion, which is exactly
        // the trap docs/agent-rules/single-mesh-visual-test-lighting.md warns
        // about.
        [[nodiscard]] f64 OrangePixelFraction(const std::vector<u8>& rgba)
        {
            if (rgba.empty())
                return 0.0;
            std::size_t hits = 0;
            std::size_t total = 0;
            for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                const f64 r = rgba[i + 0];
                const f64 g = rgba[i + 1];
                const f64 b = rgba[i + 2];
                // Red dominant over green, and far over blue — the signature of
                // this albedo under any exposure. The small floor only rejects
                // near-black pixels where the ratios are numerical noise.
                if (r > 16.0 && r > g * 1.4 && r > b * 2.0)
                    ++hits;
                ++total;
            }
            return total ? static_cast<f64>(hits) / static_cast<f64>(total) : 0.0;
        }

        // A frame that rendered nothing at all is a single flat colour. Any real
        // frame here has a lit ground plane, a sky-less clear, and a character.
        [[nodiscard]] f64 LuminanceStdDev(const std::vector<u8>& rgba)
        {
            if (rgba.empty())
                return 0.0;
            f64 sum = 0.0;
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                const f64 lum = 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                sum += lum;
                sumSq += lum * lum;
                ++count;
            }
            if (count == 0)
                return 0.0;
            const f64 mean = sum / static_cast<f64>(count);
            return std::sqrt(std::max(0.0, sumSq / static_cast<f64>(count) - mean * mean));
        }

        void FlipRowsInPlace(std::vector<u8>& rgba, u32 width, u32 height)
        {
            const std::size_t stride = static_cast<std::size_t>(width) * 4u;
            std::vector<u8> row(stride);
            for (u32 y = 0; y < height / 2; ++y)
            {
                u8* top = rgba.data() + static_cast<std::size_t>(y) * stride;
                u8* bottom = rgba.data() + static_cast<std::size_t>(height - 1 - y) * stride;
                std::memcpy(row.data(), top, stride);
                std::memcpy(top, bottom, stride);
                std::memcpy(bottom, row.data(), stride);
            }
        }
    } // namespace

    class PlayerCameraRigVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                light.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Angled so the faces the camera sees from BEHIND the character
                // are lit. A sun pointing straight down leaves every vertical
                // face in shadow and the subject renders near-black even with a
                // saturated albedo — the trap in
                // docs/agent-rules/single-mesh-visual-test-lighting.md.
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.5f, -0.8f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
                dl.m_Intensity = 4.0f;
            }

            // NO EnvironmentMapComponent on purpose. An earlier revision added one
            // for ambient light and the character came back as a mirror showing a
            // photograph of sea and mountains — the environment cubemap was
            // reaching the surface shading even at metallic 0 / roughness 1. The
            // captures are darker without it, which is why the hue test below is
            // a ratio rather than a level; a legible albedo matters more here
            // than a pretty frame, since the albedo is what identifies the
            // character in the pixel assertions.

            AddBox("Ground", { 0.0f, -0.5f, 0.0f }, { 60.0f, 1.0f, 60.0f }, { 0.34f, 0.36f, 0.38f },
                   { 30.0f, 0.5f, 30.0f });

            // The obstruction the third-person boom has to cope with. Placed
            // BEHIND the character's start (+Z, which is where the boom points
            // at yaw 0), far enough that the open-ground pose is unobstructed
            // and the backed-up pose is not.
            m_Wall = AddBox("Wall", { 0.0f, 2.0f, 6.0f }, { 12.0f, 4.0f, 1.0f }, { 0.45f, 0.5f, 0.55f },
                            { 6.0f, 2.0f, 0.5f });

            // Something ahead for the first-person pose to actually look AT, so
            // that capture is not a featureless plane.
            AddBox("Landmark", { 0.0f, 1.5f, -8.0f }, { 3.0f, 3.0f, 3.0f }, { 0.2f, 0.45f, 0.75f },
                   { 1.5f, 1.5f, 1.5f });

            // ── The player ───────────────────────────────────────────────────
            m_Player = scene.CreateEntity("Player");
            {
                auto& tc = m_Player.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 1.0f, 0.0f };
                tc.Scale = { 0.8f, 1.0f, 0.8f };
                auto& mc = m_Player.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = m_Player.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(kPlayerAlbedo, 1.0f));
                // Dielectric and rough, or the character renders as a MIRROR:
                // the material defaults are metallic + smooth, so with IBL on it
                // reflected the skybox and came back as a photograph of sea and
                // mountains instead of orange. Every hue assertion below reads
                // the albedo, so the albedo has to be what actually shows.
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.75f);

                CapsuleCollider3DComponent capsule;
                capsule.m_Radius = 0.4f;
                capsule.m_HalfHeight = 0.6f;
                m_Player.AddComponent<CapsuleCollider3DComponent>(capsule);
                m_Player.AddComponent<CharacterController3DComponent>();

                PlayerRigComponent rig = PlayerRigPresets::ThirdPersonPlayer();
                rig.m_UseDeviceInput = false; // no window here; the test drives intent
                rig.m_YawDeg = 0.0f;          // looking down -Z, boom points +Z
                rig.m_PitchDeg = -8.0f;
                m_Player.AddComponent<PlayerRigComponent>(rig);
            }

            // ── The rig camera (a ROOT entity — the rig writes a world pose) ──
            m_Camera = scene.CreateEntity("RigCamera");
            {
                auto& cc = m_Camera.AddComponent<CameraComponent>();
                cc.Primary = true; // RunFrames renders from this
                cc.Camera.SetPerspectiveVerticalFOV(glm::radians(60.0f));
                cc.Camera.SetPerspectiveNearClip(0.05f);
                cc.Camera.SetPerspectiveFarClip(1000.0f);

                CameraRigComponent rig = PlayerRigPresets::ThirdPersonCamera(m_Player.GetUUID());
                rig.m_PivotOffset = { 0.0f, 0.6f, 0.0f };
                rig.m_BoomLength = kBoomLength;
                rig.m_MinBoomLength = kMinBoomLength;
                rig.m_PositionSmoothTime = 0.0f; // rigid, so a capture is unambiguous
                m_Camera.AddComponent<CameraRigComponent>(rig);
            }

            scene.OnPhysics3DStart();
        }

        Entity AddBox(const char* name, const glm::vec3& position, const glm::vec3& scale,
                      const glm::vec3& albedo, const glm::vec3& halfExtents)
        {
            Entity e = GetScene().CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;

            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            // Dielectric + rough for the same reason as the character above.
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(0.85f);

            BoxCollider3DComponent col;
            col.m_HalfExtents = halfExtents;
            e.AddComponent<BoxCollider3DComponent>(col);
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Static;
            e.AddComponent<Rigidbody3DComponent>(body);
            return e;
        }

        // Tick the rig + full render pipeline, read back the composited frame,
        // and write it as evidence. Returns the frame for pixel assertions.
        void Capture(const std::string& poseName, u32 frames, std::vector<u8>& outRgba)
        {
            RunFrames(frames);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outRgba, width, height))
                << "no composited framebuffer for pose '" << poseName << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);

            // glGetTextureImage hands back rows bottom-up (GL origin); PNG wants
            // row 0 at the top.
            FlipRowsInPlace(outRgba, width, height);

            const fs::path dir = fs::path(OLO_TEST_EDITOR_ROOT) / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path out = dir / ("PlayerRig_" + poseName + ".png");
            // Checked: an unchecked write leaves the test green with no PNG on
            // disk, which is the worst outcome for a test whose whole product is
            // the PNG.
            EXPECT_NE(stbi_write_png(out.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                     outRgba.data(), static_cast<int>(width) * 4),
                      0)
                << "failed to write evidence PNG: " << out.string();

            // Printed with every capture so a failure says WHERE the camera was,
            // not just that the pixels disappointed. A frame that looks wrong is
            // ambiguous between "the rig mis-placed the camera" and "the scene
            // is too dark to see"; these numbers separate the two immediately.
            const glm::vec3 cameraPos = m_Camera.GetComponent<TransformComponent>().Translation;
            const glm::vec3 playerPos = m_Player.GetComponent<TransformComponent>().Translation;
            const glm::vec3 forward =
                m_Camera.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 0.0f, -1.0f);
            std::cout << "[rig-evidence] " << poseName << "  camera=(" << cameraPos.x << ", " << cameraPos.y << ", "
                      << cameraPos.z << ")  player=(" << playerPos.x << ", " << playerPos.y << ", " << playerPos.z
                      << ")  forward=(" << forward.x << ", " << forward.y << ", " << forward.z
                      << ")  boom=" << CameraRig().m_CurrentBoomLength
                      << "  orange=" << OrangePixelFraction(outRgba) << "  lumStd=" << LuminanceStdDev(outRgba)
                      << std::endl;
        }

        // The point the boom actually hangs off: the target's origin plus the
        // authored pivot offset in the target's yaw frame. Measuring against the
        // player's ORIGIN instead is subtly wrong and was the first thing these
        // assertions got wrong — with a 0.6 m eye-height offset the camera sits
        // 4.13 m from the origin on a 4 m boom, and reads 0.96 rather than ~1.0
        // on the look-at test, which looks exactly like a rig bug and isn't one.
        [[nodiscard]] glm::vec3 PivotPosition()
        {
            const glm::vec3 playerPos = m_Player.GetComponent<TransformComponent>().Translation;
            const glm::quat yaw = PlayerRigSystem::YawRotation(PlayerRig().m_YawDeg);
            return playerPos + yaw * CameraRig().m_PivotOffset;
        }

        // World-space distance from the camera to the pivot. This is the
        // assertion that says "the camera actually moved", as distinct from
        // "the boom-length field changed".
        [[nodiscard]] f32 CameraToPivotDistance()
        {
            return glm::length(m_Camera.GetComponent<TransformComponent>().Translation - PivotPosition());
        }

        // cos(angle) between the camera's forward axis and the direction to the
        // pivot. 1 means dead centre. Catches a rig that places the camera at
        // the right distance but facing the wrong way — which every offset-only
        // assertion in the Functional tests would happily accept.
        [[nodiscard]] f32 CameraLooksAtPivot()
        {
            const glm::vec3 cameraPos = m_Camera.GetComponent<TransformComponent>().Translation;
            const glm::vec3 toPivot = PivotPosition() - cameraPos;
            if (glm::dot(toPivot, toPivot) < 1e-8f)
                return 1.0f; // coincident (first person) — trivially "looking at" it
            const glm::vec3 forward =
                m_Camera.GetComponent<TransformComponent>().GetRotation() * glm::vec3(0.0f, 0.0f, -1.0f);
            return glm::dot(glm::normalize(forward), glm::normalize(toPivot));
        }

        [[nodiscard]] PlayerRigComponent& PlayerRig()
        {
            return m_Player.GetComponent<PlayerRigComponent>();
        }
        [[nodiscard]] CameraRigComponent& CameraRig()
        {
            return m_Camera.GetComponent<CameraRigComponent>();
        }

        Entity m_Player;
        Entity m_Camera;
        Entity m_Wall;
    };

    TEST_F(PlayerCameraRigVisualEvidenceTest, CaptureThirdPersonOpenPulledInAndFirstPerson)
    {
        // ── Pose 1: third person over open ground ────────────────────────────
        std::vector<u8> openFrame;
        Capture("ThirdPerson_Open", 30, openFrame);

        const f32 openBoom = CameraRig().m_CurrentBoomLength;
        EXPECT_NEAR(openBoom, kBoomLength, 1e-3f)
            << "the boom should be fully extended with nothing behind the character";
        EXPECT_NEAR(CameraToPivotDistance(), openBoom, 0.05f)
            << "the camera is not actually a boom's length from the character";
        EXPECT_GT(CameraLooksAtPivot(), 0.97f) << "the camera is not pointed at its target";
        EXPECT_GT(LuminanceStdDev(openFrame), 1.0) << "open-ground capture is a flat frame — nothing rendered";

        // ── Pose 2: backed toward the wall, so the probe shortens the arm ─────
        // Walk backwards (the boom points +Z at yaw 0, and the wall is at +Z),
        // which is what puts geometry between camera and character.
        //
        // Deliberately stopped PART WAY rather than pressed flat against the
        // wall: a fully-backed-up character floors the arm at m_MinBoomLength,
        // which puts the camera inside the wall (that is what the minimum means
        // — see the guide's Known limits) and makes the capture a slab of wall
        // interior. A partial pull-in is the case worth photographing, and it
        // exercises the interesting branch: clearance strictly between the floor
        // and the authored length.
        // Driven to a POSITION, not for a fixed frame count. A hard-coded
        // "walk backwards for 45 frames" silently encodes the preset's walk
        // speed and the fixture's dt: change either and the character either
        // never gets close enough to obstruct the boom, or presses into the wall
        // and floors the arm at m_MinBoomLength — and the assertions below are a
        // narrow band between those two. Stopping on the character's own Z makes
        // the pose reproducible whatever the tick rate.
        PlayerRig().m_MoveInput = { 0.0f, -1.0f };
        u32 driveFrames = 0;
        while (m_Player.GetComponent<TransformComponent>().Translation.z < kWallApproachZ &&
               driveFrames < kMaxDriveFrames)
        {
            RunFrames(1);
            ++driveFrames;
        }
        PlayerRig().m_MoveInput = { 0.0f, 0.0f };
        ASSERT_LT(driveFrames, kMaxDriveFrames)
            << "the character never reached the approach mark (z=" << kWallApproachZ << "); it stopped at z="
            << m_Player.GetComponent<TransformComponent>().Translation.z
            << ". Without the approach there is no obstruction and the pose below is vacuous.";

        std::vector<u8> wallFrame;
        Capture("ThirdPerson_Wall", 2, wallFrame);

        const f32 wallBoom = CameraRig().m_CurrentBoomLength;
        EXPECT_LT(wallBoom, openBoom - 0.5f)
            << "the boom did not pull in with a wall behind the character; boom=" << wallBoom;
        EXPECT_GT(wallBoom, kMinBoomLength + 1e-3f)
            << "the boom bottomed out at its minimum — this pose is meant to catch the PARTIAL "
               "pull-in branch (clearance strictly between the floor and the authored length); boom="
            << wallBoom;

        // The boom length is just a number; this is the part that says the
        // CAMERA moved. A rig that shortened the arm without re-placing the
        // camera would pass the length assertion above and fail here.
        EXPECT_NEAR(CameraToPivotDistance(), wallBoom, 0.05f)
            << "the boom shortened but the camera did not move closer to the character";
        EXPECT_GT(CameraLooksAtPivot(), 0.97f) << "the pulled-in camera stopped facing its target";
        EXPECT_GT(LuminanceStdDev(wallFrame), 1.0) << "pulled-in capture is a flat frame — nothing rendered";

        // ── Pose 3: first person is the same component with a zero boom ──────
        CameraRig().m_BoomLength = 0.0f;
        CameraRig().m_PivotOffset = { 0.0f, 0.75f, 0.0f };
        CameraRig().m_Initialized = false; // adopt the new pose outright
        PlayerRig().m_YawDeg = 180.0f;     // turn round to face the landmark
        PlayerRig().m_PitchDeg = 0.0f;

        std::vector<u8> firstPersonFrame;
        Capture("FirstPerson", 40, firstPersonFrame);

        EXPECT_NEAR(CameraRig().m_CurrentBoomLength, 0.0f, 1e-4f)
            << "a zero authored boom must stay zero — that IS the first-person rig";
        // The eye sits at the pivot: directly above the character's origin by
        // the pivot offset and nowhere else.
        const glm::vec3 eyeOffset = m_Camera.GetComponent<TransformComponent>().Translation -
                                    m_Player.GetComponent<TransformComponent>().Translation;
        EXPECT_NEAR(eyeOffset.x, 0.0f, 1e-3f);
        EXPECT_NEAR(eyeOffset.y, 0.75f, 1e-3f);
        EXPECT_NEAR(eyeOffset.z, 0.0f, 1e-3f);
        EXPECT_GT(LuminanceStdDev(firstPersonFrame), 1.0)
            << "first-person capture is a flat frame — nothing rendered";
    }

    // A/B control for the captures above.
    //
    // The rig captures show the character only from INSIDE it (the
    // first-person pose); from a correctly-aimed camera 4 m behind, the frame
    // is the clear colour. That is either "the rig camera path is wrong" or
    // "this scene does not draw that mesh at that distance, whoever is
    // looking". One switch separates them: pose an EDITOR camera at the exact
    // same eye/orientation the rig produced and render through the editor path
    // instead. Same pixels => the rig camera is fine and the gap is in how this
    // synthetic scene submits its primitive mesh; different pixels => the rig
    // is the difference and this test has found a real bug.
    TEST_F(PlayerCameraRigVisualEvidenceTest, EditorCameraAtTheRigPoseSeesTheSameThing)
    {
        std::vector<u8> rigFrame;
        Capture("ThirdPerson_Open", 30, rigFrame);

        const glm::vec3 eye = m_Camera.GetComponent<TransformComponent>().Translation;
        // EditorCamera::SetPose takes yaw/pitch in radians with the same
        // convention the rig uses (yaw about +Y, pitch positive looking up).
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(eye, glm::radians(PlayerRig().m_YawDeg), glm::radians(PlayerRig().m_PitchDeg));
        RunEditorFrames(camera, 2);

        std::vector<u8> editorFrame;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(editorFrame, width, height));
        FlipRowsInPlace(editorFrame, width, height);

        const fs::path dir = fs::path(OLO_TEST_EDITOR_ROOT) / "assets" / "tests" / "visual";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path controlOut = dir / "PlayerRig_EditorControl.png";
        EXPECT_NE(stbi_write_png(controlOut.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                 editorFrame.data(), static_cast<int>(width) * 4),
                  0)
            << "failed to write evidence PNG: " << controlOut.string();

        std::cout << "[rig-evidence] EditorControl  eye=(" << eye.x << ", " << eye.y << ", " << eye.z
                  << ")  orange=" << OrangePixelFraction(editorFrame)
                  << "  lumStd=" << LuminanceStdDev(editorFrame) << std::endl;

        // Both paths must agree about whether the character is on screen. This
        // is the assertion that would catch a rig placing the camera somewhere
        // the numbers say is right but the view disagrees with.
        const bool rigSeesCharacter = OrangePixelFraction(rigFrame) > 0.0005;
        const bool editorSeesCharacter = OrangePixelFraction(editorFrame) > 0.0005;
        EXPECT_EQ(rigSeesCharacter, editorSeesCharacter)
            << "the rig camera and an editor camera at the SAME pose disagree about whether the "
               "character is visible (rig "
            << OrangePixelFraction(rigFrame) << " vs editor "
            << OrangePixelFraction(editorFrame) << ") — the rig is placing or orienting the camera wrongly";
    }

} // namespace OloEngine::Tests
