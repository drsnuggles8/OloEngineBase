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
// line was invisible on Deferred. Since #1533 every path draws it in
// DebugOverlayPass, after the last pass that writes the scene's colour.
//
// DebugLinesDrawTheSameOnEveryPath: the scene has NO geometry behind the lines,
// the case that failed outright. Each path renders the world axis helper on and
// off, and the lines must change about as many pixels on Forward+ and Deferred
// as they do on Forward.
//
// DebugLinesDrawOverWhatLaterPassesPaint: an opaque water surface over the X
// axis. Drawn in ScenePass, the lines came before the foliage, the fur and the
// water, which covered them; they must now show through it on every path.
//
// Evidence PNGs: DebugLines[Off|Dry|UnderWater]_GL_<Path>.png and
// DebugLinesWaterControl[Dry]_GL_<Path>.png under OloEditor/assets/tests/visual/.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
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

        // Pixels of the X axis's red (1.0, 0.2, 0.2) -- and of the control cube's
        // -- which neither the water nor the sky produces.
        [[nodiscard]] u64 RedPixels(const std::vector<u8>& rgba)
        {
            u64 red = 0;
            for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                const int r = rgba[i];
                const int g = rgba[i + 1];
                const int b = rgba[i + 2];
                if (r > 120 && r > g + 60 && r > b + 60)
                {
                    ++red;
                }
            }
            return red;
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

        // Looking at the origin from above and to the side: all three axes (3 m
        // long) sit in the frame, against nothing but the clear colour.
        static inline const glm::vec3 kEye{ 0.9f, 0.9f, 2.2f };
        static constexpr f32 kYaw = -0.3f;
        static constexpr f32 kPitch = 0.3f;

        void Capture(const std::string& name, std::vector<u8>& out, const glm::vec3& eye = kEye, f32 yaw = kYaw,
                     f32 pitch = kPitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
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

    // A see-through debug draw comes after every pass that writes the scene's
    // colour (#1533). In ScenePass (ForwardOverlayPass on Deferred) it came
    // before the foliage, the fur and the water, which covered it: a gizmo
    // inside the dog's coat was hidden by the coat. Here an opaque water surface
    // lies half a metre over the X axis.
    //
    // THE CONTROL IS A SCENE DRAW UNDER THE SAME WATER: a red cube, which the
    // water must hide. That is what shows this water covers what lies under it
    // on every path -- the condition under which a debug line drawn before it
    // would vanish -- so the axis keeping its red is the overlay's doing.
    TEST_F(DebugLinePathParityTest, DebugLinesDrawOverWhatLaterPassesPaint)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene& scene = GetScene();

        const auto addCube = [&scene]
        {
            Entity cube = scene.CreateEntity("RedCube");
            auto& tc = cube.GetComponent<TransformComponent>();
            tc.Translation = { 0.6f, -0.25f, 0.6f };
            tc.Scale = glm::vec3(0.4f);
            auto& mc = cube.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = cube.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 0.08f, 0.08f, 1.0f));
            mat.m_Material.SetRoughnessFactor(1.0f);
            mat.m_Material.SetMetallicFactor(0.0f);
            return cube;
        };
        const auto addWater = [&scene]
        {
            Entity water = scene.CreateEntity("Water");
            // The grid is centred on its entity (+-size/2): 40 m square over the
            // origin, half a metre above the X axis and the cube.
            water.GetComponent<TransformComponent>().Translation = { 0.0f, 0.5f, 0.0f };
            auto& wc = water.AddComponent<WaterComponent>();
            wc.m_WorldSizeX = 40.0f;
            wc.m_WorldSizeZ = 40.0f;
            wc.m_GridResolutionX = 32;
            wc.m_GridResolutionZ = 32;
            wc.m_WaveAmplitude = 0.0f;
            // Opaque: nothing under the surface reaches the screen through it.
            wc.m_Transparency = 0.0f;
            wc.m_RefractionEnabled = false;
            return water;
        };

        // FROM 3.5 M ABOVE THE WATER. The underwater fog switches on for an eye
        // within a wave's reach of the surface (at least 2 m, Scene's
        // kMinWaveReach) and then fogs every pixel whose depth lies behind the
        // surface -- which a see-through line, writing no depth, always is. From
        // the fixture's usual eye, 0.4 m above this water, that fog erased the
        // axis in the tone map while SceneColor held it, drawn after the water.
        const glm::vec3 eye{ 0.9f, 4.0f, 2.2f };
        constexpr f32 kWaterYaw = -0.35f;
        constexpr f32 kWaterPitch = 1.03f;
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(PathName(path));
            const std::string name = PathName(path);
            std::vector<u8> frame;

            // The cube alone, dry and then under the water.
            UsePath(path, false);
            Entity cube = addCube();
            Capture("DebugLinesWaterControlDry_GL_" + name, frame, eye, kWaterYaw, kWaterPitch);
            const u64 cubeDry = RedPixels(frame);
            Entity water = addWater();
            Capture("DebugLinesWaterControl_GL_" + name, frame, eye, kWaterYaw, kWaterPitch);
            const u64 cubeWet = RedPixels(frame);
            scene.DestroyEntity(water);
            scene.DestroyEntity(cube);
            ASSERT_FALSE(HasFatalFailure());
            ASSERT_GT(cubeDry, 300u) << "the control cube is not on screen; see DebugLinesWaterControlDry_GL_" << name
                                     << ".png";
            ASSERT_LT(cubeWet, cubeDry / 10u) << "the water did not hide the cube under it (" << cubeWet << " red pixels of "
                                              << cubeDry << "), so it cannot show what it would do to a debug line";

            // The axis alone, dry and then under the same water.
            UsePath(path, true);
            Capture("DebugLinesDry_GL_" + name, frame, eye, kWaterYaw, kWaterPitch);
            const u64 axisDry = RedPixels(frame);
            water = addWater();
            Capture("DebugLinesUnderWater_GL_" + name, frame, eye, kWaterYaw, kWaterPitch);
            const u64 axisWet = RedPixels(frame);
            scene.DestroyEntity(water);
            ASSERT_FALSE(HasFatalFailure());
            ASSERT_GT(axisDry, 100u) << "the X axis is not on screen; see DebugLinesDry_GL_" << name << ".png";
            EXPECT_GT(static_cast<f64>(axisWet), 0.8 * static_cast<f64>(axisDry))
                << "the X axis kept " << axisWet << " of its " << axisDry << " red pixels under the water: a pass drawn "
                << "after the debug draws covered them. See DebugLinesUnderWater_GL_" << name << ".png";
        }
    }
} // namespace OloEngine::Tests
