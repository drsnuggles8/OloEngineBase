// =============================================================================
// SSAOVisualEvidenceTest.cpp
//
// OLO_TEST_LAYER: L8
//
// Visual evidence (PNG) + a driver-independent contract for the Screen-Space
// Ambient Occlusion pass (SSAO.glsl / SSAO_Blur.glsl / PostProcess_SSAOApply.glsl
// driven by SSAORenderPass + AOApplyPass), back-filling the visual-evidence gap
// for SSAO/GTAO (shipped enabled in 9+ sandbox scenes with no on-screen test —
// extends issue #258's visual-regression infra).
//
// A neutral grey cube rests on a neutral grey floor, lit by an overhead white
// directional light, in front of a skybox. The scene is rendered through the
// FULL deferred Renderer3D pipeline from each pose three ways and written to
//   OloEditor/assets/tests/visual/SSAO_<state>.png
//   * SSAO_Off_<pose>   — SSAO disabled (lit reference frame)
//   * SSAO_On_<pose>    — SSAO enabled (lit, AO composited in)
//   * SSAO_AO_<pose>    — the AO buffer itself (SSAODebugView), white = unoccluded
//
// What SSAO must do (this is also the regression fix this branch ships — see
// SSAO.glsl: the previous horizon variant self-occluded flat ground at any
// tilted/grazing angle and darkened the whole floor ~50%; it now uses a
// normal-oriented hemisphere obscurance estimator with a world-space range
// check, so flat surfaces stay unoccluded):
//   * FLAT, OPEN surfaces are essentially UNOCCLUDED — the floor reads near-white
//     in the AO buffer (AO ~= 1), NOT a global grey wash. This is the bug fix.
//   * The BACKGROUND sky (no geometry, depth >= 1) is fully unoccluded (white).
//   * CONTACT / concave regions are OCCLUDED — a dark AO band sits in the crease
//     where the cube meets the floor.
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs and
// needs no committed reference image. It is asserted on the AO buffer itself
// (SSAODebugView) because that is where AO correctness is unambiguous — the lit
// composite multiplies AO into HDR colour before tone mapping, which compresses
// the visible delta, but the AO buffer shows the raw occlusion structure:
//   1. The sky reads near-white (unoccluded background).
//   2. The open flat floor ALSO reads bright (unoccluded) — proving SSAO no
//      longer self-occludes flat ground (the fix; a regression to the old
//      horizon math drops this well below the threshold).
//   3. A clearly darker AO band exists in the cube/floor contact crease — proving
//      SSAO still produces real contact occlusion, localised, not global.
//   4. Both lit frames render the floor non-black (the scene actually drew).
//
// Checked from TWO poses so a view-dependent regression is caught. Runs in the
// normal suite and SKIPs (not fails) when no GL 4.6 context exists, matching
// ContactShadow/SSGI/SSRVisualEvidenceTest. SSAO reads the G-Buffer (view-space
// normals + depth), so the fixture forces the deferred render path.
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

        // Mean luma over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] f64 BandLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sum += 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        // Scan a UV region as a grid of cells and return the DARKEST cell's mean
        // luma. The contact-occlusion band is small and its exact screen position
        // shifts with the pose, so locating the darkest cell within a candidate
        // region pins "a localised dark AO band exists" without a pixel-perfect
        // band (mirrors ContactShadowVisualEvidenceTest::MaxDarkeningInRegion).
        [[nodiscard]] f64 DarkestCellLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1,
                                          u32 cellsX, u32 cellsY)
        {
            f64 darkest = 1e9;
            const f32 dx = (x1 - x0) / static_cast<f32>(cellsX);
            const f32 dy = (y1 - y0) / static_cast<f32>(cellsY);
            for (u32 cy = 0; cy < cellsY; ++cy)
            {
                for (u32 cx = 0; cx < cellsX; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    darkest = std::min(darkest, BandLuma(px, cellX0, cellX0 + dx, cellY0, cellY0 + dy));
                }
            }
            return darkest;
        }
    } // namespace

    class SSAOVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // SSAO reads the G-Buffer (view-space normals + depth), so force the
            // deferred path — the full G-Buffer gives the cleanest, most
            // deterministic AO (matches SSGI/SSR/ContactShadow evidence tests).
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Overhead white sun pointing mostly straight down (slightly angled so
            // the cube faces catch a little light and read in the PNG). Cascaded
            // shadows are DISABLED so the floor is uniformly lit and the only floor
            // variation between OFF/ON is the SSAO contribution.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.25f, -0.92f, 0.3f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.5f;
                dl.m_CastShadows = false; // isolate the SSAO contribution
            }

            // Skybox so the background (no geometry) is a bright, stable reference
            // the contract can check SSAO leaves fully unoccluded. IBL is OFF: the
            // floor is lit by the white directional only, so it reads as neutral
            // grey (an IBL ambient would tint it sky-blue).
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = false;
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

            // Neutral grey floor at y = 0, pure-diffuse, sized so its far edge sits
            // below the horizon (upper frame is sky).
            {
                Entity floor = addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 40.0f, 1.0f, 40.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // A cube resting on the floor (unit cube spans -0.5..0.5, so a scale-5
            // cube centred at y=2.5 has its base exactly on y=0). The 90-degree
            // concave dihedral where its base meets the floor is the contact crease
            // SSAO must occlude.
            {
                Entity cube = addMesh("Occluder", MeshPrimitive::Cube, { 0.0f, 2.5f, 0.0f },
                                      { 5.0f, 5.0f, 5.0f });
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
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
            ASSERT_TRUE(fb) << "No composited framebuffer for SSAO capture '" << tag << "'";

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

            const std::string path = (dir / ("SSAO_" + tag + ".png")).string();
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

    // SSAO must darken the cube/floor contact crease while leaving flat open ground
    // and the background sky unoccluded (the fix: the old horizon variant darkened
    // the whole floor). Asserted on the AO buffer (SSADebugView), checked from TWO
    // poses, with lit OFF/ON frames saved alongside as evidence. SKIPs without a
    // GL 4.6 context (see header).
    TEST_F(SSAOVisualEvidenceTest, SSAODarkensContactsNotFlatSurfacesOrSky)
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
            pp.ActiveAOTechnique = AOTechnique::SSAO;
            pp.SSAOEnabled = true;
            pp.SSAORadius = 1.5f;    // wider than the 0.5 default so the contact band reads clearly; the fixed shader keeps flat ground unoccluded even at this radius (the old horizon variant went pure black here)
            pp.SSAOBias = 0.025f;    // production default self-occlusion guard
            pp.SSAOIntensity = 1.0f; // production default
            pp.SSAOSamples = 32;     // production default slice/step budget
        };

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Low-ish angles so the frame is split: SKY across the top (above the
        // floor's far horizon) and the lit FLOOR + cube across the lower half.
        // Two heights/pitches catch a view-dependent regression.
        const std::array<Pose, 2> poses = { {
            { "Angled", { 0.0f, 6.0f, 22.0f }, 0.0f, 0.30f },
            { "Higher", { 0.0f, 8.0f, 19.0f }, 0.0f, 0.42f },
        } };

        // Scan bands (UV) on the AO buffer (white = unoccluded). Sky high in the
        // frame; open floor low and to the LEFT of the centred cube (clear of both
        // the cube and the contact crease); the contact crease straddles the cube
        // base across the centre.
        constexpr f32 kSkyX0 = 0.10f, kSkyX1 = 0.90f, kSkyY0 = 0.04f, kSkyY1 = 0.16f;
        constexpr f32 kFloorX0 = 0.08f, kFloorX1 = 0.30f, kFloorY0 = 0.60f, kFloorY1 = 0.76f;
        constexpr f32 kCreaseX0 = 0.28f, kCreaseX1 = 0.72f, kCreaseY0 = 0.40f, kCreaseY1 = 0.62f;

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            pp.SSAOEnabled = false;
            pp.SSAODebugView = false;
            std::vector<u8> offPixels;
            Capture(std::string("Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, offPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            applyOnParams();
            pp.SSAODebugView = false;
            std::vector<u8> onPixels;
            Capture(std::string("On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, onPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            // Capture the AO buffer itself (white = unoccluded) for the contract.
            applyOnParams();
            pp.SSAODebugView = true;
            std::vector<u8> aoPixels;
            Capture(std::string("AO_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, aoPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            // 1) Both lit frames drew the scene (catch a black / failed render):
            //    the open floor reads as a stable mid-tone in both.
            EXPECT_GT(BandLuma(offPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1), 20.0)
                << "SSAO-off lit frame floor rendered (near-)black";
            EXPECT_GT(BandLuma(onPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1), 20.0)
                << "SSAO-on lit frame floor rendered (near-)black";

            // AO-buffer contract (white = unoccluded, dark = occluded).
            const f64 skyAO = BandLuma(aoPixels, kSkyX0, kSkyX1, kSkyY0, kSkyY1);
            const f64 floorAO = BandLuma(aoPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            const f64 creaseAO = DarkestCellLuma(aoPixels, kCreaseX0, kCreaseX1, kCreaseY0, kCreaseY1, 16, 12);

            // 2) The background sky (no geometry) is fully unoccluded — near-white.
            EXPECT_GT(skyAO, 170.0) << "sky is not unoccluded in the AO buffer (luma=" << skyAO
                                    << "). See SSAO_AO_" << pose.Name << ".png";

            // 3) THE FIX: the open flat floor is essentially unoccluded too — it
            //    reads bright, NOT a global grey wash. A regression to the old
            //    horizon math (which self-occluded flat ground ~50%) drops this far
            //    below the threshold.
            EXPECT_GT(floorAO, 165.0)
                << "open flat floor is self-occluded in the AO buffer (luma=" << floorAO
                << ") — SSAO is darkening flat ground, not just contacts. See SSAO_AO_" << pose.Name
                << ".png / SSAO_On_" << pose.Name << ".png";

            // 4) SSAO still produces real, LOCALISED contact occlusion: a clearly
            //    darker AO band exists in the cube/floor crease.
            EXPECT_LT(creaseAO, floorAO - 10.0)
                << "no contact-occlusion band in the cube/floor crease (darkest crease cell=" << creaseAO
                << " vs open floor=" << floorAO << "). See SSAO_AO_" << pose.Name << ".png";
        }
    }
} // namespace OloEngine::Tests
