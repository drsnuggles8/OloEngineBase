// OLO_TEST_LAYER: integration
// =============================================================================
// DeferredPointLightParityEvidenceTest.cpp — issue #1457, the frame-level half.
//
// Forward walks the light array; Forward+ and Deferred take point lights from
// the clustered tiles. #1457 measured Deferred shading DDGITest's red point
// light differently from Forward in the live editor. These tests render the
// real pipeline on all three paths and diff the frames:
//
//   * one unshadowed point light, three camera angles, 1024x683;
//   * a shadowed multi-light scene at 1920x1080 (a second tile count);
//   * Deferred with a non-native upscale, and with G-Buffer MSAA;
//   * DDGITest.olo itself, the issue's scene, red light only;
//   * the ATTRIBUTION: with GTAO on, Forward darkens the point light and
//     Deferred does not — the declared approximation #1452, which is what the
//     live measurement in #1457 was seeing. Nothing else differs.
//
// The camera is a scene CameraComponent, not the editor camera, so the editor
// grid — which Forward composites and Deferred writes through the G-Buffer — is
// not in the frame. Its near/far are DDGITest's (0.01 / 1000), the widest
// depth range the Deferred position reconstruction sees in a shipped scene.
//
// Every capture writes DeferredPointLightParity_GL_<Path>_<Case>.png and a
// Forward-vs-path |delta| x 8 heatmap under OloEditor/assets/tests/visual/.
// GL only: the fixtures need a GL 4.6 context and skip without one; the Vulkan
// cells are live-verified.
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
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kFramesPerCapture = 6;

        // Any channel of any pixel further apart than this is a difference.
        // The headless paths agree to 1 level where they agree at all.
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

        struct FrameDiff
        {
            u64 Compared = 0;
            u64 OverTolerance = 0;
            u64 BBrighter = 0; // pixels where B is brighter than A by more than the tolerance
            u32 MaxAbs = 0;
            f64 MeanA = 0.0;
            f64 MeanB = 0.0;
        };

        [[nodiscard]] u32 ChannelDelta(const Frame& a, const Frame& b, sizet i, int c)
        {
            return static_cast<u32>(std::abs(static_cast<int>(a.Rgba[i + c]) - static_cast<int>(b.Rgba[i + c])));
        }

        // A pixel whose 3x3 neighbourhood is flat in a frame is an interior: no
        // silhouette, where MSAA legitimately changes the answer.
        [[nodiscard]] bool IsInterior(const Frame& f, u32 x, u32 y)
        {
            for (int c = 0; c < 3; ++c)
            {
                int lo = 255;
                int hi = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const sizet n = ((static_cast<sizet>(y) + dy) * f.Width + (static_cast<sizet>(x) + dx)) * 4u + c;
                        lo = std::min(lo, static_cast<int>(f.Rgba[n]));
                        hi = std::max(hi, static_cast<int>(f.Rgba[n]));
                    }
                }
                if (hi - lo > 6)
                    return false;
            }
            return true;
        }

        [[nodiscard]] FrameDiff Diff(const Frame& a, const Frame& b, bool interiorOnly = false)
        {
            FrameDiff d;
            if (a.Rgba.size() != b.Rgba.size() || a.Width != b.Width)
            {
                ADD_FAILURE() << "frames of different size";
                return d;
            }
            for (u32 y = 1; y + 1 < a.Height; ++y)
            {
                for (u32 x = 1; x + 1 < a.Width; ++x)
                {
                    if (interiorOnly && (!IsInterior(a, x, y) || !IsInterior(b, x, y)))
                        continue;
                    const sizet i = (static_cast<sizet>(y) * a.Width + x) * 4u;
                    u32 worst = 0;
                    int signedSum = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        worst = std::max(worst, ChannelDelta(a, b, i, c));
                        signedSum += static_cast<int>(b.Rgba[i + c]) - static_cast<int>(a.Rgba[i + c]);
                        d.MeanA += a.Rgba[i + c];
                        d.MeanB += b.Rgba[i + c];
                    }
                    d.MaxAbs = std::max(d.MaxAbs, worst);
                    d.OverTolerance += (worst > kTolerance) ? 1u : 0u;
                    d.BBrighter += (worst > kTolerance && signedSum > 0) ? 1u : 0u;
                    ++d.Compared;
                }
            }
            if (d.Compared > 0)
            {
                d.MeanA /= static_cast<f64>(d.Compared * 3u);
                d.MeanB /= static_cast<f64>(d.Compared * 3u);
            }
            return d;
        }

        void WritePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.generic_string();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                       static_cast<int>(width * 4u)),
                      0)
                << "stbi_write_png failed for " << path;
        }

        // |a - b| x 8 per channel, so a 2-level difference reads as 16.
        void WriteHeatmap(const std::string& name, const Frame& a, const Frame& b)
        {
            std::vector<u8> out(a.Rgba.size(), 255u);
            for (sizet i = 0; i + 3 < a.Rgba.size() && i + 3 < b.Rgba.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                    out[i + c] = static_cast<u8>(std::min(static_cast<int>(ChannelDelta(a, b, i, c)) * 8, 255));
            }
            WritePng(name, out, a.Width, a.Height);
        }

        void Report(const char* what, const FrameDiff& d)
        {
            std::printf("[#1457] %-44s compared %8llu | over %u: %7llu (B brighter %7llu) | max %3u | mean %.2f -> %.2f\n",
                        what, static_cast<unsigned long long>(d.Compared), kTolerance,
                        static_cast<unsigned long long>(d.OverTolerance), static_cast<unsigned long long>(d.BBrighter),
                        d.MaxAbs, d.MeanA, d.MeanB);
        }

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            glm::vec3 Rotation; // pitch, yaw, roll (radians)
        };

        constexpr Pose kFront{ "Front", { 0.0f, 2.6f, 6.0f }, { -0.30f, 0.0f, 0.0f } };
        constexpr Pose kLeft{ "Left", { -4.5f, 2.0f, 4.0f }, { -0.20f, -0.75f, 0.0f } };
        constexpr Pose kLow{ "Low", { 1.5f, 0.6f, 3.5f }, { 0.05f, 0.25f, 0.0f } };
    } // namespace

    class DeferredPointLightParityTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kWidth = 1024;
        static constexpr u32 kHeight = 683;

        Entity AddMesh(const char* name, MeshPrimitive primitive, const glm::vec3& position, const glm::vec3& scale,
                       const glm::vec3& albedo, f32 roughness)
        {
            Scene& scene = GetScene();
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = primitive;
            Ref<Mesh> mesh = (primitive == MeshPrimitive::Sphere)  ? MeshPrimitives::CreateSphere()
                             : (primitive == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                   : MeshPrimitives::CreateCube();
            if (mesh)
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(roughness);
            return e;
        }

        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            AddMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 20.0f, 1.0f, 20.0f }, glm::vec3(0.75f), 0.7f);
            AddMesh("Wall", MeshPrimitive::Cube, { 0.0f, 2.5f, -3.0f }, { 12.0f, 5.0f, 0.4f }, glm::vec3(0.75f), 0.5f);
            AddMesh("Box", MeshPrimitive::Cube, { 1.6f, 0.6f, -0.8f }, { 1.2f, 1.2f, 1.2f }, { 0.6f, 0.7f, 0.8f }, 0.35f);
            AddMesh("Ball", MeshPrimitive::Sphere, { -2.2f, 0.7f, 0.2f }, glm::vec3(1.4f), { 0.8f, 0.8f, 0.8f }, 0.25f);

            m_Camera = scene.CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            cam.Camera.SetPerspectiveNearClip(0.01f);
            cam.Camera.SetPerspectiveFarClip(1000.0f);
            cam.Camera.SetViewportSize(kWidth, kHeight);

            m_Red = scene.CreateEntity("RedPoint");
            m_Red.GetComponent<TransformComponent>().Translation = { -0.8f, 1.4f, -1.2f };
            auto& pl = m_Red.AddComponent<PointLightComponent>();
            pl.m_Color = { 1.0f, 0.25f, 0.2f };
            pl.m_Intensity = 12.0f;
            pl.m_Range = 9.0f;
            pl.m_Attenuation = 1.0f; // DDGITest's red light
            pl.m_CastShadows = false;
        }

        // Four more local lights, every one of them shadowed, plus the red one
        // shadowed too. Five lights stay under the Forward auto-switch threshold
        // (8), so Forward really walks the light array.
        void AddShadowedLights()
        {
            Scene& scene = GetScene();
            m_Red.GetComponent<PointLightComponent>().m_CastShadows = true;
            const struct
            {
                const char* Name;
                glm::vec3 Position;
                glm::vec3 Color;
                f32 Intensity;
                f32 Range;
            } points[] = {
                { "BluePoint", { 3.0f, 2.0f, 0.5f }, { 0.2f, 0.4f, 1.0f }, 10.0f, 7.0f },
                { "WarmPoint", { 0.5f, 3.5f, 2.0f }, { 1.0f, 0.8f, 0.5f }, 6.0f, 12.0f },
                { "GreenPoint", { -3.5f, 0.8f, 1.5f }, { 0.3f, 1.0f, 0.3f }, 5.0f, 5.0f },
            };
            for (const auto& p : points)
            {
                Entity e = scene.CreateEntity(p.Name);
                e.GetComponent<TransformComponent>().Translation = p.Position;
                auto& pl = e.AddComponent<PointLightComponent>();
                pl.m_Color = p.Color;
                pl.m_Intensity = p.Intensity;
                pl.m_Range = p.Range;
                pl.m_CastShadows = true;
            }
            Entity spot = scene.CreateEntity("Spot");
            spot.GetComponent<TransformComponent>().Translation = { 2.0f, 4.0f, 3.0f };
            auto& sl = spot.AddComponent<SpotLightComponent>();
            sl.m_Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f));
            sl.m_Color = { 1.0f, 1.0f, 0.9f };
            sl.m_Intensity = 20.0f;
            sl.m_Range = 12.0f;
            sl.m_CastShadows = true;
        }

        void UsePath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
        }

        void SetPose(const Pose& pose)
        {
            auto& tc = m_Camera.GetComponent<TransformComponent>();
            tc.Translation = pose.Position;
            tc.SetRotationEuler(pose.Rotation);
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
            WritePng(name, f.Rgba, f.Width, f.Height);
            return f;
        }

        struct PathFrames
        {
            Frame Forward;
            Frame ForwardPlus;
            Frame Deferred;
        };

        PathFrames CaptureAllPaths(const std::string& tag)
        {
            PathFrames frames;
            UsePath(RenderingPath::Forward);
            frames.Forward = Capture("DeferredPointLightParity_GL_Forward_" + tag);
            UsePath(RenderingPath::ForwardPlus);
            frames.ForwardPlus = Capture("DeferredPointLightParity_GL_ForwardPlus_" + tag);
            UsePath(RenderingPath::Deferred);
            frames.Deferred = Capture("DeferredPointLightParity_GL_Deferred_" + tag);
            WriteHeatmap("DeferredPointLightParity_DiffForwardPlus_" + tag, frames.Forward, frames.ForwardPlus);
            WriteHeatmap("DeferredPointLightParity_DiffDeferred_" + tag, frames.Forward, frames.Deferred);
            return frames;
        }

        // Forward+ within kTolerance of Forward everywhere; Deferred everywhere
        // but a sliver — and a lit frame, so the agreement is not two black
        // images agreeing.
        //
        // THE DEFERRED SLIVER. Forward and Forward+ shade the rasterizer's
        // interpolated position; Deferred shades the one it RECONSTRUCTS from
        // depth, which is sub-millimetre off. That is invisible in a smooth
        // gradient and flips a pixel only where the answer is discontinuous:
        // a silhouette (measured: 4 pixels of 696k under an upscale, all with a
        // 3x3 range of 47-96 levels) and a shadow boundary, where one PCF tap
        // changes side (145 of 2.07M at 1080p, in short runs along the
        // sphere's contact shadow). The defects this test exists for were
        // 0.3-46 % of the frame, so 0.02 % separates the two by three orders
        // of magnitude.
        void ExpectPathsAgree(const std::string& tag, const PathFrames& frames, bool interiorOnly = false)
        {
            const FrameDiff plus = Diff(frames.Forward, frames.ForwardPlus, interiorOnly);
            const FrameDiff deferred = Diff(frames.Forward, frames.Deferred, interiorOnly);
            Report((tag + " Forward vs Forward+").c_str(), plus);
            Report((tag + " Forward vs Deferred").c_str(), deferred);

            EXPECT_GT(plus.MeanA, 10.0) << tag << ": the Forward frame is (near-)black";
            EXPECT_GT(deferred.Compared, 100000u) << tag << ": too few pixels compared";
            EXPECT_EQ(plus.OverTolerance, 0u) << tag << ": Forward+ differs from Forward on " << plus.OverTolerance
                                              << " pixels (max " << plus.MaxAbs << ")";
            EXPECT_LE(deferred.OverTolerance, deferred.Compared / 5000u)
                << tag << ": Deferred differs from Forward on " << deferred.OverTolerance << " of " << deferred.Compared
                << " pixels (max " << deferred.MaxAbs << ", " << deferred.BBrighter
                << " of them brighter on Deferred) — more than position reconstruction explains; see "
                << "DeferredPointLightParity_DiffDeferred_" << tag << ".png (issue #1457)";
        }

        Entity m_Camera;
        Entity m_Red;
    };

    TEST_F(DeferredPointLightParityTest, OneUnshadowedPointLightMatchesForwardFromThreeAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        for (const Pose& pose : { kFront, kLeft, kLow })
        {
            SetPose(pose);
            const PathFrames frames = CaptureAllPaths(std::string("Single_") + pose.Name);
            ASSERT_FALSE(HasFatalFailure());
            ExpectPathsAgree(std::string("Single_") + pose.Name, frames);
        }
    }

    TEST_F(DeferredPointLightParityTest, ShadowedMultiLightSceneMatchesForwardAt1080p)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        AddShadowedLights();
        ResizeRenderTarget(1920, 1080);
        m_Camera.GetComponent<CameraComponent>().Camera.SetViewportSize(1920, 1080);

        for (const Pose& pose : { kFront, kLeft })
        {
            SetPose(pose);
            const PathFrames frames = CaptureAllPaths(std::string("MultiShadowed1080p_") + pose.Name);
            ASSERT_FALSE(HasFatalFailure());
            ExpectPathsAgree(std::string("MultiShadowed1080p_") + pose.Name, frames);
        }
    }

    // The tiles are built at RENDER resolution; an upscale makes that differ
    // from the display resolution the composite is read at.
    TEST_F(DeferredPointLightParityTest, UpscaledRenderMatchesForward)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Quality;
        SetPose(kFront);
        const PathFrames frames = CaptureAllPaths("UpscaleQuality_Front");
        ASSERT_FALSE(HasFatalFailure());
        ExpectPathsAgree("UpscaleQuality_Front", frames);
    }

    // MSAA exists on the Deferred path only (the G-Buffer's sample count;
    // DeferredLighting_MSAA shades per sample through the same tile evaluator).
    // Silhouettes legitimately differ from the aliased Forward frame, so this
    // compares the flat interiors, where the light's shading lives.
    TEST_F(DeferredPointLightParityTest, DeferredMSAAMatchesForwardOnInteriors)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 4;
        Renderer3D::GetRendererSettings().Deferred.PerSampleLighting = true;
        SetPose(kFront);
        const PathFrames frames = CaptureAllPaths("DeferredMSAA4_Front");
        ASSERT_FALSE(HasFatalFailure());
        ExpectPathsAgree("DeferredMSAA4_Front", frames, /*interiorOnly=*/true);
    }

    // THE ATTRIBUTION of what #1457 measured live. The editor runs GTAO; Forward
    // multiplies the COMPOSED colour by it (PostProcess_SSAOApply), Deferred
    // multiplies the ambient term alone (lighting-signal-contract.md, #1452).
    // So with the point light off the two agree, and with it on Forward is
    // darker wherever AO < 1 — never brighter. When #1452 lands, the second
    // half of this test must flip to "agree" and this comment goes.
    TEST_F(DeferredPointLightParityTest, ScreenSpaceAOIsTheWholeRemainingGap)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        SetPose(kFront);

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.ActiveAOTechnique = AOTechnique::GTAO;
        pp.GTAOEnabled = true;
        Renderer3D::ApplyRendererSettings();

        auto& red = m_Red.GetComponent<PointLightComponent>();
        const f32 intensity = red.m_Intensity;
        red.m_Intensity = 0.0f;
        const PathFrames dark = CaptureAllPaths("GTAO_LightOff_Front");
        ASSERT_FALSE(HasFatalFailure());
        red.m_Intensity = intensity;
        const PathFrames lit = CaptureAllPaths("GTAO_LightOn_Front");
        ASSERT_FALSE(HasFatalFailure());

        const FrameDiff darkDiff = Diff(dark.Forward, dark.Deferred);
        const FrameDiff litDiff = Diff(lit.Forward, lit.Deferred);
        const FrameDiff aoRan = Diff(dark.Forward, lit.Forward);
        Report("GTAO, light off: Forward vs Deferred", darkDiff);
        Report("GTAO, light on:  Forward vs Deferred", litDiff);

        EXPECT_GT(aoRan.OverTolerance, 10000u) << "the point light does not reach the frame";
        EXPECT_EQ(darkDiff.OverTolerance, 0u)
            << "with no direct light the two paths differ — the gap is not only the AO applied to direct light";
        EXPECT_GT(litDiff.OverTolerance, 1000u)
            << "Forward no longer darkens the point light by the AO. If #1452 has landed, this is the fix: make this "
               "case assert agreement.";
        EXPECT_EQ(litDiff.OverTolerance, litDiff.BBrighter)
            << "Deferred is DARKER than Forward on " << (litDiff.OverTolerance - litDiff.BBrighter)
            << " pixels; AO applied to the whole colour on Forward can only make Forward the darker one";
    }

    // The issue's own scene: DDGITest.olo, red point light only, unshadowed,
    // probe volume at 0, through the scene's runtime camera.
    class DeferredPointLightParitySceneTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kWidth = 1024;
        static constexpr u32 kHeight = 683;

        void BuildScene() override
        {
            const fs::path scenePath = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Scenes/DDGITest.olo";
            SceneSerializer serializer(GetSceneRef());
            ASSERT_TRUE(serializer.Deserialize(scenePath)) << scenePath.string();
            EnableRendering(kWidth, kHeight);

            Scene& scene = GetScene();
            for (auto e : scene.GetAllEntitiesWith<DirectionalLightComponent>())
                Entity{ e, &scene }.GetComponent<DirectionalLightComponent>().m_Intensity = 0.0f;
            for (auto e : scene.GetAllEntitiesWith<PointLightComponent>())
                Entity{ e, &scene }.GetComponent<PointLightComponent>().m_CastShadows = false;
            for (auto e : scene.GetAllEntitiesWith<LightProbeVolumeComponent>())
            {
                auto& volume = Entity{ e, &scene }.GetComponent<LightProbeVolumeComponent>();
                volume.m_Intensity = 0.0f;
                volume.m_Dirty = true;
            }
        }

        Frame Capture(RenderingPath path, const std::string& name)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
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
            WritePng(name, f.Rgba, f.Width, f.Height);
            return f;
        }
    };

    TEST_F(DeferredPointLightParitySceneTest, DDGITestRedPointLightMatchesForward)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);

        const Frame forward = Capture(RenderingPath::Forward, "DeferredPointLightParity_GL_Forward_DDGITest");
        const Frame forwardPlus = Capture(RenderingPath::ForwardPlus, "DeferredPointLightParity_GL_ForwardPlus_DDGITest");
        const Frame deferred = Capture(RenderingPath::Deferred, "DeferredPointLightParity_GL_Deferred_DDGITest");
        ASSERT_FALSE(HasFatalFailure());
        WriteHeatmap("DeferredPointLightParity_DiffDeferred_DDGITest", forward, deferred);

        const FrameDiff plus = Diff(forward, forwardPlus);
        const FrameDiff def = Diff(forward, deferred);
        Report("DDGITest Forward vs Forward+", plus);
        Report("DDGITest Forward vs Deferred", def);
        EXPECT_GT(def.MeanA, 10.0) << "the DDGITest frame is (near-)black";
        EXPECT_EQ(plus.OverTolerance, 0u) << "Forward+ differs from Forward on DDGITest's red light";
        EXPECT_EQ(def.OverTolerance, 0u) << "Deferred differs from Forward on DDGITest's red light (issue #1457)";
    }
} // namespace OloEngine::Tests
