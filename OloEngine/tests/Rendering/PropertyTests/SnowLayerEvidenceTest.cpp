// OLO_TEST_LAYER: integration
// =============================================================================
// SnowLayerEvidenceTest.cpp — issue #1451 on the REAL pipeline.
//
// 1. THE MASK COVERS SNOW AND NOTHING ELSE. The snow subsurface blur used to
//    read its mask from scene-colour alpha, which every non-snow writer sets to
//    1, so with Snow.SSSBlurEnabled on it blurred every opaque pixel in the
//    frame. The mask now travels in the skin-diffusion hand-off lane as
//    -snowWeight. With the blur on, every pixel of a surface that carries no
//    snow must be byte-identical to the blur-off frame, while the snow itself
//    changes (the positive control).
// 2. SNOW LOOKS THE SAME ON EVERY PATH. Deferred used to drop the snow weight
//    in the G-Buffer (no blur, no sparkle, and PBR meshes got no snow at all).
//    The weight now rides G-Buffer RT3.a, so a snowy scene must render the same
//    on Forward, Forward+ and Deferred, within a stated noise floor.
//
// The scene is split: the LEFT half is ground below the snow line with an
// emissive box and a glossy box on it, and nothing there may carry snow; the
// RIGHT half is a raised platform above the snow line, fully snow-covered, with
// a box on it. A strong low sun makes the sparkle and the snow's shading visible.
//
// Evidence PNGs: Snow<Case>[Off]_GL_<Path>.png under OloEditor/assets/tests/visual/.
// GL only: the headless fixtures need a GL 4.6 context and skip without one; the
// Vulkan cells are live-verified.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kFramesPerCapture = 8;

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

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        struct RegionDiff
        {
            f64 MeanA = 0.0;
            f64 MeanAbsDiff = 0.0;
            // Per channel as well as luminance: two colours of equal luma
            // must not pass as the same snow.
            std::array<f64, 3> MeanAbsDiffRgb{};
            u32 MaxAbsDiff = 0;
            u64 Changed = 0; // pixels with any channel differing
            u64 Count = 0;
        };

        // UV rectangle, rows top-down.
        [[nodiscard]] RegionDiff CompareRegion(const std::vector<u8>& a, const std::vector<u8>& b, f32 x0, f32 x1,
                                               f32 y0, f32 y1)
        {
            RegionDiff d;
            for (u32 y = static_cast<u32>(y0 * kHeight); y < static_cast<u32>(y1 * kHeight); ++y)
            {
                for (u32 x = static_cast<u32>(x0 * kWidth); x < static_cast<u32>(x1 * kWidth); ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    d.MeanA += Luma(&a[idx]);
                    d.MeanAbsDiff += std::abs(Luma(&a[idx]) - Luma(&b[idx]));
                    u32 pixelMax = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        const u32 channelDiff =
                            static_cast<u32>(std::abs(static_cast<int>(a[idx + c]) - static_cast<int>(b[idx + c])));
                        d.MeanAbsDiffRgb[static_cast<std::size_t>(c)] += channelDiff;
                        pixelMax = std::max(pixelMax, channelDiff);
                    }
                    d.MaxAbsDiff = std::max(d.MaxAbsDiff, pixelMax);
                    if (pixelMax > 0)
                        ++d.Changed;
                    ++d.Count;
                }
            }
            if (d.Count > 0)
            {
                d.MeanA /= static_cast<f64>(d.Count);
                d.MeanAbsDiff /= static_cast<f64>(d.Count);
                for (f64& channel : d.MeanAbsDiffRgb)
                    channel /= static_cast<f64>(d.Count);
            }
            return d;
        }
    } // namespace

    class SnowLayerTest : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        void TearDown() override
        {
            Renderer3D::GetSnowSettings() = m_SavedSnow;
            RendererAttachedTest::TearDown();
        }

        Entity AddBox(const char* name, const glm::vec3& position, const glm::vec3& scale, const glm::vec3& albedo,
                      f32 roughness, const glm::vec3& emissive)
        {
            Scene& scene = GetScene();
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(roughness);
            return e;
        }

        void BuildScene() override
        {
            m_SavedSnow = Renderer3D::GetSnowSettings();
            EnableRendering(kWidth, kHeight);
            UsePath(GetParam());

            Scene& scene = GetScene();
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.35f, -0.55f, -0.75f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 4.0f;

            // LEFT: below the snow line (tops at y <= 1.2, the line starts at 2).
            AddBox("Ground", { -6.0f, -0.5f, 0.0f }, { 12.0f, 1.0f, 30.0f }, glm::vec3(0.35f, 0.3f, 0.25f), 0.8f,
                   glm::vec3(0.0f));
            AddBox("Emissive", { -3.0f, 0.6f, 0.5f }, { 1.2f, 1.2f, 1.2f }, glm::vec3(0.1f), 0.5f,
                   glm::vec3(0.9f, 0.4f, 0.1f));
            AddBox("Glossy", { -1.4f, 0.5f, -0.6f }, { 1.0f, 1.0f, 1.0f }, glm::vec3(0.6f, 0.6f, 0.65f), 0.15f,
                   glm::vec3(0.0f));
            // RIGHT: a platform whose top (y = 3) is above the full-coverage
            // height, so it and the box on it are fully snow-covered.
            AddBox("Platform", { 6.0f, 1.5f, 0.0f }, { 12.0f, 3.0f, 30.0f }, glm::vec3(0.35f, 0.3f, 0.25f), 0.8f,
                   glm::vec3(0.0f));
            AddBox("SnowyBox", { 3.0f, 3.6f, 0.0f }, { 1.2f, 1.2f, 1.2f }, glm::vec3(0.5f, 0.2f, 0.2f), 0.6f,
                   glm::vec3(0.0f));

            SnowSettings& snow = Renderer3D::GetSnowSettings();
            snow = SnowSettings{};
            snow.Enabled = true;
            snow.HeightStart = 2.0f;
            snow.HeightFull = 2.6f;
            snow.SSSBlurEnabled = false;
        }

        void UsePath(RenderingPath path)
        {
            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = path;
            // The editor grid is unlit overlay geometry drawn over the ground;
            // it would only dilute every measurement below.
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.SSAOEnabled = false;
            pp.GTAOEnabled = false;
            Renderer3D::ApplyRendererSettings();
        }

        void Capture(const std::string& name, const glm::vec3& position, f32 yaw, f32 pitch, std::vector<u8>& out)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, kFramesPerCapture);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(fb) << "no composited framebuffer for '" << name << "'";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = out.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.generic_string();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, out.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "stbi_write_png failed for " << path;
        }

        SnowSettings m_SavedSnow{};
    };

    std::string SnowPathName(const ::testing::TestParamInfo<RenderingPath>& info)
    {
        return PathName(info.param);
    }

    // The front pose: the snow-free half fills the left of the frame, the
    // snow-covered platform the right.
    constexpr glm::vec3 kFrontPose{ 0.0f, 5.0f, 11.0f };
    constexpr f32 kFrontYaw = 0.0f;
    constexpr f32 kFrontPitch = 0.4f;
    // Snow-free screen region (well inside the left half, clear of the seam).
    constexpr f32 kClearX0 = 0.02f, kClearX1 = 0.40f, kClearY0 = 0.30f, kClearY1 = 0.98f;
    // Snow-covered screen region: the platform top to the right of the seam.
    constexpr f32 kSnowX0 = 0.62f, kSnowX1 = 0.98f, kSnowY0 = 0.45f, kSnowY1 = 0.98f;

    TEST_P(SnowLayerTest, TheBlurTouchesSnowAndNothingElse)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const std::string path = PathName(GetParam());

        Renderer3D::GetSnowSettings().SSSBlurEnabled = false;
        std::vector<u8> off;
        Capture("SnowBlurOff_GL_" + path, kFrontPose, kFrontYaw, kFrontPitch, off);
        ASSERT_FALSE(HasFatalFailure());

        SnowSettings& snow = Renderer3D::GetSnowSettings();
        snow.SSSBlurEnabled = true;
        snow.SSSBlurRadius = 3.0f;
        std::vector<u8> on;
        Capture("SnowBlur_GL_" + path, kFrontPose, kFrontYaw, kFrontPitch, on);
        ASSERT_FALSE(HasFatalFailure());

        const RegionDiff clear = CompareRegion(off, on, kClearX0, kClearX1, kClearY0, kClearY1);
        const RegionDiff snowy = CompareRegion(off, on, kSnowX0, kSnowX1, kSnowY0, kSnowY1);
        EXPECT_GT(clear.MeanA, 10.0) << "the snow-free half rendered (near-)black";
        EXPECT_GT(snowy.MeanA, 60.0) << "the snow-covered half is not bright; is there snow at all?";

        // Positive control: the blur ran, and it changed the snow.
        EXPECT_GT(snowy.Changed, 200u) << "the blur changed only " << snowy.Changed << " snow pixels, so the "
                                       << "assertion below would be vacuous — see SnowBlur_GL_" << path << ".png";

        EXPECT_EQ(clear.Changed, 0u)
            << "THE SNOW BLUR CHANGED " << clear.Changed << " PIXELS OF SURFACES THAT CARRY NO SNOW on " << path
            << " (max " << clear.MaxAbsDiff << " levels). The mask must cover snow pixels only. Compare "
            << "SnowBlurOff_GL_" << path << ".png / SnowBlur_GL_" << path << ".png.";
    }

    TEST_P(SnowLayerTest, SnowLooksTheSameAsOnTheForwardPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const RenderingPath cell = GetParam();
        if (cell == RenderingPath::Forward)
            GTEST_SKIP() << "Forward is the reference every other path is compared against";

        struct Pose
        {
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
            const char* Name;
        };
        const std::array<Pose, 2> poses = { {
            { kFrontPose, kFrontYaw, kFrontPitch, "Front" },
            { { 9.0f, 6.0f, 6.0f }, 0.75f, 0.45f, "Platform" },
        } };

        for (const bool blur : { false, true })
        {
            Renderer3D::GetSnowSettings().SSSBlurEnabled = blur;
            for (const Pose& pose : poses)
            {
                SCOPED_TRACE(std::string(pose.Name) + (blur ? " blur" : " no blur"));
                const std::string suffix = std::string("_") + pose.Name + (blur ? "_Blur" : "");
                UsePath(RenderingPath::Forward);
                std::vector<u8> reference;
                Capture("SnowParity_GL_Forward" + suffix, pose.Position, pose.Yaw, pose.Pitch, reference);
                ASSERT_FALSE(HasFatalFailure());
                UsePath(cell);
                std::vector<u8> frame;
                Capture(std::string("SnowParity_GL_") + PathName(cell) + suffix, pose.Position, pose.Yaw, pose.Pitch,
                        frame);
                ASSERT_FALSE(HasFatalFailure());

                const RegionDiff whole = CompareRegion(reference, frame, 0.0f, 1.0f, 0.0f, 1.0f);
                EXPECT_GT(whole.MeanA, 30.0) << "the Forward reference rendered (near-)black";
                EXPECT_LT(whole.MeanAbsDiff, 1.5)
                    << PathName(cell) << " SNOW DIFFERS FROM FORWARD: mean |d luma| " << whole.MeanAbsDiff << ", max "
                    << whole.MaxAbsDiff << ", " << whole.Changed << " pixels differ. Compare SnowParity_GL_Forward"
                    << suffix << ".png / SnowParity_GL_" << PathName(cell) << suffix << ".png.";
                const f64 worstChannel = *std::ranges::max_element(whole.MeanAbsDiffRgb);
                EXPECT_LT(worstChannel, 1.5)
                    << PathName(cell) << " SNOW COLOUR DIFFERS FROM FORWARD: mean |d| per channel (r,g,b) = ("
                    << whole.MeanAbsDiffRgb[0] << ", " << whole.MeanAbsDiffRgb[1] << ", " << whole.MeanAbsDiffRgb[2]
                    << "), so equal luminance is hiding a hue difference.";
            }
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, SnowLayerTest,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             SnowPathName);
} // namespace OloEngine::Tests
