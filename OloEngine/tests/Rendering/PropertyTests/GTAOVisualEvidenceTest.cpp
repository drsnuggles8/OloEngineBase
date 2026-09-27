// =============================================================================
// GTAOVisualEvidenceTest.cpp
//
// OLO_TEST_LAYER: L8
//
// Visual evidence (PNG) + a driver-independent contract for Ground Truth
// Ambient Occlusion (GTAO.comp driven by GTAORenderPass + AOApplyRenderPass),
// added alongside the fix for issue #533 ("Renderer: GTAO-lit scenes render
// too dark"). Mirrors SSAOVisualEvidenceTest's structure/contract shape.
//
// Root cause (see GTAO.comp and GTAOMathTest.cpp for the full derivation):
// the per-slice tangent-elevation angle ("n") was computed by projecting the
// surface normal against an axis built from `cross(sliceDirection,
// viewNormal)`. Crossing anything with `viewNormal` is, by definition of the
// cross product, always perpendicular to `viewNormal` — so that axis made
// `n` collapse to 0 for EVERY slice on EVERY pixel, regardless of the
// surface's actual tilt relative to the camera. Only a surface facing the
// camera dead-on has a genuinely-zero tilt; any other surface (i.e. almost
// every visible pixel in a normal 3D scene) then measured its horizon
// against the wrong (untilted) baseline and self-occluded — producing the
// reported near-black composite. The fix crosses the slice direction with
// the camera's fixed view axis instead of the surface normal.
//
// A neutral grey cube rests on a neutral grey floor, lit by an overhead white
// directional light, in front of a skybox — same scene as
// SSAOVisualEvidenceTest so the two techniques are directly comparable. The
// scene is rendered through the FULL deferred Renderer3D pipeline from two
// LOW-ANGLE poses (the bug is angle-dependent: it does not reproduce for a
// camera looking straight down, only at a grazing/tilted view of the floor)
// three ways and written to OloEditor/assets/tests/visual/GTAO_<state>.png:
//   * GTAO_Off_<pose>   — GTAO disabled (lit reference frame)
//   * GTAO_On_<pose>    — GTAO enabled (lit, AO composited in)
//   * GTAO_AO_<pose>    — the AO buffer itself (GTAODebugView), white = unoccluded
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs
// and needs no committed reference image, asserted on the AO buffer itself
// (GTAODebugView) because that is where AO correctness is unambiguous:
//   1. The sky reads near-white (unoccluded background).
//   2. The open flat floor, viewed at a grazing angle, ALSO reads bright
//      (unoccluded) — THE FIX. Issue #533's bug drove this well below
//      white because the tangent-elevation term never adapted to the
//      floor's tilt relative to the camera.
//   3. A clearly darker AO band exists in the cube/floor contact crease —
//      proving GTAO still produces real, localised contact occlusion.
//   4. Both lit frames render the floor non-black (the scene actually drew),
//      AND the lit GTAO-on frame is not catastrophically darker than
//      GTAO-off (guards the "essentially fully black" symptom directly, on
//      the composited frame, not just the AO buffer).
//
// Checked from TWO poses so a view-dependent regression is caught. Runs in
// the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching SSAO/SSGI/SSR/ContactShadowVisualEvidenceTest. GTAO reads the
// G-Buffer (view-space normals + depth), so the fixture forces the deferred
// render path.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Core/DebugLevers.h"
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
#include <iostream>
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
        // band (mirrors SSAOVisualEvidenceTest::DarkestCellLuma).
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

    class GTAOVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Overridden by the forward-path subclass below. Deferred stays the default
        // so the existing goldens (GTAO_*_Angled/Higher.png) keep their filenames
        // and content.
        RenderingPath m_RenderPath = RenderingPath::Deferred;
        // Prefix for evidence PNGs so the forward variant writes its own files
        // rather than clobbering the deferred goldens.
        std::string m_EvidencePrefix;

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Which path GTAO sources its normals from is the whole point of the
            // forward variant below, so it is a fixture knob rather than a
            // hard-coded Deferred (which is what let a forward-only GTAO bug ship:
            // every AO evidence test ran deferred, so nothing ever sampled the
            // forward path's AO output).
            Renderer3D::GetRendererSettings().Path = m_RenderPath;
            Renderer3D::ApplyRendererSettings();

            // Overhead white sun pointing mostly straight down (slightly angled so
            // the cube faces catch a little light and read in the PNG). Cascaded
            // shadows are DISABLED so the floor is uniformly lit and the only floor
            // variation between OFF/ON is the GTAO contribution.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.25f, -0.92f, 0.3f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.5f;
                dl.m_CastShadows = false; // isolate the GTAO contribution
            }

            // Skybox so the background (no geometry) is a bright, stable reference
            // the contract can check GTAO leaves fully unoccluded. IBL is OFF: the
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
            // GTAO must occlude.
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
            Capture(tag, camera, outPixels);
        }

        // Camera-taking overload, so a caller that needs an orbit pose (Focus)
        // rather than a free pose (SetPose) shares the same readback + PNG
        // evidence + round-trip verification.
        void Capture(const std::string& tag, EditorCamera& camera, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for GTAO capture '" << tag << "'";

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

            const std::string path = (dir / ("GTAO_" + m_EvidencePrefix + tag + ".png")).string();
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
        // Shared contract body, run once per render path. Extracted so the
        // forward variant asserts EXACTLY the same AO properties rather than a
        // weaker near-copy — the whole point is that both paths must agree.
        void RunAOContract()
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
                pp.ActiveAOTechnique = AOTechnique::GTAO;
                pp.GTAOEnabled = true;
                pp.GTAORadius = 1.5f;             // wider than the 0.5 default so the contact band reads clearly
                pp.GTAOPower = 2.2f;              // production default contrast curve
                pp.GTAOFalloffRange = 0.615f;     // production default
                pp.GTAOSampleDistribution = 2.0f; // production default
                pp.GTAOThinCompensation = 0.0f;   // production default
                pp.GTAODepthMipOffset = 3.3f;     // production default
                pp.GTAODenoiseEnabled = true;     // production default
                pp.GTAODenoisePasses = 4;         // production default
                pp.GTAODenoiseBeta = 1.2f;        // production default
                // ActiveAOTechnique swaps which AO pass is registered in the render
                // graph (RegisterSceneAndLightingNodes's switch on ActiveAOTechnique).
                // Without this, the graph stays wired for whatever technique was
                // active at build time, GTAOPass never executes, AOBuffer is never
                // written, and AOApplyRenderPass still multiplies the whole frame by
                // that all-zero buffer -- see issue #533 and olo_render_toggle_pass's
                // matching fix in McpTools.cpp.
                Renderer3D::ApplyRendererSettings();
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
            // Crucially, at these grazing pitches the floor is NOT face-on to the
            // camera -- this is exactly where issue #533's tangent-elevation bug
            // self-occluded flat ground (a straight-down camera pose would not
            // have reproduced it, since n == 0 is the correct answer there too).
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

                // EVERY GTAOEnabled / GTAODebugView change needs its own
                // ApplyRendererSettings() before the Capture. The pass reads a
                // CACHED copy of these settings, so a write that isn't followed
                // by an apply renders the PREVIOUS state: without the applies
                // below, the "Off" frame kept the GTAO-on wiring left applied by
                // the previous pose's applyOnParams(), and the "AO" frame never
                // entered debug view at all (applyOnParams() applies, then the
                // GTAODebugView write lands after it) — so aoPixels was the lit
                // composite, not the AO buffer, and the contract below was
                // asserting against the wrong image.
                pp.GTAOEnabled = false;
                pp.GTAODebugView = false;
                Renderer3D::ApplyRendererSettings();
                std::vector<u8> offPixels;
                Capture(std::string("Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, offPixels);
                if (::testing::Test::HasFatalFailure())
                    return;

                applyOnParams();
                pp.GTAODebugView = false;
                Renderer3D::ApplyRendererSettings();
                std::vector<u8> onPixels;
                Capture(std::string("On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, onPixels);
                if (::testing::Test::HasFatalFailure())
                    return;

                // Capture the AO buffer itself (white = unoccluded) for the contract.
                applyOnParams();
                pp.GTAODebugView = true;
                Renderer3D::ApplyRendererSettings();
                std::vector<u8> aoPixels;
                Capture(std::string("AO_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, aoPixels);
                if (::testing::Test::HasFatalFailure())
                    return;

                // 1) Both lit frames drew the scene (catch a black / failed render):
                //    the open floor reads as a stable mid-tone in both.
                const f64 offFloorLit = BandLuma(offPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
                const f64 onFloorLit = BandLuma(onPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
                EXPECT_GT(offFloorLit, 20.0) << "GTAO-off lit frame floor rendered (near-)black";
                EXPECT_GT(onFloorLit, 20.0) << "GTAO-on lit frame floor rendered (near-)black. See GTAO_On_"
                                            << pose.Name << ".png";

                // 1b) THE ISSUE #533 SYMPTOM, checked directly on the composite: GTAO
                //     must not crush the open floor to a near-black fraction of its
                //     GTAO-off brightness. Deliberately loose: it guards the reported
                //     "essentially fully black" collapse. The tight bar (an unoccluded
                //     floor reads AO >= 0.97 from 0 to 75 degrees) is
                //     GTAOElevationEvidenceTest's, since #1463 fixed the view-angle
                //     darkening this comment used to call a residual of the integral.
                EXPECT_GT(onFloorLit, offFloorLit * 0.15)
                    << "GTAO-on lit floor (" << onFloorLit << ") is far darker than GTAO-off (" << offFloorLit
                    << ") on OPEN ground -- near-black composite regression. See GTAO_On_" << pose.Name
                    << ".png vs GTAO_Off_" << pose.Name << ".png";

                // AO-buffer contract (white = unoccluded, dark = occluded).
                const f64 skyAO = BandLuma(aoPixels, kSkyX0, kSkyX1, kSkyY0, kSkyY1);
                const f64 floorAO = BandLuma(aoPixels, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
                const f64 creaseAO = DarkestCellLuma(aoPixels, kCreaseX0, kCreaseX1, kCreaseY0, kCreaseY1, 16, 12);

                // 2) The background sky (no geometry) is fully unoccluded — near-white.
                EXPECT_GT(skyAO, 170.0) << "sky is not unoccluded in the AO buffer (luma=" << skyAO
                                        << "). See GTAO_AO_" << pose.Name << ".png";

                // 3) THE FIX: the open flat floor, viewed at a grazing angle, is no
                //    longer a near-black wash. Before the #533 fix (axisVS built from
                //    the surface normal instead of a view-related axis, which
                //    collapsed the tangent-elevation angle to 0 for every tilted
                //    slice) this read as literally 0 whenever the graph was actually
                //    wired for GTAO — see GTAOMathTest.cpp for the degenerate-formula
                //    regression tests (both that bug and the follow-up cosN-tautology
                //    bug the per-pixel view-vector basis also fixes).
                //    The earlier note here said the open ground could never read
                //    white because the horizon integral does not reconstruct 1.0 for a
                //    tilted surface. It does: the shortfall was issue #1463 (horizons
                //    measured against the normal, not the view vector), and
                //    GTAOElevationEvidenceTest now pins the raw AO at >= 0.97.
                EXPECT_GT(floorAO, 50.0)
                    << "open flat floor is essentially black in the AO buffer (luma=" << floorAO
                    << ") at a grazing camera angle -- GTAO is collapsing to near-zero, not just tuned "
                       "contrast (issue #533). See GTAO_AO_"
                    << pose.Name << ".png / GTAO_On_" << pose.Name << ".png";

                // 4) The cube/floor contact crease must not read BRIGHTER than the
                //    open floor (a sanity check on the AO buffer's polarity/values).
                //    Only a polarity check here. The crease used to read close to the
                //    open floor because the floor itself was falsely occluded (#1463);
                //    GTAOElevationEvidenceTest now asserts the crease strictly, on the
                //    raw AO buffer (<= 0.75 five centimetres from the wall).
                EXPECT_LT(creaseAO, floorAO + 10.0)
                    << "crease reads brighter than open floor -- possible sign-flipped AO term (darkest "
                       "crease cell="
                    << creaseAO << " vs open floor=" << floorAO << "). See GTAO_AO_" << pose.Name << ".png";
            }
        }
    };

    // GTAO must darken the cube/floor contact crease while leaving flat open
    // ground and the background sky unoccluded — even at a grazing camera
    // angle. Asserted on the AO buffer (GTAODebugView), checked from TWO
    // poses, with lit OFF/ON frames saved alongside as evidence. SKIPs
    // without a GL 4.6 context (see header).
    TEST_F(GTAOVisualEvidenceTest, GTAODarkensContactsNotFlatSurfacesOrSky)
    {
        RunAOContract();
    }

    // Same contract, FORWARD path. This exists because the deferred-only coverage
    // above let a forward-path GTAO bug ship undetected: the forward scene shader
    // writes VIEW-space normals while GTAO.comp converts its input to view space
    // itself, so forward normals were transformed twice and every surface came back
    // fully occluded (AO ~0.03 everywhere, swimming as the camera turned). The sky
    // and open-floor assertions in RunAOContract fail loudly on that, but only when
    // something actually renders GTAO through the forward path — which nothing did.
    class GTAOForwardPathVisualEvidenceTest : public GTAOVisualEvidenceTest
    {
      protected:
        void BuildScene() override
        {
            m_RenderPath = RenderingPath::Forward;
            m_EvidencePrefix = "Fwd_";
            GTAOVisualEvidenceTest::BuildScene();
        }
    };

    TEST_F(GTAOForwardPathVisualEvidenceTest, GTAODarkensContactsNotFlatSurfacesOrSkyInForwardPath)
    {
        RunAOContract();
    }

    // The contract above is necessary but NOT sufficient: at a near-axis-aligned
    // camera pose mat3(V) is close to identity, so applying the view rotation twice
    // is nearly a no-op and a double-transformed normal still yields plausible AO.
    // The invariant that actually pins this is PARITY — forward and deferred shade
    // the same scene from the same camera, so their AO buffers must agree. The
    // error grows with the camera's rotation away from the axes, hence the
    // deliberately off-axis yaw/pitch below.
    TEST_F(GTAOForwardPathVisualEvidenceTest, ForwardAndDeferredGTAOAgreeAtOffAxisPose)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Well away from any axis: both mat3(V) rows and columns mix all three
        // components, so a doubled view rotation is maximally wrong here.
        // Orbit the cube rather than free-posing at it: Focus() guarantees the
        // geometry is framed at any yaw, so the pose can be chosen purely for
        // being off-axis. An empty frame would make the two paths agree
        // trivially — which is exactly what a mis-aimed free pose produced.
        constexpr f32 kOrbitDistance = 24.0f;
        constexpr f32 kYaw = 0.9f;    // ~52 deg — mixes X and Z throughout mat3(V)
        constexpr f32 kPitch = 0.55f; // looking down at the cube/floor contact

        auto& pp = Renderer3D::GetPostProcessSettings();
        const auto captureAO = [&](RenderingPath path, const char* tag, std::vector<u8>& out)
        {
            Renderer3D::GetRendererSettings().Path = path;
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.GTAOEnabled = true;
            pp.GTAORadius = 1.5f;
            pp.GTAOPower = 2.2f;
            pp.GTAODenoiseEnabled = true;
            pp.GTAODenoisePasses = 4;
            pp.GTAODebugView = true;
            Renderer3D::ApplyRendererSettings();

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(glm::vec3(0.0f, 1.5f, 0.0f), kOrbitDistance, kYaw, kPitch);
            Capture(tag, camera, out);
        };

        std::vector<u8> forwardAO;
        captureAO(RenderingPath::Forward, "Parity_Forward", forwardAO);
        if (::testing::Test::HasFatalFailure())
            return;

        std::vector<u8> deferredAO;
        captureAO(RenderingPath::Deferred, "Parity_Deferred", deferredAO);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_EQ(forwardAO.size(), deferredAO.size());

        // Compare on the AO channel only. The two paths are not bit-identical
        // (different shaders, different precision along the way), so the bar is a
        // mean absolute difference. Measured on this scene/pose: 0.007/255 with
        // the normal-space fix in place, 6.07/255 with it reverted — so the 1.5
        // bound below sits ~200x above the clean value (ample room for GPU
        // nondeterminism) and ~4x below the regression it exists to catch.
        f64 sumAbs = 0.0;
        f64 sumForward = 0.0;
        f64 sumDeferred = 0.0;
        std::size_t counted = 0;
        for (std::size_t i = 0; i + 3 < forwardAO.size(); i += 4)
        {
            const f64 f = static_cast<f64>(forwardAO[i]);
            const f64 d = static_cast<f64>(deferredAO[i]);
            sumAbs += std::abs(f - d);
            sumForward += f;
            sumDeferred += d;
            ++counted;
        }
        ASSERT_GT(counted, 0u);

        const f64 meanAbsDiff = sumAbs / static_cast<f64>(counted);
        const f64 meanForward = sumForward / static_cast<f64>(counted);
        const f64 meanDeferred = sumDeferred / static_cast<f64>(counted);

        // Guard against the degenerate "both black" / "both white" pass: an AO
        // buffer with no variation would trivially satisfy the diff bound.
        EXPECT_GT(meanForward, 20.0) << "Forward AO buffer is (near-)black -- see GTAO_Fwd_Parity_Forward.png";
        EXPECT_GT(meanDeferred, 20.0) << "Deferred AO buffer is (near-)black -- see GTAO_Fwd_Parity_Deferred.png";

        EXPECT_LT(meanAbsDiff, 1.5)
            << "Forward and deferred GTAO disagree (mean |diff| = " << meanAbsDiff
            << "/255, forward mean = " << meanForward << ", deferred mean = " << meanDeferred
            << "). The forward scene shader writes VIEW-space normals while the deferred "
               "G-Buffer writes WORLD-space ones; if GTAO applies the view matrix to both, "
               "forward normals are rotated twice and the AO is wrong by an amount that "
               "grows with camera rotation. Compare GTAO_Fwd_Parity_Forward.png against "
               "GTAO_Fwd_Parity_Deferred.png.";
    }

    // =========================================================================
    // Issue #1463, GPU half: an UNOCCLUDED floor reads AO >= 0.97 from 0 to 75
    // degrees off its normal, on every rendering path, and a contact crease
    // still reads clearly occluded.
    //
    // Before #1463 GTAO measured its horizons against the surface normal while
    // reconstructing them as angles from the view vector, which clipped the lit
    // half of every slice by the normal's tilt: DDGITest.olo's open floor read
    // 1.00 straight down, 0.71 at 45 degrees and 0.34 grazing. The CPU mirror in
    // GTAOMathTest reproduces those numbers from the old conventions; this
    // reads the real AOBuffer, raw, after the real pipeline.
    //
    // Production GTAO settings (radius 0.5, power 2.2), because the issue's
    // numbers are production numbers. The floor point is at (10, 0, 10), 7.5 m
    // clear of the cube, so nothing lies within the radius of it. AOBuffer is a
    // transient read AFTER the frame, so aliasing is off for the fixture: with
    // it on, the planner may hand the texture to a later pass first.
    //
    // Evidence: GTAO_GL_<Path>_Elevation<deg>.png and GTAO_GL_<Path>_Crease.png,
    // the AO debug view of each pose.
    // =========================================================================
    class GTAOElevationEvidenceTest : public GTAOVisualEvidenceTest, public ::testing::WithParamInterface<RenderingPath>
    {
      public:
        [[nodiscard]] static const char* PathTag(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

      protected:
        void SetUp() override
        {
            m_SavedDisableAliasing = Levers::DisableTransientAliasing();
            Levers::SetDisableTransientAliasing(true);
            GTAOVisualEvidenceTest::SetUp();
        }

        void TearDown() override
        {
            GTAOVisualEvidenceTest::TearDown();
            Levers::SetDisableTransientAliasing(m_SavedDisableAliasing);
        }

        void BuildScene() override
        {
            m_RenderPath = GetParam();
            m_EvidencePrefix = std::string("GL_") + PathTag(GetParam()) + "_";
            GTAOVisualEvidenceTest::BuildScene();
        }

        // The whole AOBuffer, raw (RGBA float readback; AO in .r).
        struct AOImage
        {
            i32 Width = 0;
            i32 Height = 0;
            std::vector<f32> Texels;

            [[nodiscard]] f32 At(i32 x, i32 y) const
            {
                return Texels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(Width) + static_cast<std::size_t>(x)) * 4u];
            }
        };

        [[nodiscard]] AOImage ReadAO()
        {
            AOImage image;
            const u32 ao = Renderer3D::ResolveFrameGraphTexture(ResourceNames::AOBuffer);
            EXPECT_NE(ao, 0u) << "no AOBuffer this frame";
            if (ao == 0u)
                return image;
            ::glGetTextureLevelParameteriv(ao, 0, GL_TEXTURE_WIDTH, &image.Width);
            ::glGetTextureLevelParameteriv(ao, 0, GL_TEXTURE_HEIGHT, &image.Height);
            if (image.Width > 0 && image.Height > 0)
                ReadbackRgbaFloat(ao, static_cast<u32>(image.Width), static_cast<u32>(image.Height), image.Texels);
            return image;
        }

        // Mean raw AO over a (2*half+1)^2 window at the centre of AOBuffer:
        // the texels under the camera's focus point.
        [[nodiscard]] f64 CentreAO(i32 half)
        {
            const AOImage image = ReadAO();
            if (image.Texels.empty())
                return -1.0;
            const i32 w = image.Width;
            const i32 h = image.Height;
            EXPECT_GT(w, 2 * half);
            EXPECT_GT(h, 2 * half);
            if (w <= 2 * half || h <= 2 * half)
                return -1.0;
            const std::vector<f32>& texels = image.Texels;
            f64 sum = 0.0;
            u32 count = 0;
            for (i32 y = h / 2 - half; y <= h / 2 + half; ++y)
            {
                for (i32 x = w / 2 - half; x <= w / 2 + half; ++x)
                {
                    const std::size_t texel = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                    sum += texels[texel * 4u];
                    ++count;
                }
            }
            return sum / static_cast<f64>(count);
        }

        bool m_SavedDisableAliasing = false;
    };

    TEST_P(GTAOElevationEvidenceTest, UnoccludedFloorIsFullyVisibleFromEveryElevation)
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
        pp.ActiveAOTechnique = AOTechnique::GTAO;
        pp.GTAOEnabled = true;
        pp.GTAORadius = 0.5f;             // production default
        pp.GTAOPower = 2.2f;              // production default
        pp.GTAOFalloffRange = 0.615f;     // production default
        pp.GTAOSampleDistribution = 2.0f; // production default
        pp.GTAOThinCompensation = 0.0f;   // production default
        pp.GTAODepthMipOffset = 3.3f;     // production default
        pp.GTAODenoiseEnabled = true;     // production default
        pp.GTAODenoisePasses = 4;         // production default
        pp.GTAODenoiseBeta = 1.2f;        // production default
        pp.GTAODebugView = true;          // the evidence PNG shows the AO buffer
        Renderer3D::ApplyRendererSettings();

        const auto aim = [](const glm::vec3& target, f32 distance, f32 pitchDeg)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(target, distance, 0.0f, glm::radians(pitchDeg));
            return camera;
        };

        const glm::vec3 openFloor(10.0f, 0.0f, 10.0f);
        for (const i32 zenithDeg : { 0, 15, 30, 45, 60, 75 })
        {
            SCOPED_TRACE(zenithDeg);
            EditorCamera camera = aim(openFloor, 6.0f, 90.0f - static_cast<f32>(zenithDeg));
            std::vector<u8> pixels;
            Capture("Elevation" + std::to_string(zenithDeg), camera, pixels);
            if (::testing::Test::HasFatalFailure())
                return;
            const f64 ao = CentreAO(8);
            std::cout << "[GTAO #1463] " << PathTag(GetParam()) << " open floor, " << zenithDeg
                      << " deg from the normal: AO " << ao << '\n';
            EXPECT_GE(ao, 0.97) << "an unoccluded floor " << zenithDeg << " degrees from its normal reads AO " << ao
                                << " (issue #1463). See GTAO_" << m_EvidencePrefix << "Elevation" << zenithDeg << ".png";
        }

        // Positive control: a floor point 5 cm in front of the 5 m cube's +z
        // face. Fixing the flat plane must not flatten the crease.
        EditorCamera creaseCamera = aim(glm::vec3(0.0f, 0.0f, 2.55f), 8.0f, 35.0f);
        std::vector<u8> pixels;
        Capture("Crease", creaseCamera, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const f64 crease = CentreAO(1);
        std::cout << "[GTAO #1463] " << PathTag(GetParam()) << " crease, 5 cm from the wall: AO " << crease << '\n';
        EXPECT_LE(crease, 0.75) << "the cube/floor contact crease is not clearly occluded (AO " << crease
                                << "). See GTAO_" << m_EvidencePrefix << "Crease.png";
    }

    // Issue #1503: GTAO reads the HZB mip chain on GL, as it does on Vulkan.
    // GTAODepthMipOffset decides which level each horizon sample reads; before
    // the fix, GL's HZB had no mip filter and every read came from level 0, so
    // moving the offset from its 3.3 default to 0 changed 21 pixels live on GL
    // against 350k on Vulkan. Prediction for a working chain: many texels
    // change, and the crease gets BRIGHTER, because the coarser levels are
    // max-reduced (farther) depths, and a farther occluder can only lower a
    // horizon. Evidence: GTAO_GL_<Path>_MipOffset33.png / _MipOffset0.png.
    TEST_P(GTAOElevationEvidenceTest, HorizonSamplesReadTheHzbMipChain)
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
        pp.ActiveAOTechnique = AOTechnique::GTAO;
        pp.GTAOEnabled = true;
        pp.GTAORadius = 0.5f;
        pp.GTAOPower = 2.2f;
        pp.GTAOFalloffRange = 0.615f;
        pp.GTAOSampleDistribution = 2.0f;
        pp.GTAOThinCompensation = 0.0f;
        pp.GTAODenoiseEnabled = true;
        pp.GTAODenoisePasses = 4;
        pp.GTAODenoiseBeta = 1.2f;
        pp.GTAODebugView = true;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.Focus(glm::vec3(0.0f, 0.0f, 2.55f), 8.0f, 0.0f, glm::radians(35.0f));

        const auto captureAt = [&](f32 mipOffset, const char* tag)
        {
            pp.GTAODepthMipOffset = mipOffset;
            Renderer3D::ApplyRendererSettings();
            std::vector<u8> pixels;
            Capture(tag, camera, pixels);
            return ReadAO();
        };
        const AOImage fine = captureAt(3.3f, "MipOffset33");
        if (::testing::Test::HasFatalFailure())
            return;
        const AOImage coarse = captureAt(0.0f, "MipOffset0");
        if (::testing::Test::HasFatalFailure())
            return;
        ASSERT_FALSE(fine.Texels.empty());
        ASSERT_EQ(fine.Texels.size(), coarse.Texels.size());

        std::size_t changed = 0;
        for (i32 y = 0; y < fine.Height; ++y)
            for (i32 x = 0; x < fine.Width; ++x)
                changed += std::abs(fine.At(x, y) - coarse.At(x, y)) > 2.0f / 255.0f ? 1u : 0u;
        const f64 changedFraction =
            static_cast<f64>(changed) / (static_cast<f64>(fine.Width) * static_cast<f64>(fine.Height));

        // The crease: the same window CentreAO(1) reads for the positive control.
        const auto creaseAO = [](const AOImage& image)
        {
            f64 sum = 0.0;
            for (i32 y = image.Height / 2 - 1; y <= image.Height / 2 + 1; ++y)
                for (i32 x = image.Width / 2 - 1; x <= image.Width / 2 + 1; ++x)
                    sum += image.At(x, y);
            return sum / 9.0;
        };
        const f64 creaseFine = creaseAO(fine);
        const f64 creaseCoarse = creaseAO(coarse);
        std::cout << "[GTAO #1503] " << PathTag(GetParam()) << " mip offset 3.3 -> 0: " << changedFraction * 100.0
                  << " % of texels change; crease AO " << creaseFine << " -> " << creaseCoarse << '\n';

        EXPECT_GE(changedFraction, 0.01) << "moving GTAODepthMipOffset from 3.3 to 0 changed " << changedFraction * 100.0
                                         << " % of the AO buffer: the horizon samples are not reading the HZB's "
                                            "mip chain (issue #1503). See GTAO_"
                                         << m_EvidencePrefix << "MipOffset0.png";
        EXPECT_GT(creaseCoarse, creaseFine)
            << "coarser, max-reduced HZB levels must lower the crease's horizons, not raise them (crease AO "
            << creaseFine << " -> " << creaseCoarse << ")";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, GTAOElevationEvidenceTest,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(GTAOElevationEvidenceTest::PathTag(info.param)); });
} // namespace OloEngine::Tests
