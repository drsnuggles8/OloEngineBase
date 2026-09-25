// OLO_TEST_LAYER: integration
// =============================================================================
// WaterSingleDisplacementEvidenceTest.cpp — issue #1470.
//
// The water surface is displaced ONCE. Every water program has a tessellation-
// evaluation stage and every water draw is a patch list, so the TES always runs
// and always displaces; "tessellation off" (the WaterComponent default) only
// means a tess level of 1. The vertex stage used to displace as well whenever
// tessellation was off, and the TES then displaced the displaced point a second
// time — double the swell, double the choppy drift, sampled at the wrong XZ. On
// the integrated benchmark's 10 m tile under a 180 m FFT swell that was the
// whole tile sliding metres sideways between wave phases, which is what #1470
// was first reported as ("Vulkan renders different geometry than GL"; the two
// backends agreed once the editor capture pinned its clock).
//
// The prediction pinned here: at tessellation factor 1, "tess on" lays out the
// SAME patches as "tess off", so the two must render the same frame. Tess on is
// the path that always displaced once (its vertex stage passes through), so
// agreement means tess off displaces once too. With the vertex-stage
// displacement restored this fails by tens of thousands of pixels.
//
// Every capture writes WaterSingleDisplacement_GL_<Path>_<Pose>.png (tess off,
// the default) and WaterSingleDisplacementTessOn_GL_<Path>_<Pose>.png (the
// reference) under OloEditor/assets/tests/visual/. GL only: the fixture needs a
// GL 4.6 context and skips without one; the Vulkan cells are live-verified.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 450;
        constexpr f32 kCaptureTime = 7.0f;
        constexpr u32 kFramesPerCapture = 4;

        // A channel further apart than this is a difference. The two draws are
        // the same patches through the same stages, so they agree exactly where
        // they agree at all; the slack only absorbs a rasterisation tie.
        constexpr u32 kTolerance = 2;

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
        };

        struct Frame
        {
            std::vector<u8> Rgba; // rows top-down
            u32 Width = 0;
            u32 Height = 0;
        };

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            glm::vec3 Rotation; // pitch, yaw, roll (radians)
        };

        // Across the tile at a low eye (where a doubled crest reads as a slab),
        // steeply down onto it (where doubled choppiness moves the edges), and
        // from the side (the tile's silhouette against the floor).
        constexpr Pose kGrazing{ "Grazing", { 0.0f, 2.0f, 16.0f }, { -0.10f, 0.0f, 0.0f } };
        constexpr Pose kAbove{ "Above", { 0.0f, 18.0f, 6.0f }, { -1.20f, 0.0f, 0.0f } };
        constexpr Pose kSide{ "Side", { 17.0f, 5.0f, 3.0f }, { -0.25f, 1.25f, 0.0f } };

        void WritePng(const std::string& name, const Frame& f)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.generic_string();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(f.Width), static_cast<int>(f.Height), 4,
                                       f.Rgba.data(), static_cast<int>(f.Width * 4u)),
                      0)
                << "stbi_write_png failed for " << path;
        }

        // Water-blue pixels: blue clearly above red and above green. The floor
        // is grey and the sky is off (clear colour), so this counts the surface.
        [[nodiscard]] u64 CountWaterPixels(const Frame& f)
        {
            u64 count = 0;
            for (sizet i = 0; i + 3 < f.Rgba.size(); i += 4)
            {
                const int r = f.Rgba[i + 0];
                const int g = f.Rgba[i + 1];
                const int b = f.Rgba[i + 2];
                count += (b > r + 25 && b > g + 5) ? 1u : 0u;
            }
            return count;
        }

        [[nodiscard]] u64 CountDifferingPixels(const Frame& a, const Frame& b, u32& outMaxAbs)
        {
            outMaxAbs = 0;
            u64 over = 0;
            for (sizet i = 0; i + 3 < a.Rgba.size() && i + 3 < b.Rgba.size(); i += 4)
            {
                u32 worst = 0;
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, static_cast<u32>(std::abs(static_cast<int>(a.Rgba[i + c]) -
                                                                      static_cast<int>(b.Rgba[i + c]))));
                outMaxAbs = std::max(outMaxAbs, worst);
                over += (worst > kTolerance) ? 1u : 0u;
            }
            return over;
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
                default:
                    return "Other";
            }
        }
    } // namespace

    class WaterSingleDisplacementTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.GTAOEnabled = false;
            pp.SSAOEnabled = false;

            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
            dl.m_Intensity = 2.0f;

            // A grey floor under the tile, so the tile's edges and any see-through
            // read against something that is not water-blue.
            Entity floor = scene.CreateEntity("Floor");
            auto& ftc = floor.GetComponent<TransformComponent>();
            ftc.Translation = { 0.0f, -2.5f, 0.0f };
            ftc.Scale = { 60.0f, 1.0f, 60.0f };
            auto& mc = floor.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                mc.m_MeshSource = plane->GetMeshSource();
            auto& mat = floor.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.55f, 0.55f, 0.52f, 1.0f));

            // A finite tile with a strong analytic swell: 1 m Gerstner amplitude
            // on a 16 m tile, so a doubled displacement moves the surface by a
            // visible fraction of the tile. Analytic rather than FFT so the
            // fixture needs no compute field; the TES treats both the same way.
            m_Water = scene.CreateEntity("Water");
            auto& wc = m_Water.AddComponent<WaterComponent>();
            wc.m_WorldSizeX = 16.0f;
            wc.m_WorldSizeZ = 16.0f;
            wc.m_GridResolutionX = 64;
            wc.m_GridResolutionZ = 64;
            wc.m_WaveAmplitude = 1.0f;
            wc.m_WaveFrequency = 0.5f;
            wc.m_TessellationEnabled = false;
            // Factor 1: "on" subdivides nothing, so it is the same patch layout.
            wc.m_TessellationFactor = 1.0f;

            m_Camera = scene.CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            cam.Camera.SetPerspectiveNearClip(0.05f);
            cam.Camera.SetPerspectiveFarClip(500.0f);
            cam.Camera.SetViewportSize(kWidth, kHeight);
        }

        Frame Capture(const std::string& name)
        {
            Frame f;
            RunFrames(kFramesPerCapture);
            std::vector<u8> bottomUp;
            if (!ReadbackComposite(bottomUp, f.Width, f.Height))
            {
                ADD_FAILURE() << "composite readback unavailable for " << name;
                return f;
            }
            f.Rgba.resize(bottomUp.size());
            const sizet rowBytes = static_cast<sizet>(f.Width) * 4u;
            for (u32 y = 0; y < f.Height; ++y)
                std::memcpy(f.Rgba.data() + y * rowBytes, bottomUp.data() + (f.Height - 1u - y) * rowBytes, rowBytes);
            WritePng(name, f);
            return f;
        }

        Entity m_Water;
        Entity m_Camera;
    };

    TEST_F(WaterSingleDisplacementTest, TessellationOffDisplacesOnceOnEveryPathFromThreeAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            for (const Pose& pose : { kGrazing, kAbove, kSide })
            {
                auto& tc = m_Camera.GetComponent<TransformComponent>();
                tc.Translation = pose.Position;
                tc.SetRotationEuler(pose.Rotation);
                const std::string cell = std::string("_GL_") + PathName(path) + "_" + pose.Name;

                m_Water.GetComponent<WaterComponent>().m_TessellationEnabled = false;
                const Frame off = Capture("WaterSingleDisplacement" + cell);
                m_Water.GetComponent<WaterComponent>().m_TessellationEnabled = true;
                const Frame on = Capture("WaterSingleDisplacementTessOn" + cell);
                ASSERT_FALSE(HasFatalFailure());

                u32 maxAbs = 0;
                const u64 differing = CountDifferingPixels(off, on, maxAbs);
                const u64 water = CountWaterPixels(off);
                std::printf("[#1470] %-28s water px %7llu | tess off vs on: %7llu px over %u (max %3u)\n",
                            cell.c_str() + 1, static_cast<unsigned long long>(water),
                            static_cast<unsigned long long>(differing), kTolerance, maxAbs);

                // The surface is in the frame — agreement is not two empty frames.
                EXPECT_GT(water, static_cast<u64>(kWidth) * kHeight / 20u)
                    << cell << ": too little water in the frame to compare";
                EXPECT_LE(differing, static_cast<u64>(kWidth) * kHeight / 5000u)
                    << cell << ": tessellation OFF renders a different surface than tessellation ON at level 1 ("
                    << differing << " pixels, max " << maxAbs
                    << ") — the water is being displaced twice (issue #1470). Compare WaterSingleDisplacement"
                    << cell << ".png with WaterSingleDisplacementTessOn" << cell << ".png";
            }
        }
    }
} // namespace OloEngine::Tests
