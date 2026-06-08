// =============================================================================
// SSGIVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the Screen-Space
// Global Illumination pass (PostProcess_SSGI.glsl / SSGIRenderPass).
//
// A neutral white floor sits in front of a bright RED emissive wall. With SSGI
// on, the wall's red light should bleed onto the floor in front of it (one-bounce
// indirect diffuse / colour bleeding). The scene is rendered twice through the
// FULL deferred Renderer3D pipeline from the same pose — once with SSGI OFF and
// once with SSGI ON — and both frames are written to
//   OloEditor/assets/tests/visual/SSGI_<state>.png
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs and
// needs no committed reference image: turning SSGI on must add the wall's red
// into the neutral floor region (the floor patch gets measurably redder), and
// that added light must be red-dominant (the bounced colour of the wall, not a
// grey brightening). The cheap SSGI *math* contracts (projection round-trip,
// cosine-weighted sampling, march bounds, fade curves) live in
// ScreenSpaceGIMathTest.cpp.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching SSRVisualEvidenceTest. SSGI is deferred-only, so the fixture forces
// the deferred render path.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

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

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 4.0f; // freeze the clock for deterministic frames

        struct BandStats
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;
        };

        // Mean RGB over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] BandStats SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count,
                     static_cast<f64>(sumB) / count };
        }
    } // namespace

    class SSGIVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // SSGI only runs on the deferred path (it reads the G-Buffer).
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Gentle sun so the white floor reads as a mid grey baseline (a black
            // floor would have nothing for the differential to brighten, and a
            // blown-out floor would swamp the red bounce).
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.85f, 0.3f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.2f;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                return e;
            };

            // Neutral white floor at y = 0. Pure-diffuse so any colour it gains is
            // the SSGI bounce, not a material tint.
            {
                Entity floor = addMesh("WhiteFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.85f, 0.85f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // Bright RED emissive back wall standing on the floor. Emissive so it is
            // unambiguously red in the lit scene colour the SSGI gather samples; its
            // light bounces forward onto the floor in front of it.
            {
                Entity wall = addMesh("RedWall", MeshPrimitive::Cube, { 0.0f, 5.0f, -9.0f },
                                      { 40.0f, 10.0f, 1.0f });
                auto& mat = wall.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(4.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        // Render the current scene/settings from the given pose, read back the
        // composited frame (top-down rows), save it as PNG evidence, and verify
        // the PNG round-trips (write succeeded + reloads bit-identical).
        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for SSGI capture '" << tag << "'";

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

            const std::string path = (dir / ("SSGI_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            EXPECT_EQ(ch, 4) << "Written PNG should have 4 channels (RGBA)";
            if (w == static_cast<int>(kWidth) && h == static_cast<int>(kHeight))
            {
                EXPECT_EQ(std::memcmp(loaded, outPixels.data(),
                                      static_cast<std::size_t>(kWidth) * kHeight * 4u),
                          0)
                    << "Reloaded PNG pixels differ from the written buffer: " << path;
            }
            ::stbi_image_free(loaded);
        }
    };

    // SSGI off vs on: the white floor in front of the red wall must gain the
    // wall's red as bounced indirect diffuse. Checked from TWO poses to catch
    // view-dependent regressions. SKIPs without a GL 4.6 context (see header).
    TEST_F(SSGIVisualEvidenceTest, ColorBleedAppearsOnFloor)
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

        auto& pp = Renderer3D::GetPostProcessSettings();
        const auto applyOnParams = [&pp]()
        {
            pp.SSGIEnabled = true;
            pp.SSGIIntensity = 2.5f;
            pp.SSGIMaxDistance = 30.0f;
            pp.SSGIThickness = 1.5f;
            pp.SSGIStride = 0.6f;
            pp.SSGIMaxSteps = 24; // keep the per-pixel ray budget modest so the GPU
            pp.SSGIRayCount = 8;  // evidence pass stays well under a minute
            pp.SSGIEdgeFade = 0.1f;
        };

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
            // Floor band (UV) near the wall base where the bounce is expected.
            f32 BandY0;
            f32 BandY1;
        };

        // The wall stands at z=-9; the strongest bleed is on the floor just in
        // front of its base, which sits in the upper-middle of a downward view.
        const std::array<Pose, 2> poses = { {
            { "Frontal", { 0.0f, 6.0f, 7.0f }, 0.0f, 0.45f, 0.50f, 0.74f },
            { "Lower", { 0.0f, 3.5f, 9.0f }, 0.0f, 0.28f, 0.52f, 0.80f },
        } };

        constexpr f32 bx0 = 0.32f, bx1 = 0.68f;

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            pp.SSGIEnabled = false;
            std::vector<u8> offPixels;
            Capture(std::string("Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, offPixels);
            if (::testing::Test::HasFatalFailure())
                return;
            const BandStats off = SampleBand(offPixels, bx0, bx1, pose.BandY0, pose.BandY1);

            applyOnParams();
            std::vector<u8> onPixels;
            Capture(std::string("On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, onPixels);
            if (::testing::Test::HasFatalFailure())
                return;
            const BandStats on = SampleBand(onPixels, bx0, bx1, pose.BandY0, pose.BandY1);

            // Both frames must be non-trivial (catch a black / failed render).
            EXPECT_GT(off.R + off.G + off.B, 5.0) << "SSGI-off frame rendered (near-)black";
            EXPECT_GT(on.R + on.G + on.B, 5.0) << "SSGI-on frame rendered (near-)black";

            // Core contract: SSGI brings the wall's red bounce into the floor band.
            EXPECT_GT(on.R, off.R + 6.0)
                << "Enabling SSGI did not add bounced red to the floor (off.R=" << off.R
                << " on.R=" << on.R << "). See SSGI_Off_" << pose.Name << ".png / SSGI_On_" << pose.Name << ".png";

            // The added light reads as red (the wall's colour), not a grey
            // brightening: red must rise more than green/blue.
            EXPECT_GT(on.R - off.R, (on.G - off.G) + 3.0)
                << "SSGI floor bounce is not red-dominant vs off (dR=" << (on.R - off.R)
                << " dG=" << (on.G - off.G) << " dB=" << (on.B - off.B) << "). See SSGI_On_" << pose.Name << ".png";
            EXPECT_GT(on.R - off.R, (on.B - off.B) + 3.0)
                << "SSGI floor bounce is not red-dominant vs off (dR=" << (on.R - off.R)
                << " dG=" << (on.G - off.G) << " dB=" << (on.B - off.B) << "). See SSGI_On_" << pose.Name << ".png";
        }
    }
} // namespace OloEngine::Tests
