// OLO_TEST_LAYER: L8
// =============================================================================
// FluidVisualEvidenceTest.cpp
//
// Visual evidence for the screen-space fluid rendering (issue #630): a real
// scene with a prefilled FluidComponent pool is ticked through
// Scene::OnUpdateRuntime (GPU PBF solver + FluidIntermediates/FluidComposite
// passes) and captured from multiple camera angles to
// OloEditor/assets/tests/visual/Fluid_*.png.
//
// No golden-image comparison: the GPU solver's atomic neighbour sums make the
// particle configuration run-to-run nondeterministic, so pixel-exact goldens
// would flake. The contracts are driver-independent instead:
//   * every capture is materially non-black (the scene rendered);
//   * the fluid changes the frame — the same pose rendered with the fluid
//     disabled must differ significantly inside the pool region;
//   * PNGs are always written so a human can actually look at the water
//     (CLAUDE.md: trust the image, not the green test).
//
// SKIPs cleanly when no GL 4.5+ context exists (headless CI).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Fluid/FluidWorld.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        struct CapturePose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Side-on (the classic "is it transparent" breakage angle), high
        // three-quarter (foam/thickness), and near-waterline. Yaw convention
        // (WaterVisualEvidenceTest): yaw 0 at +z looks toward -z, yaw -90 at +x
        // looks toward -x; POSITIVE pitch looks down.
        constexpr CapturePose kPoses[] = {
            { "Side", { 7.5f, 2.5f, 0.0f }, glm::radians(-90.0f), glm::radians(8.0f) },
            { "ThreeQuarter", { 6.0f, 5.5f, 6.0f }, glm::radians(-45.0f), glm::radians(30.0f) },
            { "Waterline", { 4.5f, 1.9f, 4.5f }, glm::radians(-45.0f), glm::radians(4.0f) },
        };

        [[nodiscard]] f64 MeanChannel(const std::vector<u8>& rgba)
        {
            f64 sum = 0.0;
            for (sizet i = 0; i < rgba.size(); ++i)
            {
                if (i % 4 != 3)
                {
                    sum += rgba[i];
                }
            }
            return rgba.empty() ? 0.0 : sum / (static_cast<f64>(rgba.size()) * 3.0 / 4.0);
        }

        /// Mean absolute per-channel difference between two same-sized images.
        [[nodiscard]] f64 MeanAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            for (sizet i = 0; i < a.size(); ++i)
            {
                if (i % 4 != 3)
                {
                    sum += std::abs(static_cast<f64>(a[i]) - static_cast<f64>(b[i]));
                }
            }
            return sum / (static_cast<f64>(a.size()) * 3.0 / 4.0);
        }

        void WritePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            std::filesystem::create_directories("assets/tests/visual");
            const std::string path = "assets/tests/visual/" + name + ".png";
            stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                           rgba.data(), static_cast<int>(width * 4));
        }
    } // namespace

    class FluidVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);

            Entity sun = GetScene().CreateEntity("Sun");
            sun.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.5f));
            light.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
            light.m_Intensity = 2.0f;

            // Ground so the pool has context (single-mesh scenes render
            // near-black — docs/agent-rules/single-mesh-visual-test-lighting.md).
            Entity ground = GetScene().CreateEntity("Ground");
            ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
            ground.GetComponent<TransformComponent>().Scale = { 20.0f, 1.0f, 20.0f };
            auto& groundMesh = ground.AddComponent<MeshComponent>();
            groundMesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
            {
                groundMesh.m_MeshSource = cube->GetMeshSource();
            }
            auto& groundMat = ground.AddComponent<MaterialComponent>();
            groundMat.m_Material.SetBaseColorFactor(glm::vec4(0.55f, 0.5f, 0.45f, 1.0f));

            m_Fluid = GetScene().CreateEntity("FluidPool");
            m_Fluid.GetComponent<TransformComponent>().Translation = { 0.0f, 1.5f, 0.0f };
            auto& fluid = m_Fluid.AddComponent<FluidComponent>();
            fluid.m_Enabled = true;
            fluid.m_DomainHalfExtents = { 2.5f, 1.5f, 2.5f };
            fluid.m_MaxParticles = 16384;
            fluid.m_SolverMode = FluidSolverMode::Auto; // renderer up => GPU
            fluid.m_PrefillFraction = 0.5f;
        }

        [[nodiscard]] bool Capture(const CapturePose& pose, std::vector<u8>& outRgba)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // SetPose is the only setter that rebuilds the view matrix.
            camera.SetPose(pose.Position, pose.Yaw, pose.Pitch);
            RunEditorFrames(camera, 2);

            Ref<Framebuffer> fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            if (!fb)
            {
                return false;
            }

            std::vector<u8> upsideDown;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, upsideDown);
            if (upsideDown.size() != static_cast<sizet>(kWidth) * kHeight * 4)
            {
                return false;
            }

            // GL rows are bottom-up.
            outRgba.resize(upsideDown.size());
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4;
            for (u32 y = 0; y < kHeight; ++y)
            {
                std::copy_n(upsideDown.data() + static_cast<sizet>(kHeight - 1 - y) * rowBytes,
                            rowBytes, outRgba.data() + static_cast<sizet>(y) * rowBytes);
            }
            return true;
        }

        Entity m_Fluid;
    };

    // -------------------------------------------------------------------------
    // Play-mode stress: GPU solver + Jolt bodies + emitter + full renderer, the
    // exact combination the FluidDamBreak demo scene runs in editor Play mode
    // (a nondeterministic debug-break was observed there; this pins it in a
    // console harness where any assert/fault message is visible).
    // -------------------------------------------------------------------------
    TEST_F(FluidVisualEvidenceTest, PlayModeStressWithBodiesAndEmitterSurvives)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Scene& scene = GetScene();

        // Emitter pouring into the pool.
        Entity emitter = scene.CreateEntity("Faucet");
        emitter.GetComponent<TransformComponent>().Translation = { 1.5f, 2.6f, 0.0f };
        emitter.GetComponent<TransformComponent>().SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });
        auto& ec = emitter.AddComponent<FluidEmitterComponent>();
        ec.m_Rate = 1500.0f;
        ec.m_Speed = 3.0f;
        ec.m_SpreadRadius = 0.2f;

        // Ground + a dynamic crate inside the domain (proxy + feedback path).
        Entity ground = scene.CreateEntity("GroundBody");
        ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& groundRb = ground.AddComponent<Rigidbody3DComponent>();
        groundRb.m_Type = BodyType3D::Static;
        ground.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 10.0f, 0.5f, 10.0f };

        Entity crate = scene.CreateEntity("Crate");
        crate.GetComponent<TransformComponent>().Translation = { -0.8f, 2.0f, 0.0f };
        auto& crateRb = crate.AddComponent<Rigidbody3DComponent>();
        crateRb.m_Type = BodyType3D::Dynamic;
        crateRb.m_Mass = 40.0f;
        crate.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 0.3f, 0.3f, 0.3f };

        // Physics needs the engine task scheduler (JoltJobSystemAdapter queues
        // into it from Simulate) — FunctionalTest::EnablePhysics3D does the
        // same dance; this fixture doesn't construct an Application either.
        static const bool s_TaskSchedulerOnce = []
        {
            LowLevelTasks::InitGameThreadId();
            Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
            LowLevelTasks::FScheduler::Get().StartWorkers();
            return true;
        }();
        (void)s_TaskSchedulerOnce;
        scene.OnPhysics3DStart();

        // 5 simulated seconds of the full loop: fluid GPU step + coupling at
        // the kick seam, Jolt world step, and the render graph every frame.
        RunFrames(300);

        FluidWorld* world = scene.TryGetFluidWorld();
        ASSERT_NE(world, nullptr);
        FluidInstance* instance = world->Find(m_Fluid.GetComponent<IDComponent>().ID);
        ASSERT_NE(instance, nullptr);
        ASSERT_TRUE(instance->Gpu || instance->Cpu);
        if (instance->Gpu)
        {
            EXPECT_GT(instance->Gpu->GetParticleUpperBound(), 0u);
        }
        EXPECT_TRUE(std::isfinite(crate.GetComponent<TransformComponent>().Translation.y));
    }

    TEST_F(FluidVisualEvidenceTest, PoolRendersFromMultipleAnglesAndChangesTheFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Let the prefilled pool seed, settle a little, and submit draws.
        RunFrames(30);

        // --- Captures with the fluid visible --------------------------------
        std::vector<std::vector<u8>> withFluid(std::size(kPoses));
        for (sizet i = 0; i < std::size(kPoses); ++i)
        {
            ASSERT_TRUE(Capture(kPoses[i], withFluid[i])) << kPoses[i].Name;
            WritePng(std::string("Fluid_") + kPoses[i].Name, withFluid[i], kWidth, kHeight);

            const f64 mean = MeanChannel(withFluid[i]);
            EXPECT_GT(mean, 5.0) << "capture " << kPoses[i].Name << " is black — nothing rendered";
        }

        // --- Control: identical poses, fluid disabled ------------------------
        m_Fluid.GetComponent<FluidComponent>().m_Enabled = false;
        RunFrames(2); // let the disable propagate (instance sweeps, no draws submitted)

        for (sizet i = 0; i < std::size(kPoses); ++i)
        {
            std::vector<u8> control;
            ASSERT_TRUE(Capture(kPoses[i], control)) << kPoses[i].Name;
            if (i == 0)
            {
                WritePng("Fluid_Side_ControlNoFluid", control, kWidth, kHeight);
            }

            // The fluid must actually change the image. The pool covers a
            // large fraction of every pose, so even a subtle shading pass
            // moves the mean absolute difference well past readback noise
            // (identical GPU frames reread give ~0).
            // Threshold: identical frames re-read give ~0.0 (the failed-pose
            // captures measured exactly 0), while the fluid's footprint from
            // the farthest pose measures ~0.6 — 0.3 splits them with margin
            // on both sides without depending on pool coverage per pose.
            const f64 diff = MeanAbsDiff(withFluid[i], control);
            EXPECT_GT(diff, 0.3)
                << "pose " << kPoses[i].Name
                << ": with-fluid and no-fluid frames are near-identical — the composite drew nothing";
        }
    }
} // namespace OloEngine::Tests
