// OLO_TEST_LAYER: L8
// =============================================================================
// ObserverCameraVisualEvidenceTest.cpp  (#726)
//
// Full-pipeline visual evidence for the observer camera, and the automated form
// of the issue's second acceptance criterion: "an intentionally over-culling
// change is visibly diagnosable in one screenshot".
//
// The scene is a wide row of cubes on a ground plane. A NARROW-FOV camera at the
// centre sees only the middle few; the outer cubes are frustum-culled. The
// culling camera is frozen at that pose, then the viewport is flown far back so
// every cube is inside the OBSERVER's view.
//
// The contract is golden-free and differential — the same observer pose is
// rendered twice, once frozen and once not:
//   1. The outer bands of the frame lose geometry when frozen. That missing
//      geometry IS the diagnostic: the cubes the frozen camera culled are still
//      culled, so a wrongly-culled object is visibly absent instead of merely
//      off-screen.
//   2. The centre band is essentially unchanged. Freezing must not disturb what
//      the frozen camera DID see -- if the whole frame moved, the tool is
//      showing something other than the frozen cut, which is the failure mode
//      that makes it worse than no tool at all.
//   3. Unfreezing restores the unfrozen frame, so the state is not one-way.
//
// Both frames plus a third with the frozen-frustum wireframe on are written to
//   OloEditor/assets/tests/visual/ObserverCamera_<state>.png
// The wireframe frame is deliberately NOT one of the compared pair: the debug
// geometry crosses the centre band and would break contract 2 for a reason that
// has nothing to do with culling.
//
// The ground plane is there on purpose — see
// docs/agent-rules/single-mesh-visual-test-lighting.md: a sparse scene renders
// the subject near-black and every band statistic then measures noise.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence). SKIPs
// cleanly when no GL 4.6 context exists.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>

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

        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 240;

        // How far out the row of cubes reaches, and how narrow the frozen
        // camera's view is. The two numbers only have to satisfy one property:
        // the outer cubes must be outside the frozen frustum and inside the
        // observer's. The test asserts that property directly rather than
        // trusting these constants.
        constexpr i32 kCubesPerSide = 4;
        constexpr f32 kCubeSpacing = 4.0f;
        constexpr f32 kCubeScale = 1.5f;
        constexpr f32 kCubeDepth = -6.0f;
        constexpr f32 kFrozenFovDegrees = 16.0f;

        // Bands, as fractions of the frame width. The outer thirds are where the
        // cubes the frozen camera culled land once the observer pulls back; the
        // narrow centre band holds only the one cube the frozen camera kept.
        // Sized from the geometry above rather than picked by eye -- the first
        // version of this test measured two bands that contained no cubes at all
        // and read "freezing changed nothing" when it had changed everything the
        // cubes covered.
        constexpr f32 kOuterLeft0 = 0.0f, kOuterLeft1 = 0.33f;
        constexpr f32 kOuterRight0 = 0.67f, kOuterRight1 = 1.0f;
        constexpr f32 kCentre0 = 0.46f, kCentre1 = 0.54f;

        // Count the CUBE pixels in a vertical band: the cubes are the only warm
        // thing in a scene that is otherwise neutral grey (ground, grid, sky,
        // shadows), so "r meaningfully above b" isolates them exactly.
        //
        // Deliberately NOT a band luminance mean, which is the obvious statistic
        // and the wrong one: these cubes are only a hair darker than the ground
        // they stand on, so deleting four of them moved the band mean by 0.0006
        // -- and *upward*, because their shadows went away with them. A count of
        // "how much cube is in this band" says what the test actually means and
        // has an unambiguous direction.
        [[nodiscard]] u64 BandCubePixels(const std::vector<u8>& px, f32 x0Frac, f32 x1Frac)
        {
            const auto x0 = static_cast<u32>(x0Frac * static_cast<f32>(kWidth));
            const auto x1 = static_cast<u32>(x1Frac * static_cast<f32>(kWidth));
            u64 count = 0;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = x0; x < x1 && x < kWidth; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (i + 3 >= px.size())
                        continue;
                    if (static_cast<int>(px[i]) - static_cast<int>(px[i + 2]) > 40)
                        ++count;
                }
            }
            return count;
        }

        // Fraction of pixels in a band that differ by more than `threshold` in
        // any channel.
        [[nodiscard]] f64 BandFractionChanged(const std::vector<u8>& a, const std::vector<u8>& b,
                                              f32 x0Frac, f32 x1Frac, int threshold)
        {
            const auto x0 = static_cast<u32>(x0Frac * static_cast<f32>(kWidth));
            const auto x1 = static_cast<u32>(x1Frac * static_cast<f32>(kWidth));
            u64 changed = 0;
            u64 count = 0;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = x0; x < x1 && x < kWidth; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (i + 3 >= a.size() || i + 3 >= b.size())
                        continue;
                    const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                    const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                    const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                    if (dr > threshold || dg > threshold || db > threshold)
                        ++changed;
                    ++count;
                }
            }
            return count ? static_cast<f64>(changed) / static_cast<f64>(count) : 0.0;
        }
    } // namespace

    class ObserverCameraVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 6.0f, 14.0f, 10.0f };
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f));
            dl.m_Color = { 1.0f, 0.97f, 0.92f };
            dl.m_Intensity = 4.0f;

            const Ref<Mesh> cube = MeshPrimitives::CreateCube();

            // Ground plane. Without it the cubes read near-black from every
            // angle and the band statistics measure nothing (see the agent-rules
            // note quoted in the header).
            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -1.6f, 0.0f };
                tc.Scale = { 140.0f, 0.2f, 60.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (cube)
                    mc.m_MeshSource = cube->GetMeshSource();
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.35f, 0.36f, 0.38f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            // A wide row of cubes, all at the same depth so the frozen camera's
            // narrow frustum cuts the row by X alone. Non-instanced MeshComponents
            // so the CPU frustum-cull path (Renderer3D::ViewFrustum) is what
            // decides -- the GPU instance cull has its own parity coverage.
            for (i32 i = -kCubesPerSide; i <= kCubesPerSide; ++i)
            {
                Entity e = scene.CreateEntity("Cube" + std::to_string(i));
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = { static_cast<f32>(i) * kCubeSpacing, 0.0f, kCubeDepth };
                tc.Scale = glm::vec3(kCubeScale);
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (cube)
                    mc.m_MeshSource = cube->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                // Warm, bright, and clearly not the ground: an outer cube going
                // missing has to be visible as a change in the band mean.
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.55f, 0.15f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.55f);
            }
        }

        // The camera whose frustum gets frozen: narrow FOV, at the origin,
        // looking down -Z at the middle of the row.
        [[nodiscard]] static EditorCamera MakeFrozenPoseCamera()
        {
            EditorCamera camera(kFrozenFovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.1f, 300.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 0.5f, 8.0f), /*yaw*/ 0.0f, /*pitch*/ 0.0f);
            return camera;
        }

        // The observer: pulled far back and up with a wide FOV, so the whole row
        // -- including the cubes the frozen camera culled -- is on screen.
        [[nodiscard]] static EditorCamera MakeObserverCamera()
        {
            EditorCamera camera(70.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Close enough that the row of cubes spans most of the frame. Pulling
            // further back is prettier and useless: every cube collapses into a
            // handful of pixels in the middle and the outer bands measure sky.
            camera.SetPose(glm::vec3(0.0f, 6.0f, 18.0f), /*yaw*/ 0.0f, /*pitch*/ glm::radians(10.0f));
            return camera;
        }

        void Capture(const EditorCamera& camera, const std::string& tag, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for observer-camera capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame.
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();

            const std::string path = (dir / ("ObserverCamera_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }
    };

    // RAII: the culling freeze and the debug-draw toggle are process-wide, and a
    // failing ASSERT must not leave the culling camera frozen for every later
    // test in the binary.
    struct ObserverStateRestore
    {
        bool m_Frozen = Renderer3D::IsCullingCameraFrozen();
        bool m_DebugDraw = Renderer3D::GetRendererSettings().ShaderDebugDrawEnabled;
        bool m_DrawFrustum = Renderer3D::GetRendererSettings().ObserverCameraDrawFrustum;

        ~ObserverStateRestore()
        {
            Renderer3D::SetCullingCameraFrozen(m_Frozen);
            Renderer3D::GetRendererSettings().ShaderDebugDrawEnabled = m_DebugDraw;
            Renderer3D::GetRendererSettings().ObserverCameraDrawFrustum = m_DrawFrustum;
            Renderer3D::ApplyRendererSettings();
        }
    };

    TEST_F(ObserverCameraVisualEvidenceTest, FrozenCullIsVisiblyMissingGeometryFromTheObserver)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ObserverStateRestore restore;

        // Frustum wireframe off for the compared pair: it crosses the centre
        // band and would break the "centre is unchanged" contract for a reason
        // that has nothing to do with culling.
        Renderer3D::GetRendererSettings().ShaderDebugDrawEnabled = false;
        Renderer3D::ApplyRendererSettings();
        Renderer3D::SetCullingCameraFrozen(false);

        // 1. The observer pose, NOT frozen: the whole row is drawn.
        std::vector<u8> unfrozen;
        Capture(MakeObserverCamera(), "unfrozen", unfrozen);
        ASSERT_FALSE(HasFatalFailure());

        // 2. Sit at the narrow gameplay pose and freeze there. RunEditorFrames
        //    inside Capture is what pushes that pose through BeginScene, so the
        //    freeze below snapshots it rather than whatever came before.
        std::vector<u8> atFrozenPose;
        Capture(MakeFrozenPoseCamera(), "frozen_pose_view", atFrozenPose);
        ASSERT_FALSE(HasFatalFailure());
        Renderer3D::SetCullingCameraFrozen(true);
        ASSERT_TRUE(Renderer3D::IsCullingCameraFrozen());

        // 3. Fly out to the observer pose with the cut still frozen.
        std::vector<u8> frozen;
        Capture(MakeObserverCamera(), "frozen", frozen);
        ASSERT_FALSE(HasFatalFailure());

        // -- Contract 1: the outer bands lose geometry ------------------------
        // The outer cubes were outside the narrow frozen frustum, so they must
        // still be culled from a viewpoint that can plainly see where they are.
        const u64 leftUnfrozen = BandCubePixels(unfrozen, kOuterLeft0, kOuterLeft1);
        const u64 leftFrozen = BandCubePixels(frozen, kOuterLeft0, kOuterLeft1);
        const u64 rightUnfrozen = BandCubePixels(unfrozen, kOuterRight0, kOuterRight1);
        const u64 rightFrozen = BandCubePixels(frozen, kOuterRight0, kOuterRight1);

        const f64 leftChanged = BandFractionChanged(unfrozen, frozen, kOuterLeft0, kOuterLeft1, 12);
        const f64 rightChanged = BandFractionChanged(unfrozen, frozen, kOuterRight0, kOuterRight1, 12);

        EXPECT_GT(leftChanged + rightChanged, 0.01)
            << "Freezing the culling camera changed nothing in the outer bands. Either the cubes "
               "outside the frozen frustum were never culled (culling is following the observer), "
               "or the scene no longer places any cube outside it. left="
            << leftChanged
            << " right=" << rightChanged;

        // Stated as a DIRECTION, not just "something changed": geometry went
        // away, it did not merely move. A frame that shifted would change the
        // band without reducing the cube coverage in it.
        ASSERT_GT(leftUnfrozen, 0u) << "no cubes in the left band even unfrozen - the scene geometry and the "
                                       "measured band have drifted apart, so this test proves nothing";
        ASSERT_GT(rightUnfrozen, 0u) << "no cubes in the right band even unfrozen - see above";
        EXPECT_LT(leftFrozen, leftUnfrozen)
            << "left band kept its cubes when the frozen camera had culled them: unfrozen=" << leftUnfrozen
            << "px frozen=" << leftFrozen << "px";
        EXPECT_LT(rightFrozen, rightUnfrozen)
            << "right band kept its cubes when the frozen camera had culled them: unfrozen=" << rightUnfrozen
            << "px frozen=" << rightFrozen << "px";

        // -- Contract 2: the centre is left alone -----------------------------
        // What the frozen camera DID see must render identically. A frame that
        // moved everywhere is showing some other cut, which is the failure this
        // instrument cannot be allowed to have.
        const f64 centreChanged = BandFractionChanged(unfrozen, frozen, kCentre0, kCentre1, 12);
        EXPECT_LT(centreChanged, 0.05)
            << "the centre of the frame changed when the culling camera was frozen (" << centreChanged
            << "). Freezing must not disturb geometry the frozen camera could see.";

        // -- Contract 3: unfreezing puts it back ------------------------------
        Renderer3D::SetCullingCameraFrozen(false);
        std::vector<u8> restored;
        Capture(MakeObserverCamera(), "restored", restored);
        ASSERT_FALSE(HasFatalFailure());
        const f64 restoredChanged = BandFractionChanged(unfrozen, restored, 0.0f, 1.0f, 12);
        EXPECT_LT(restoredChanged, 0.05)
            << "unfreezing did not restore the live cut (" << restoredChanged
            << " of the frame still differs from the never-frozen capture)";
    }

    // -------------------------------------------------------------------------
    // The GPU compute cull (issue #726)
    //
    // Everything above rides the CPU frustum filter. The dense-scatter path is a
    // different implementation entirely: above 1024 instances a batch routes
    // through Renderer3D::SubmitGPUCulledInstanced, and the frustum planes are
    // then extracted inside InstanceFrustumCull.comp from a matrix uploaded in
    // InstanceCullParams -- NOT from the camera UBO, which describes the
    // observer once the culling camera is frozen.
    //
    // That seam has no other live coverage. GPUFrustumCullParityTest is a CPU
    // re-implementation of the shader's arithmetic and never dispatches it, so
    // it cannot see a std140 offset that shifted or a field nobody filled. The
    // failure this catches is the nastiest one available here: the CPU half of
    // the cut freezes, the GPU half keeps following the observer, and the frame
    // shows a coherent, plausible, WRONG visible set.
    // -------------------------------------------------------------------------
    class ObserverCameraGpuCullEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Above SubmitGPUCulledInstanced's threshold (1024) on purpose -- at or
        // below it the batch silently takes the CPU path and this test would
        // pass while proving nothing about the shader.
        static constexpr i32 kGridX = 40;
        static constexpr i32 kGridZ = 30;
        static constexpr f32 kHalfExtentX = 24.0f;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 6.0f, 20.0f, 10.0f };
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
            dl.m_Color = { 1.0f, 0.97f, 0.92f };
            dl.m_Intensity = 4.0f;

            const Ref<Mesh> cube = MeshPrimitives::CreateCube();

            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -1.2f, -8.0f };
                tc.Scale = { 90.0f, 0.2f, 60.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (cube)
                    mc.m_MeshSource = cube->GetMeshSource();
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.35f, 0.36f, 0.38f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            Entity field = scene.CreateEntity("InstancedField");
            // The warm base colour has to come from a MaterialComponent, not
            // from InstanceData::Color: the entity-owned instanced path picks
            // the entity's material (or the default one) for the whole batch,
            // and the first version of this test relied on the per-instance
            // colour, rendered a field of plain white cubes, and measured 105
            // "warm" pixels in every band -- which were the editor grid's red
            // axis line, identical frozen and unfrozen.
            auto& fieldMat = field.AddComponent<MaterialComponent>();
            fieldMat.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.45f, 0.10f, 1.0f));
            fieldMat.m_Material.SetMetallicFactor(0.0f);
            fieldMat.m_Material.SetRoughnessFactor(0.55f);

            auto& imc = field.AddComponent<InstancedMeshComponent>();
            if (cube)
                imc.MeshSource = cube->GetMeshSource();
            imc.CastShadows = false;
            imc.Instances.reserve(static_cast<sizet>(kGridX) * kGridZ);
            for (i32 gz = 0; gz < kGridZ; ++gz)
            {
                for (i32 gx = 0; gx < kGridX; ++gx)
                {
                    const f32 x = (static_cast<f32>(gx) / static_cast<f32>(kGridX - 1) - 0.5f) * (2.0f * kHalfExtentX);
                    const f32 z = -2.0f - (static_cast<f32>(gz) / static_cast<f32>(kGridZ - 1)) * 14.0f;
                    InstanceData inst;
                    inst.Transform = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, z)),
                                                glm::vec3(0.35f));
                    inst.PrevTransform = inst.Transform;
                    // Same warm colour the BandCubePixels test relies on: the
                    // scene is otherwise entirely neutral.
                    inst.Color = glm::vec4(0.95f, 0.55f, 0.15f, 1.0f);
                    imc.Instances.push_back(inst);
                }
            }
        }

        [[nodiscard]] static EditorCamera MakeFrozenPoseCamera()
        {
            EditorCamera camera(kFrozenFovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.1f, 300.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.0f, 16.0f), /*yaw*/ 0.0f, /*pitch*/ 0.0f);
            return camera;
        }

        // Looking down on the field: the frozen frustum's footprint on the
        // scatter is a wedge, and a wedge with geometry inside it and bare
        // ground outside is the single most legible form this diagnostic takes.
        [[nodiscard]] static EditorCamera MakeObserverCamera()
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 42.0f, 14.0f), /*yaw*/ 0.0f, /*pitch*/ glm::radians(66.0f));
            return camera;
        }

        void Capture(const EditorCamera& camera, const std::string& tag, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for GPU-cull capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir: " << ec.message();
            const std::string path = (dir / ("ObserverCamera_GpuCull_" + tag + ".png")).string();
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       outPixels.data(), static_cast<int>(kWidth) * 4),
                      0)
                << "stbi_write_png failed for '" << path << "'";
        }
    };

    TEST_F(ObserverCameraGpuCullEvidenceTest, ComputeCullHonoursTheFrozenCameraNotTheObserver)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ObserverStateRestore restore;

        static_assert(kGridX * kGridZ > 1024,
                      "the batch must exceed SubmitGPUCulledInstanced's threshold or this test "
                      "silently exercises the CPU cull instead of the compute shader");

        Renderer3D::GetRendererSettings().ShaderDebugDrawEnabled = false;
        Renderer3D::ApplyRendererSettings();
        Renderer3D::SetCullingCameraFrozen(false);

        std::vector<u8> unfrozen;
        Capture(MakeObserverCamera(), "unfrozen", unfrozen);
        ASSERT_FALSE(HasFatalFailure());

        std::vector<u8> atFrozenPose;
        Capture(MakeFrozenPoseCamera(), "frozen_pose_view", atFrozenPose);
        ASSERT_FALSE(HasFatalFailure());
        Renderer3D::SetCullingCameraFrozen(true);

        std::vector<u8> frozen;
        Capture(MakeObserverCamera(), "frozen", frozen);
        ASSERT_FALSE(HasFatalFailure());

        const u64 leftUnfrozen = BandCubePixels(unfrozen, kOuterLeft0, kOuterLeft1);
        const u64 leftFrozen = BandCubePixels(frozen, kOuterLeft0, kOuterLeft1);
        const u64 rightUnfrozen = BandCubePixels(unfrozen, kOuterRight0, kOuterRight1);
        const u64 rightFrozen = BandCubePixels(frozen, kOuterRight0, kOuterRight1);

        ASSERT_GT(leftUnfrozen, 0u) << "no instanced cubes in the left band even unfrozen - the field and the "
                                       "measured band have drifted apart, so this test proves nothing";
        ASSERT_GT(rightUnfrozen, 0u) << "no instanced cubes in the right band even unfrozen - see above";

        // The whole point: the compute shader must read the FROZEN camera. If it
        // kept reading the camera UBO these counts would not move at all, and
        // the frame would look perfectly reasonable.
        EXPECT_LT(leftFrozen, leftUnfrozen)
            << "the GPU instance cull kept drawing instances the frozen camera had culled (left band: "
            << leftUnfrozen << "px -> " << leftFrozen << "px). Check u_CullViewProjection in "
                                                         "InstanceFrustumCull.comp and its std140 twin InstanceCullUBO.";
        EXPECT_LT(rightFrozen, rightUnfrozen)
            << "the GPU instance cull kept drawing instances the frozen camera had culled (right band: "
            << rightUnfrozen << "px -> " << rightFrozen << "px)";

        // ...and it must not have culled the middle, which the frozen camera saw.
        const u64 centreUnfrozen = BandCubePixels(unfrozen, kCentre0, kCentre1);
        const u64 centreFrozen = BandCubePixels(frozen, kCentre0, kCentre1);
        ASSERT_GT(centreUnfrozen, 0u) << "no instanced cubes in the centre band even unfrozen";
        EXPECT_GT(centreFrozen, centreUnfrozen / 2u)
            << "the centre of the frozen frustum lost most of its instances (" << centreUnfrozen << "px -> "
            << centreFrozen << "px) - the cull is using neither camera correctly";
    }

    // The evidence image the issue actually asks for: one screenshot in which an
    // over-culling cut is diagnosable, WITH the frozen frustum drawn so the
    // missing cubes can be attributed to it rather than guessed at.
    TEST_F(ObserverCameraVisualEvidenceTest, FrozenFrustumWireframeIsDrawnFromTheObserver)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ObserverStateRestore restore;

        Renderer3D::GetRendererSettings().ShaderDebugDrawEnabled = true;
        Renderer3D::GetRendererSettings().ObserverCameraDrawFrustum = true;
        Renderer3D::ApplyRendererSettings();

        Renderer3D::SetCullingCameraFrozen(false);
        std::vector<u8> ignored;
        Capture(MakeFrozenPoseCamera(), "wireframe_pose_view", ignored);
        ASSERT_FALSE(HasFatalFailure());
        Renderer3D::SetCullingCameraFrozen(true);

        std::vector<u8> withFrustum;
        Capture(MakeObserverCamera(), "frozen_with_frustum", withFrustum);
        ASSERT_FALSE(HasFatalFailure());

        // Turning the wireframe off from the same pose must change the frame:
        // that difference IS the frustum. Asserted rather than eyeballed because
        // "the frustum drew nothing" and "the frustum overflowed its channel"
        // look identical in a PNG -- see docs/agent-rules/gpu-debug-draws.md §2b.
        Renderer3D::GetRendererSettings().ObserverCameraDrawFrustum = false;
        Renderer3D::ApplyRendererSettings();
        std::vector<u8> withoutFrustum;
        Capture(MakeObserverCamera(), "frozen_without_frustum", withoutFrustum);
        ASSERT_FALSE(HasFatalFailure());

        const f64 changed = BandFractionChanged(withFrustum, withoutFrustum, 0.0f, 1.0f, 8);
        EXPECT_GT(changed, 0.0005)
            << "the frozen-frustum wireframe drew nothing (" << changed
            << " of the frame differs with it on). Check the ShaderDebugDraw channel counters: an "
               "overflow and a no-op look the same on screen.";
    }
} // namespace OloEngine::Tests
