// OLO_TEST_LAYER: L8
// =============================================================================
// FogVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent, golden-free contract for the
// distance-fog post-process pass (PostProcess_Fog.glsl / PostProcess_FogUpsample
// .glsl / FogRenderPass), in its analytical (non-volumetric) mode.
//
// A warm-coloured floor recedes from just in front of the camera to a distant
// back wall, lit by a sun. With fog ON the DISTANT geometry must tend toward the
// (blue) fog colour while the NEAR geometry stays unfogged; with fog OFF the
// pass is skipped entirely and the frame is the plain lit scene. The scene is
// rendered twice through the FULL Renderer3D pipeline from the same pose — once
// with fog OFF and once with fog ON — and both frames are written to
//   OloEditor/assets/tests/visual/Fog_<state>.png
//
// The contract is GOLDEN-FREE and differential (robust across GPUs, no
// committed reference image):
//   1. Both frames render non-black.
//   2. NEAR band: fog ON ≈ fog OFF and stays warm (R-dominant). The near
//      geometry is NOT repainted with fog — this is the pixel-level guard for
//      the "fog floods the whole frame" failure mode CLAUDE.md warns about.
//   3. FAR band: fog ON shifts measurably toward the blue fog colour vs fog
//      OFF, and reads blue-dominant there (distant geometry tends to fog
//      colour, not a grey wash).
//   4. Gradient: the far band (fog ON) is bluer than the near band (fog ON) —
//      more fog with distance.
//
// The cheap fog *math* contracts live in FogMathTest.cpp (distance/height
// factor, monotonicity, MaxOpacity clamp, composite blend) and
// ShaderUnitTests.cpp (GPU endpoint invariants). Per the CLAUDE.md rendering
// rule, those prove the formula; this test proves the rendered frame looks right.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching WaterVisualEvidenceTest / SSRVisualEvidenceTest.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
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

    class FogVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Whole-scene world-space offset. Derived fixtures set this in their
        // constructor (before SetUp -> BuildScene runs) to place the identical
        // scene far from the origin — which is where a wrong/OOB u_CameraPosition
        // stops being benign. Defaults to the origin for the normal tests.
        glm::vec3 m_WorldOffset{ 0.0f };

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Sun so the warm floor/wall is lit.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3{ 0.0f, 30.0f, 0.0f } + m_WorldOffset;
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale, const glm::vec3& albedo)
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
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            };

            // Warm floor receding into the distance (near camera → far wall).
            // Warm albedo (orange-ish) so the shift toward the BLUE fog colour
            // in the distance is unambiguous (R-dominant near, B-dominant far).
            addMesh("Floor", MeshPrimitive::Plane, glm::vec3{ 0.0f, -1.0f, -100.0f } + m_WorldOffset,
                    { 400.0f, 1.0f, 400.0f }, { 0.75f, 0.6f, 0.45f });
            // Distant back wall so the far band always has solid geometry to fog
            // (≈170 units away → fully fogged at the chosen density).
            addMesh("BackWall", MeshPrimitive::Cube, glm::vec3{ 0.0f, 25.0f, -160.0f } + m_WorldOffset,
                    { 400.0f, 80.0f, 4.0f }, { 0.75f, 0.6f, 0.45f });
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
            ASSERT_TRUE(fb) << "No composited framebuffer for fog capture '" << tag << "'";

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

            const std::string path = (dir / ("Fog_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
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

    // Fog off vs on: distant geometry tends toward the fog colour, near geometry
    // stays unfogged. SKIPs without a GL 4.6 context (see file header).
    TEST_F(FogVisualEvidenceTest, DistantGeometryFogsNearStaysClear)
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

        // The fixture restores RendererSettings + PostProcessSettings, but NOT
        // the scene-level FogSettings (s_Data.Fog) — snapshot + RAII-restore it
        // here so enabling fog cannot leak into later GPU tests in this process.
        struct ScopedFogSettings
        {
            FogSettings Saved;
            ScopedFogSettings() : Saved(Renderer3D::GetFogSettings()) {}
            ~ScopedFogSettings()
            {
                Renderer3D::GetFogSettings() = Saved;
            }
        } scopedFog;

        // Looking down the corridor: camera up + forward, pitched down so the
        // floor fills the lower frame and the distant wall sits high in it.
        const glm::vec3 pos{ 0.0f, 5.0f, 10.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f; // positive tilts the view DOWN (see WaterVisualEvidenceTest)

        // --- Fog OFF baseline (the pass is skipped entirely) ---
        Renderer3D::GetFogSettings().Enabled = false;
        std::vector<u8> offPixels;
        Capture("Off", pos, yaw, pitch, offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Fog ON: analytical (non-volumetric) blue distance fog ---
        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.Mode = FogMode::ExponentialSquared;
            fog.Color = glm::vec3(0.20f, 0.40f, 0.90f); // strong blue
            fog.Density = 0.02f;
            fog.HeightFalloff = 0.0f; // distance-only fog (predictable gradient)
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 1.0f;
            fog.EnableScattering = false;
            fog.EnableVolumetric = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
        }
        std::vector<u8> onPixels;
        Capture("On", pos, yaw, pitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // Bands: near floor at the bottom, distant wall/floor near the top.
        constexpr f32 nx0 = 0.30f, nx1 = 0.70f, ny0 = 0.80f, ny1 = 0.93f;
        constexpr f32 fx0 = 0.30f, fx1 = 0.70f, fy0 = 0.20f, fy1 = 0.34f;

        const BandStats nearOff = SampleBand(offPixels, nx0, nx1, ny0, ny1);
        const BandStats nearOn = SampleBand(onPixels, nx0, nx1, ny0, ny1);
        const BandStats farOff = SampleBand(offPixels, fx0, fx1, fy0, fy1);
        const BandStats farOn = SampleBand(onPixels, fx0, fx1, fy0, fy1);

        // (1) Both frames rendered non-black.
        EXPECT_GT(nearOff.R + nearOff.G + nearOff.B, 5.0) << "Fog-off frame rendered (near-)black";
        EXPECT_GT(nearOn.R + nearOn.G + nearOn.B, 5.0) << "Fog-on frame rendered (near-)black";

        // (2) NEAR band stays unfogged: fog ON ≈ fog OFF, and it stays WARM
        //     (red-dominant) — the near floor is not repainted with blue fog.
        //     This is the "fog floods the whole frame" guard at the pixel level.
        EXPECT_GT(nearOff.R, nearOff.B) << "Near floor is not warm in the fog-off baseline";
        EXPECT_GT(nearOn.R, nearOn.B)
            << "Fog flooded the NEAR floor blue (R=" << nearOn.R << " B=" << nearOn.B
            << ") — fog is repainting unfogged geometry. See Fog_On.png";
        EXPECT_NEAR(nearOn.R, nearOff.R, 30.0)
            << "Near floor changed too much when fog enabled (off.R=" << nearOff.R
            << " on.R=" << nearOn.R << ") — near geometry should stay unfogged. See Fog_On.png";

        // (3) FAR band tends toward the blue fog colour: fog ON is markedly
        //     bluer than fog OFF there, and reads blue-dominant.
        EXPECT_GT(farOn.B, farOff.B + 15.0)
            << "Distant geometry did not gain fog blue (off.B=" << farOff.B << " on.B=" << farOn.B
            << "). See Fog_Off.png / Fog_On.png";
        EXPECT_GT(farOn.B, farOn.R + 10.0)
            << "Distant fogged band is not blue-dominant (R=" << farOn.R << " G=" << farOn.G
            << " B=" << farOn.B << ") — fog colour did not take over the distance. See Fog_On.png";

        // (4) Gradient with distance: the far (fogged) band is bluer than the
        //     near (clear) band when fog is on.
        EXPECT_GT(farOn.B, nearOn.B + 10.0)
            << "No fog gradient with distance (near.B=" << nearOn.B << " far.B=" << farOn.B
            << "). See Fog_On.png";
    }

    // Deterministic regression guard for #446. In the full suite, cross-test GPU
    // buffer churn left UBO binding 17 (the Fog UBO) bound to 0 — deleting any
    // buffer GL currently has bound at a slot reverts that slot — so the late
    // post-process fog pass read an all-zero FogData (u_FogFlags.x == 0), took
    // its disabled early-out, and produced a frame byte-identical with fog OFF.
    // The pass only bound the UBO in its constructor; the fix re-binds the Fog /
    // FogVolumes UBOs on every per-frame upload. Here we reproduce the corrupt
    // binding state deterministically (no reliance on suite order): knock binding
    // 17 to 0, render with fog ON, and assert the distant band is STILL fogged.
    // Without the re-bind in RenderPipeline this fails exactly like the flake.
    TEST_F(FogVisualEvidenceTest, ReappliesFogAfterFogUboBindingKnockedOut)
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

        // Snapshot + RAII-restore the scene-level fog settings so enabling fog
        // here cannot leak into later GPU tests (mirrors the main test).
        struct ScopedFogSettings
        {
            FogSettings Saved;
            ScopedFogSettings() : Saved(Renderer3D::GetFogSettings()) {}
            ~ScopedFogSettings()
            {
                Renderer3D::GetFogSettings() = Saved;
            }
        } scopedFog;

        // Analytical (non-volumetric) blue distance fog — same params as the
        // main test's fog-ON capture.
        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.Mode = FogMode::ExponentialSquared;
            fog.Color = glm::vec3(0.20f, 0.40f, 0.90f);
            fog.Density = 0.02f;
            fog.HeightFalloff = 0.0f;
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 1.0f;
            fog.EnableScattering = false;
            fog.EnableVolumetric = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
        }

        const glm::vec3 pos{ 0.0f, 5.0f, 10.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f;

        // Reproduce the #446 hazard deterministically: leave the Fog UBO's slot
        // unbound right before the frame. The per-frame fog upload MUST rebind
        // it, or the fog pass renders an all-zero FogData and skips fog entirely.
        // RAII-restore the original binding 17 so this deliberate global-GL-state
        // corruption cannot leak into later GPU tests — including on a fatal
        // failure / early return below (GLStateGuard does not restore per-slot
        // UBO bindings).
        struct ScopedUboBinding
        {
            GLuint Slot;
            GLint Saved = 0;
            explicit ScopedUboBinding(GLuint slot) : Slot(slot)
            {
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, slot, &Saved);
            }
            ~ScopedUboBinding()
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, Slot, static_cast<GLuint>(Saved));
            }
        } scopedFogUboBinding(ShaderBindingLayout::UBO_FOG);

        glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_FOG, 0);

        std::vector<u8> onPixels;
        Capture("KnockoutOn", pos, yaw, pitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // Direct invariant: the frame's upload must have re-established binding 17.
        GLint boundFogUbo = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, ShaderBindingLayout::UBO_FOG, &boundFogUbo);
        EXPECT_NE(boundFogUbo, 0)
            << "Fog UBO (binding " << ShaderBindingLayout::UBO_FOG
            << ") left unbound after the frame — the per-frame re-bind regressed; "
               "the fog shader would read zeroed fog params and skip fog (#446).";

        // Pixel-level proof the re-bind worked: distant geometry still tends to
        // the blue fog colour (blue-dominant), with a gradient vs the near band.
        constexpr f32 nx0 = 0.30f, nx1 = 0.70f, ny0 = 0.80f, ny1 = 0.93f;
        constexpr f32 fx0 = 0.30f, fx1 = 0.70f, fy0 = 0.20f, fy1 = 0.34f;
        const BandStats nearOn = SampleBand(onPixels, nx0, nx1, ny0, ny1);
        const BandStats farOn = SampleBand(onPixels, fx0, fx1, fy0, fy1);

        EXPECT_GT(farOn.R + farOn.G + farOn.B, 5.0) << "Frame rendered (near-)black";
        EXPECT_GT(farOn.B, farOn.R + 10.0)
            << "Distant band not blue-dominant after binding-17 knockout (R=" << farOn.R
            << " G=" << farOn.G << " B=" << farOn.B
            << ") — fog UBO re-bind missing, fog not applied (#446). See Fog_KnockoutOn.png";
        EXPECT_GT(farOn.B, nearOn.B + 10.0)
            << "No fog gradient after binding-17 knockout (near.B=" << nearOn.B
            << " far.B=" << farOn.B << "). See Fog_KnockoutOn.png";
    }

    // Whole-scene world-space offset large enough that an out-of-bounds
    // u_CameraPosition stops being benign: |worldPos| from the origin (~1000)
    // dwarfs the true camera->fragment distances (~12 near, ~175 far).
    const glm::vec3 kFogWorldOffset{ 0.0f, 0.0f, 1000.0f };

    class FogVisualEvidenceOffOriginTest : public FogVisualEvidenceTest
    {
      public:
        FogVisualEvidenceOffOriginTest()
        {
            // Set before SetUp() -> BuildScene() runs so the scene is built at
            // the offset; the test poses the camera by the same offset.
            m_WorldOffset = kFogWorldOffset;
        }
    };

    // Regression guard for the binding-0 out-of-bounds camera-position read.
    // The fog shaders read the full CameraMatrices layout (u_CameraPosition at
    // std140 offset 192) from UBO binding 0, but an earlier stage can leave a
    // smaller 64-byte ViewProjection-only camera UBO bound there, making the read
    // OOB. Origin-centred scenes survive because robust-access OOB reads return
    // 0 ≈ the true camera; an off-origin scene does not. FogRenderPass now
    // re-binds the full camera UBO before drawing — without that, the near floor
    // (here ~1000 units from the origin but only ~12 from the true camera) gets
    // flooded with distance fog. Asserts the near floor stays warm/unfogged.
    TEST_F(FogVisualEvidenceOffOriginTest, NearStaysClearWhenSceneIsFarFromOrigin)
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

        struct ScopedFogSettings
        {
            FogSettings Saved;
            ScopedFogSettings() : Saved(Renderer3D::GetFogSettings()) {}
            ~ScopedFogSettings()
            {
                Renderer3D::GetFogSettings() = Saved;
            }
        } scopedFog;

        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.Mode = FogMode::ExponentialSquared;
            fog.Color = glm::vec3(0.20f, 0.40f, 0.90f);
            fog.Density = 0.02f;
            fog.HeightFalloff = 0.0f;
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 1.0f;
            fog.EnableScattering = false;
            fog.EnableVolumetric = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
        }

        // Same view as the origin test, shifted by the scene's world offset.
        const glm::vec3 pos = glm::vec3{ 0.0f, 5.0f, 10.0f } + kFogWorldOffset;
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f;

        std::vector<u8> onPixels;
        Capture("OffOriginOn", pos, yaw, pitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        constexpr f32 nx0 = 0.30f, nx1 = 0.70f, ny0 = 0.80f, ny1 = 0.93f;
        constexpr f32 fx0 = 0.30f, fx1 = 0.70f, fy0 = 0.20f, fy1 = 0.34f;
        const BandStats nearOn = SampleBand(onPixels, nx0, nx1, ny0, ny1);
        const BandStats farOn = SampleBand(onPixels, fx0, fx1, fy0, fy1);

        EXPECT_GT(nearOn.R + nearOn.G + nearOn.B, 5.0) << "Frame rendered (near-)black";

        // The proof: the near floor stays WARM (red-dominant). With an OOB
        // u_CameraPosition (== 0) the near floor reads ~1000 units from the
        // "camera" and floods blue — so this fails without the binding-0 re-bind.
        EXPECT_GT(nearOn.R, nearOn.B)
            << "Near floor went blue in an off-origin scene (R=" << nearOn.R << " B=" << nearOn.B
            << ") — u_CameraPosition read out-of-bounds at binding 0. See Fog_OffOriginOn.png";

        // Fog still applies correctly with distance: far band is blue-dominant
        // and bluer than the (clear, warm) near band.
        EXPECT_GT(farOn.B, farOn.R)
            << "Distant band not blue-dominant off-origin (R=" << farOn.R << " B=" << farOn.B
            << "). See Fog_OffOriginOn.png";
        EXPECT_GT(farOn.B, nearOn.B + 10.0)
            << "No fog gradient with distance off-origin (near.B=" << nearOn.B
            << " far.B=" << farOn.B << "). See Fog_OffOriginOn.png";
    }
} // namespace OloEngine::Tests
