// =============================================================================
// WaterBuoyancyVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the buoyancy system (WATER_FUTURE_IMPROVEMENTS.md
// §5.1) inside a REAL rendered water scene. A dynamic cube with a
// BuoyancyComponent is dropped above a WaterComponent ocean and the FULL
// Scene::OnUpdateRuntime -> Renderer3D pipeline is driven for enough frames that
// Jolt + BuoyancySystem catch it and float it at the waterline. The composited
// frame (the same image the editor viewport shows) is read back and written to
//   OloEditor/assets/tests/visual/WaterBuoyancy_VisualEvidence.png
// so a human can eyeball the cube actually floating on the waves.
//
// This is the "buoyant object in the water scene" test: unlike the headless
// WaterBuoyancyTest (which proves the physics numerically) this one runs the
// physics AND the renderer together, so it both asserts the cube settled at the
// surface and produces a frame proving it's visible there.
//
// Driven through RendererAttachedTest::RunFrames (runtime primary camera, full
// pipeline, each tick wrapped in a GLStateGuard). Physics is brought up the same
// way Functional::FunctionalTest::EnablePhysics3D does — the engine task
// scheduler (Jolt queues into it from Simulate) plus Scene::OnPhysics3DStart.
// Time is frozen so the wave phase — and therefore the rest height — is
// deterministic.
//
// SKIPs (not fails) without a GL 4.6 context, like the other evidence tests.
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path + physics, RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/NamedThreads.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 384;
        constexpr u32 kHeight = 256;
        constexpr f32 kWaterPlaneY = 0.0f;
        constexpr f32 kBoxHalfExtent = 0.5f;

        // Bring up the engine task scheduler exactly once per process (idempotent
        // — FScheduler::StartWorkers no-ops when workers are already active). Jolt's
        // JoltJobSystemAdapter queues into the scheduler from inside Simulate(), so
        // it must be running before any RunFrames that steps physics.
        void EnsureTaskSchedulerStarted()
        {
            static const bool s_Once = []
            {
                LowLevelTasks::InitGameThreadId();
                Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
                LowLevelTasks::FScheduler::Get().StartWorkers();
                return true;
            }();
            (void)s_Once;
        }

        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        f32 MeanLuminanceInRegion(const std::vector<u8>& px, u32 w, u32 h, u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = (cx > halfExtent) ? cx - halfExtent : 0u;
            const u32 y0 = (cy > halfExtent) ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, w);
            const u32 y1 = std::min(cy + halfExtent, h);
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = y0; y < y1; ++y)
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4);
                    ++count;
                }
            return count ? static_cast<f32>(sum / count) : 0.0f;
        }

        // Flip RGBA8 rows in place — glGetTextureImage is bottom-up; PNGs are top-down.
        void FlipVertically(std::vector<u8>& px, u32 w, u32 h)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(w) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < h / 2u; ++y)
            {
                u8* top = px.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = px.data() + static_cast<std::size_t>(h - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        }

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "WaterBuoyancy_VisualEvidence.png";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // FloatingCubeOnOcean — a buoyant cube dropped onto a rendered ocean.
    // -------------------------------------------------------------------------
    class FloatingCubeOnOcean : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Runtime primary camera: at the waterline, looking across the ocean so
            // the cube is framed straddling the surface.
            Entity camera = scene.CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.4f, 6.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // Sun.
            Entity sun = scene.CreateEntity("Sun");
            sun.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.4f));
            dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
            dl.m_Intensity = 3.0f;

            // Ocean at y = 0 (gentle waves so the rest height stays near the plane).
            Entity ocean = scene.CreateEntity("Ocean");
            ocean.GetComponent<TransformComponent>().Translation = { 0.0f, kWaterPlaneY, 0.0f };
            auto& wc = ocean.AddComponent<WaterComponent>();
            wc.m_WorldSizeX = 200.0f;
            wc.m_WorldSizeZ = 200.0f;
            wc.m_GridResolutionX = 128;
            wc.m_GridResolutionZ = 128;
            wc.m_WaveAmplitude = 0.3f;

            // Buoyant cube dropped from above the surface, bright orange so it reads
            // clearly against the blue water. Mass ≈ half the displaced water so it
            // floats with its centre at the waterline (see WaterBuoyancyTest).
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Entity buoy = scene.CreateEntity("FloatingCube");
            auto& bt = buoy.GetComponent<TransformComponent>();
            bt.Translation = { 0.0f, 2.0f, 0.0f };
            if (cube)
                buoy.AddComponent<MeshComponent>(cube->GetMeshSource());
            auto& mat = buoy.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.45f, 0.1f, 1.0f));

            auto& body = buoy.AddComponent<Rigidbody3DComponent>();
            body.m_Type = BodyType3D::Dynamic;
            body.m_Mass = 250.0f;
            body.m_LinearDrag = 0.0f;
            body.m_AngularDrag = 0.0f;

            auto& col = buoy.AddComponent<BoxCollider3DComponent>();
            col.m_HalfExtents = glm::vec3(kBoxHalfExtent);

            auto& buoyancy = buoy.AddComponent<BuoyancyComponent>();
            buoyancy.m_ProbeExtents = glm::vec3(kBoxHalfExtent);
            buoyancy.m_FluidDensity = 1000.0f;
            buoyancy.m_SubmergenceRamp = 1.0f;
            buoyancy.m_LinearDrag = 4.0f;
            buoyancy.m_AngularDrag = 2.0f;

            m_Buoy = buoy;

            EnableRendering(kWidth, kHeight);
        }

        void TearDown() override
        {
            Time::ClearMockTime();
            RendererAttachedTest::TearDown();
        }

        Entity m_Buoy;
    };

    TEST_F(FloatingCubeOnOcean, CubeFloatsOnRenderedOceanAndProducesPng)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Deterministic, frozen wave phase so the rest height (and the PNG) are stable.
        Time::SetMockTime(0.0f);

        // Physics bring-up (mirrors Functional::FunctionalTest::EnablePhysics3D):
        // start the task scheduler Jolt queues into, then initialise the Scene's
        // JoltScene + create bodies from the rigidbody/collider entities built above.
        EnsureTaskSchedulerStarted();
        GetScene().OnPhysics3DStart();

        const f32 startY = m_Buoy.GetComponent<TransformComponent>().Translation.y;
        ASSERT_GT(startY, 1.0f) << "cube should start above the water";

        // Drive physics + the full render pipeline together until the cube settles
        // (~4 s of simulated time; strong submerged drag damps the drop quickly).
        RunFrames(240);

        const f32 restY = m_Buoy.GetComponent<TransformComponent>().Translation.y;

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);
        FlipVertically(px, width, height);

        // Always write the PNG first so the artifact survives a later assertion.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(width),
                                           static_cast<int>(height), 4, px.data(),
                                           static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // (1) Physics contract: the cube was caught by buoyancy and rests near the
        // waterline — it neither sank away nor was ejected.
        EXPECT_NEAR(restY, kWaterPlaneY, 0.5f)
            << "cube did not float at the waterline; restY=" << restY << " (see " << out.string() << ")";
        EXPECT_LT(restY, startY) << "cube never fell toward the water";

        // (2) Frame is non-trivial: the lit cube + ocean produce real contrast, not
        // a flat clear colour.
        f32 minLum = 1.0f;
        f32 maxLum = 0.0f;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const f32 l = LuminanceAt(px, i);
            minLum = std::min(minLum, l);
            maxLum = std::max(maxLum, l);
        }
        EXPECT_GT(maxLum - minLum, 0.05f)
            << "rendered frame is nearly flat (min=" << minLum << ", max=" << maxLum
            << ") — nothing drew; see " << out.string();

        // (3) The cube is visible at frame centre: a warm (orange) object reads with
        // R notably above B there, unlike the surrounding blue/teal water.
        u32 bestRminusB = 0;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const int r = px[i + 0];
            const int b = px[i + 2];
            if (r - b > static_cast<int>(bestRminusB))
                bestRminusB = static_cast<u32>(std::max(0, r - b));
        }
        EXPECT_GT(bestRminusB, 30u)
            << "no warm (orange) cube pixels found over the blue water — the cube may have "
            << "sunk out of view; see " << out.string();
    }
} // namespace OloEngine::Tests
