// =============================================================================
// ContactShadowVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the Screen-Space
// Contact Shadows pass (PostProcess_ContactShadow.glsl / ContactShadowRenderPass).
//
// A neutral grey floor sits under a cube that rests on it, lit by a grazing
// directional light. The directional light's cascaded shadow map is DISABLED
// (m_CastShadows = false) so the ONLY thing that can darken the floor is the
// contact-shadow pass: with it off the floor is uniformly lit, with it on a dark
// band appears on the floor along the cube's lee side where the cube occludes the
// sun. The scene is rendered twice through the FULL deferred Renderer3D pipeline
// from the same pose — once with contact shadows OFF and once ON — and both
// frames are written to
//   OloEditor/assets/tests/visual/ContactShadow_<state>.png
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs and
// needs no committed reference image: turning contact shadows on must DARKEN the
// floor band beside the cube base (a neutral darkening, not a colour shift), and
// a control band on the far/lit side of the cube must stay essentially unchanged
// (the effect is a localised contact shadow, not a global dimming). The cheap
// contact-shadow *math* contracts (projection round-trip, occlusion predicate,
// fade curves, multiplicative composite) live in ContactShadowMathTest.cpp.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching SSGIVisualEvidenceTest. Contact shadows are deferred-only, so the
// fixture forces the deferred render path.
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

            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
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

        // Per-cell darkening result over a scanned region.
        struct DarkenCell
        {
            f64 Drop = 0.0; // off.Luma - on.Luma at the strongest cell
            f64 dR = 0.0, dG = 0.0, dB = 0.0;
            BandStats Off{}; // off stats at that cell (to confirm non-black)
            BandStats On{};
        };

        // Scan a UV region as a grid of cells and return the cell with the
        // greatest luma darkening (off -> on). Contact shadows are small and their
        // exact screen position shifts with the pose, so rather than hard-code a
        // pixel-perfect band we locate the strongest-darkening cell within the
        // candidate region — robust to small framing differences while still
        // pinning that a localised neutral darkening exists.
        [[nodiscard]] DarkenCell MaxDarkeningInRegion(const std::vector<u8>& off, const std::vector<u8>& on,
                                                      f32 x0, f32 x1, f32 y0, f32 y1,
                                                      u32 cellsX, u32 cellsY)
        {
            DarkenCell best;
            const f32 dx = (x1 - x0) / static_cast<f32>(cellsX);
            const f32 dy = (y1 - y0) / static_cast<f32>(cellsY);
            for (u32 cy = 0; cy < cellsY; ++cy)
            {
                for (u32 cx = 0; cx < cellsX; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    const BandStats o = SampleBand(off, cellX0, cellX0 + dx, cellY0, cellY0 + dy);
                    const BandStats n = SampleBand(on, cellX0, cellX0 + dx, cellY0, cellY0 + dy);
                    const f64 drop = o.Luma() - n.Luma();
                    if (drop > best.Drop)
                        best = { drop, o.R - n.R, o.G - n.G, o.B - n.B, o, n };
                }
            }
            return best;
        }
    } // namespace

    class ContactShadowVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Contact shadows only run on the deferred path (they read the G-Buffer).
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Grazing sun from the left (travels toward +X and down). Cascaded
            // shadows are DISABLED so the only floor darkening comes from the
            // contact-shadow pass — the differential is then unambiguous.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.7f, -0.45f, 0.0f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.5f;
                dl.m_CastShadows = false; // isolate the contact-shadow contribution
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

            // Neutral grey floor at y = 0. Pure-diffuse so any darkening it gains
            // is the contact shadow, not a material/specular effect.
            {
                Entity floor = addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 80.0f, 1.0f, 80.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // A cube resting on the floor (unit cube spans -0.5..0.5, so a scale-5
            // cube centred at y=2.5 has its base exactly on y=0). It occludes the
            // grazing sun, casting a short contact shadow onto the floor on its
            // +X (lee) side.
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
            ASSERT_TRUE(fb) << "No composited framebuffer for contact-shadow capture '" << tag << "'";

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

            const std::string path = (dir / ("ContactShadow_" + tag + ".png")).string();
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

    // Contact shadow off vs on: the floor band along the cube's lee side must
    // DARKEN when contact shadows are enabled, while a control band on the lit
    // side stays essentially unchanged. Checked from TWO poses. SKIPs without a
    // GL 4.6 context (see header).
    TEST_F(ContactShadowVisualEvidenceTest, ContactShadowDarkensFloorBesideOccluder)
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
            pp.ContactShadowEnabled = true;
            pp.ContactShadowIntensity = 1.0f;
            pp.ContactShadowMaxDistance = 4.0f; // beyond the 1m default so the shadow reads clearly for the test
            pp.ContactShadowThickness = 1.5f;   // a solid occluder read (the squared falloff keeps the far end clean)
            pp.ContactShadowStride = 0.06f;
            pp.ContactShadowMaxSteps = 80;
            pp.ContactShadowBias = 0.02f;
            pp.ContactShadowEdgeFade = 0.1f;
        };

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // The cube stands at the origin (base on y=0, top y=5), screen-centred and
        // looked at down -Z. The grazing sun travels +X, so the contact shadow
        // falls on the +X (screen-RIGHT) floor; the -X (screen-LEFT) floor stays
        // fully lit. Both poses look down at the contact region from different
        // heights/distances so a regression that is view-dependent is caught.
        const std::array<Pose, 2> poses = { {
            { "Angled", { 0.0f, 9.0f, 16.0f }, 0.0f, 0.55f },
            { "Higher", { 0.0f, 11.0f, 15.0f }, 0.0f, 0.7f },
        } };

        // Scan regions (UV), chosen wide enough to contain the contact shadow
        // across both poses without clipping the cube (right edge ~0.61). The
        // strongest-darkening cell within the lee region is located automatically
        // (see MaxDarkeningInRegion), so the contract does not depend on a
        // pixel-perfect band. The lit region mirrors it on the cube's left.
        constexpr f32 kLeeX0 = 0.62f, kLeeX1 = 0.82f; // screen-right of the cube
        constexpr f32 kLitX0 = 0.18f, kLitX1 = 0.38f; // screen-left of the cube
        constexpr f32 kScanY0 = 0.35f, kScanY1 = 0.70f;
        constexpr u32 kCellsX = 16, kCellsY = 16;

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            pp.ContactShadowEnabled = false;
            std::vector<u8> offPixels;
            Capture(std::string("Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, offPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            applyOnParams();
            std::vector<u8> onPixels;
            Capture(std::string("On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, onPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            // Both frames must be non-trivial (catch a black / failed render): the
            // open lit floor on the left is a stable mid-tone in both.
            const BandStats offLit = SampleBand(offPixels, kLitX0, kLitX1, kScanY0, kScanY1);
            const BandStats onLit = SampleBand(onPixels, kLitX0, kLitX1, kScanY0, kScanY1);
            EXPECT_GT(offLit.Luma(), 20.0) << "contact-shadow-off frame rendered (near-)black";
            EXPECT_GT(onLit.Luma(), 20.0) << "contact-shadow-on frame rendered (near-)black";

            // Core contract: enabling contact shadows produces a localised neutral
            // DARKENING somewhere on the lee-side floor.
            const DarkenCell lee = MaxDarkeningInRegion(offPixels, onPixels, kLeeX0, kLeeX1, kScanY0, kScanY1, kCellsX, kCellsY);
            EXPECT_GT(lee.Drop, 5.0)
                << "Enabling contact shadows did not darken the lee-side floor (max drop=" << lee.Drop
                << "). See ContactShadow_Off_" << pose.Name << ".png / ContactShadow_On_" << pose.Name << ".png";

            // The strongest darkening must read as a neutral shadow (a multiply on
            // the grey floor), not a colour shift: every channel drops and the
            // per-channel drops stay close.
            EXPECT_GT(lee.dR, 0.0) << "red channel did not darken";
            EXPECT_GT(lee.dG, 0.0) << "green channel did not darken";
            EXPECT_GT(lee.dB, 0.0) << "blue channel did not darken";
            EXPECT_LT(std::abs(lee.dR - lee.dG), lee.dR * 0.6 + 6.0) << "darkening is not neutral (R vs G drift)";
            EXPECT_LT(std::abs(lee.dR - lee.dB), lee.dR * 0.6 + 6.0) << "darkening is not neutral (R vs B drift)";

            // The effect must be LOCALISED to the contact, not a global dim: the
            // lit floor on the cube's opposite side must show no comparable
            // darkening (allow a small margin for AA/dither at the cube edge).
            const DarkenCell lit = MaxDarkeningInRegion(offPixels, onPixels, kLitX0, kLitX1, kScanY0, kScanY1, kCellsX, kCellsY);
            EXPECT_LT(lit.Drop, lee.Drop * 0.5)
                << "contact shadows darkened the lit (sun-facing) floor as much as the lee floor (lit drop="
                << lit.Drop << " lee drop=" << lee.Drop << "). See ContactShadow_On_" << pose.Name << ".png";
        }
    }
} // namespace OloEngine::Tests
