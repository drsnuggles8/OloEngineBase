// =============================================================================
// PCSSVisualEvidenceTest.cpp
//
// Visual evidence for Percentage-Closer Soft Shadows (contact-hardening
// variable-penumbra directional shadows). Renders a deliberately shadow-centric
// scene — a flat ground plane, a tall thin pole standing ON the ground, and a
// cube floating ABOVE it — lit by a low-angle directional sun, through the FULL
// Renderer3D editor pipeline, with PCSS toggled OFF then ON.
//
// The pole is the contact-hardening probe: its shadow is sharp where the pole
// meets the ground (occluder touching receiver) and softens toward the far tip
// (occluder far from receiver). The float­ing cube adds a second, gap-separated
// shadow. In rebase mode the frames are written to
//   OloEditor/assets/tests/visual/PCSS_<pose>_<mode>.png
// for a human (or the agent) to eyeball the contact hardening.
//
// Driver-independent contracts (no committed golden PNGs, so no cross-GPU RMSE
// flakiness):
//   1. The PCSS-on frame contains both lit ground and shadowed ground — a
//      shadow actually renders.
//   2. It contains a non-trivial band of partial-shadow (penumbra) pixels — the
//      edges are soft, not a hard binary cut.
//   3. PCSS-on differs measurably from PCSS-off (legacy hardware PCF) — the
//      toggle actually changes the image. The cheap penumbra *math* contract is
//      pinned separately in Rendering/PCSSShadowTest.cpp.
//
// Runs in the normal suite; SKIPs cleanly (not fails) when no GL 4.6 context
// exists, matching the issue #258 RendererAttachedTest discipline (each render
// wrapped in a GLStateGuard via RunEditorFrames).
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
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
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        constexpr f32 kCaptureTime = 5.0f; // frozen clock -> deterministic frames

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }
    } // namespace

    class PCSSVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Low-angle sun pointing toward +Z and down, so the pole throws a long
            // shadow toward the camera (which sits on +Z). White, bright.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 25.0f, -25.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.18f, -0.55f, 0.55f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.5f;
            }

            auto addPrimitive = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
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
            };

            // Flat light-grey ground (the receiver) — large so the long shadow fits.
            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f },
                         { 0.72f, 0.72f, 0.74f });
            // Tall thin pole STANDING on the ground at z = -2. Contact at the base
            // (sharp), tip of its cast shadow far from the occluder (soft).
            addPrimitive("Pole", MeshPrimitive::Cube, { 0.0f, 6.0f, -2.0f }, { 0.6f, 12.0f, 0.6f },
                         { 0.55f, 0.55f, 0.58f });
            // Cube floating ABOVE the ground — a second, gap-separated soft shadow.
            addPrimitive("FloatCube", MeshPrimitive::Cube, { 6.0f, 4.0f, 1.0f }, { 2.0f, 2.0f, 2.0f },
                         { 0.6f, 0.3f, 0.25f });
        }

        // Render one pose with PCSS off/on, read back the composited frame, flip
        // to top-down rows, optionally write the golden PNG (rebase only).
        void Capture(const std::string& poseName, bool softShadows, const EditorCamera& camera,
                     std::vector<u8>& outPixels)
        {
            // Toggle PCSS for this capture. SetSettings (no resolution change) just
            // updates the flag; the next shadow-pass UploadUBO picks it up.
            ShadowSettings s = Renderer3D::GetShadowMap().GetSettings();
            s.Enabled = true;
            s.SoftShadows = softShadows;
            s.Softness = 1.5f; // a clearly-visible apparent light size
            Renderer3D::GetShadowMap().SetSettings(s);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the TOP of the frame.
            {
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
            }

            if (GoldenRebaseRequested())
            {
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string path =
                    (dir / ("PCSS_" + poseName + "_" + (softShadows ? "on" : "off") + ".png")).string();
                ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                 outPixels.data(), static_cast<int>(kWidth) * 4);
            }
        }
    };

    // Capture the contact-hardening probe scene and assert the driver-independent
    // soft-shadow contracts. SKIPs without a GL 4.6 context.
    TEST_F(PCSSVisualEvidenceTest, SoftShadowsRenderAndDifferFromHardPCF)
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

        // Render one camera pose with hard PCF then PCSS and assert the
        // driver-independent soft-shadow contracts. Classifies ground luma over the
        // lower 55% of the frame (foreground ground + cast shadows): lit grey ground
        // reads bright; a shadow darkens it; PCSS spreads occlusion into a lighter
        // penumbra so its deepest-dark (umbra) area is smaller than hard PCF's.
        const auto verifyPose = [this](const std::string& poseName, const EditorCamera& camera)
        {
            std::vector<u8> hardPixels;
            Capture(poseName, /*softShadows=*/false, camera, hardPixels);
            if (::testing::Test::HasFatalFailure())
                return;
            std::vector<u8> softPixels;
            Capture(poseName, /*softShadows=*/true, camera, softPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            const u32 yStart = static_cast<u32>(static_cast<f32>(kHeight) * 0.45f);
            const auto classify = [&](const std::vector<u8>& px, std::size_t& lit,
                                      std::size_t& shadowed, std::size_t& umbra, std::size_t& total)
            {
                lit = shadowed = umbra = total = 0;
                for (u32 y = yStart; y < kHeight; ++y)
                {
                    for (u32 x = 0; x < kWidth; ++x)
                    {
                        const u8* p = px.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                        const f64 l = Luma(p);
                        ++total;
                        if (l > 155.0)
                            ++lit; // fully-lit ground
                        else if (l > 20.0 && l < 145.0)
                            ++shadowed; // darkened ground (soft or hard), excl. near-black non-ground
                        if (l > 8.0 && l < 70.0)
                            ++umbra; // deep, near-umbra occlusion (excl. pure-black background)
                    }
                }
            };
            std::size_t litOn, shadowOn, umbraOn, totalOn;
            std::size_t litOff, shadowOff, umbraOff, totalOff;
            classify(softPixels, litOn, shadowOn, umbraOn, totalOn);
            classify(hardPixels, litOff, shadowOff, umbraOff, totalOff);
            ASSERT_GT(totalOn, 0u);

            // (1) Lit ground and a shadow render in both modes.
            EXPECT_GT(litOn, totalOn / 100) << poseName << ": expected lit ground in the lower frame";
            EXPECT_GT(shadowOn, totalOn / 300) << poseName << ": expected a (soft) shadow in the PCSS frame";
            EXPECT_GT(shadowOff, totalOff / 300) << poseName << ": expected a shadow in the hard-PCF frame";

            // (2) Contact-hardening / softening: PCSS lifts umbra into a lighter
            // penumbra, so its deepest-dark area is strictly smaller than hard PCF's.
            EXPECT_LT(umbraOn, umbraOff)
                << poseName << ": PCSS should spread occlusion into a lighter penumbra than hard PCF "
                << "(umbra px: PCSS=" << umbraOn << " hardPCF=" << umbraOff << ")";

            // (3) The PCSS toggle measurably changes the shadows vs legacy hardware PCF.
            const f64 rmse = Rgba8Rmse(hardPixels, softPixels);
            EXPECT_GT(rmse, 1.0)
                << poseName << ": PCSS-on frame should differ from PCSS-off (legacy PCF); RMSE=" << rmse;
        };

        const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

        // Pose A — 3/4 view from +Z, angled down: the pole's long shadow (sharp at
        // the base, soft at the tip near the camera) fills the lower frame.
        // SetPose: yaw 0 looks toward -Z, positive pitch tilts down.
        EditorCamera threeQuarter(60.0f, aspect, 0.05f, 1000.0f);
        threeQuarter.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        threeQuarter.SetPose({ 0.0f, 11.0f, 17.0f }, 0.0f, 0.55f);
        verifyPose("ThreeQuarter", threeQuarter);
        if (::testing::Test::HasFatalFailure())
            return;

        // Pose B — higher, X-offset, steeper oblique view of the same scene. A
        // different eye height / pitch / lateral offset changes the cascade
        // coverage and the on-screen projection of the penumbra, so re-checking the
        // contracts here guards against view-dependent regressions (e.g. a cascade
        // selection or projection bug that only shows from one angle).
        EditorCamera oblique(60.0f, aspect, 0.05f, 1000.0f);
        oblique.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        oblique.SetPose({ 8.0f, 14.0f, 13.0f }, 0.0f, 0.72f);
        verifyPose("Oblique", oblique);
    }
} // namespace OloEngine::Tests
