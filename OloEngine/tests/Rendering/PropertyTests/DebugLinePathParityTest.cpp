// OLO_TEST_LAYER: integration
// =============================================================================
// DebugLinePathParityTest.cpp — Renderer3D::DrawLine reaches the screen on
// every render path.
//
// A debug line (light gizmos, the world axis helper, skeletons, groom previews)
// is a see-through mesh: depth test off, and a colour mask of attachment 0 only,
// written for the scene framebuffer's layout (1 = entity ID, 2 = view normal).
// On the Deferred path DrawMesh used to send it into the G-Buffer, where that
// mask kept it out of the emissive lane and, with no depth written, the
// lighting pass shaded it as background wherever sky was behind it. Every debug
// line was invisible on Deferred. It now goes to ForwardOverlayPass there.
//
// The scene has NO geometry behind the lines, the case that failed outright:
// each path renders the world axis helper on and off, and the lines must change
// about as many pixels on Forward+ and Deferred as they do on Forward.
//
// Evidence PNGs: DebugLines[Off]_GL_<Path>.png under OloEditor/assets/tests/visual/.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
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

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        constexpr u32 kFramesPerCapture = 4;

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

        // Pixels whose colour differs by more than a rounding step.
        [[nodiscard]] u64 ChangedPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            u64 changed = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) > 2)
                    {
                        ++changed;
                        break;
                    }
                }
            }
            return changed;
        }
    } // namespace

    class DebugLinePathParityTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            // A light, so no path takes an empty-scene shortcut; its gizmo is
            // switched off below so the axis helper is the only debug draw.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.3f, -0.6f, -0.7f));
            dl.m_Intensity = 2.0f;
        }

        void UsePath(RenderingPath path, bool axes)
        {
            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = path;
            rs.ShowGrid = false;
            rs.ShowLightGizmos = false;
            rs.EditorDebugDrawsEnabled = true;
            rs.ShowWorldAxisHelper = axes;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            Renderer3D::ApplyRendererSettings();
        }

        void Capture(const std::string& name, std::vector<u8>& out)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Looking at the origin from above and to the side: all three axes
            // (3 m long) sit in the frame, against nothing but the clear colour.
            camera.SetPose({ 0.9f, 0.9f, 2.2f }, -0.3f, 0.3f);
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

        [[nodiscard]] u64 LinePixels(RenderingPath path)
        {
            const std::string name = PathName(path);
            std::vector<u8> off;
            std::vector<u8> on;
            UsePath(path, false);
            Capture("DebugLinesOff_GL_" + name, off);
            UsePath(path, true);
            Capture("DebugLines_GL_" + name, on);
            return ChangedPixels(off, on);
        }
    };

    TEST_F(DebugLinePathParityTest, DebugLinesDrawTheSameOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const u64 forward = LinePixels(RenderingPath::Forward);
        ASSERT_FALSE(HasFatalFailure());
        // Positive control: the reference path draws them, or the comparison
        // below means nothing.
        ASSERT_GT(forward, 300u) << "the world axis helper changed only " << forward
                                 << " pixels on Forward — see DebugLines_GL_Forward.png";

        for (const RenderingPath path : { RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            const u64 pixels = LinePixels(path);
            ASSERT_FALSE(HasFatalFailure());
            const f64 ratio = static_cast<f64>(pixels) / static_cast<f64>(forward);
            EXPECT_GT(ratio, 0.8) << "debug lines changed " << pixels << " pixels on " << PathName(path) << " against "
                                  << forward << " on Forward: they are (partly) invisible there. Compare "
                                  << "DebugLines_GL_" << PathName(path) << ".png with DebugLines_GL_Forward.png.";
            EXPECT_LT(ratio, 1.25) << "debug lines changed " << pixels << " pixels on " << PathName(path)
                                   << " against " << forward << " on Forward";
        }
    }
} // namespace OloEngine::Tests
