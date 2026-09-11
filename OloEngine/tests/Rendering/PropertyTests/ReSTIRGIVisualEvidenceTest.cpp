// OLO_TEST_LAYER: integration
//
// =============================================================================
// ReSTIRGIVisualEvidenceTest.cpp — #1169.
//
// WHAT THIS TENANT CAN AND CANNOT PROVE, said first because it is the whole
// design of the file.
//
// The fixture renders through an OPENGL 4.6 context, and GL_EXT_ray_query has no
// OpenGL representation — so a ReSTIR GI reservoir can never be built here. The
// four shaders are not even created. That is not a gap this test works around;
// it is the single most important arm of the tier, because it is the arm every
// CI runner and every non-RT GPU takes. So this file proves the FALLBACK, and
// proves it the only way worth anything:
//
//   1. Arming the tier on a device that cannot deliver it moves NO MORE pixels
//      than the renderer moves on its own between two identical captures. This
//      matters more here than it did for the DI tier, because ReSTIR GI
//      REPLACES THE WHOLE DIFFUSE AMBIENT when it is live — the probe rung, the
//      baked-lightmap rung and the sky-irradiance rung all go. A fallback that
//      half-engaged would not be a slightly different bounce; it would be a
//      scene with no indirect light at all.
//
//      It is phrased against a measured floor rather than against zero for the
//      reason live-verification-noise-floor.md gives: the two captures share one
//      renderer, so the second runs with more temporal history behind it.
//      Asserting a flat zero would be asserting that every temporal accumulator
//      in the engine has settled — somebody else's code — and would fail as a
//      flake read as this feature's bug.
//
//   2. The frame it is identical TO actually contains indirect light. Two
//      identical black frames would satisfy (1) perfectly, so a shadowed probe
//      point — a spot the direct light cannot reach, lit only by bounce and
//      ambient — is asserted non-black on the same capture.
//
//   3. SSGI IS NOT STOOD DOWN WHILE THE TIER IS INERT. This is the half of
//      #979's non-goal that a fallback can get wrong in the OTHER direction:
//      standing SSGI down is correct while ReSTIR GI owns the term and is a
//      silent loss of indirect light when it does not. The hand-off's own
//      statistics are asserted to say so.
//
// THE RESAMPLED IMAGES THEMSELVES ARE NOT EVIDENCE FROM HERE. They come from a
// live editor run on the Vulkan backend, captured through the MCP diagnostics
// server, and are reported in the PR. Claiming otherwise from a GL test would be
// exactly the "green unit tests are not evidence" failure CLAUDE.md warns about.
// The ESTIMATOR's correctness is ReSTIRGIOracleTest's, measured against dense
// hemisphere quadrature; neither this file nor that one substitutes for looking
// at a frame.
//
// Classification: integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/ReSTIRGIPass.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRGITechnique.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // Frozen wall clock, for the reason the other visual tenants freeze it:
        // the byte-comparison below is only meaningful if both captures render
        // the same instant.
        constexpr f32 kCaptureTime = 4.0f;

        const glm::vec3 kCameraPosition{ 0.0f, 6.0f, 14.0f };
        constexpr f32 kCameraYaw = 0.0f;
        constexpr f32 kCameraPitch = 0.35f;

        // A floor point INSIDE the alcove the blockers form: the point light
        // cannot reach it, so whatever luminance it has is indirect and ambient.
        // That is what makes the "not two black frames" assertion stand on the
        // term this tier owns rather than on direct lighting.
        const glm::vec3 kShadowedReferencePoint{ 0.0f, 0.2f, -1.0f };
    } // namespace

    class ReSTIRGIVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

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

            // A CORNELL-ish alcove: a white floor between two SATURATED walls, so
            // the bounce this tier estimates has a colour that could only have
            // come from a bounce. A grey box would light up the same amount from
            // ambient and prove nothing about which term produced it — which is
            // also why the live Vulkan captures reported in the PR use this same
            // scene rather than a different one.
            addPrimitive("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 24.0f, 1.0f, 24.0f },
                         { 0.85f, 0.85f, 0.86f });
            addPrimitive("RedWall", MeshPrimitive::Cube, { -4.0f, 3.0f, 0.0f }, { 0.5f, 6.0f, 10.0f },
                         { 0.85f, 0.12f, 0.10f });
            addPrimitive("GreenWall", MeshPrimitive::Cube, { 4.0f, 3.0f, 0.0f }, { 0.5f, 6.0f, 10.0f },
                         { 0.10f, 0.80f, 0.15f });
            addPrimitive("BackWall", MeshPrimitive::Cube, { 0.0f, 3.0f, -5.5f }, { 8.5f, 6.0f, 0.5f },
                         { 0.80f, 0.80f, 0.80f });
            // The occluder that makes the reference point shadowed: a slab over
            // the alcove's mouth, so direct light cannot reach the floor behind it.
            addPrimitive("Lid", MeshPrimitive::Cube, { 0.0f, 5.6f, -1.0f }, { 8.5f, 0.4f, 6.0f },
                         { 0.8f, 0.8f, 0.8f });

            // ONE bright light, outside the alcove and above it. One light rather
            // than many on purpose: this tier's engagement criterion is not a
            // count (see ReSTIRGIEngagePredicate), so a light grid would prove
            // nothing extra and would make the direct term drown the bounce.
            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 4.0f, 6.0f);
            auto& pl = light.AddComponent<PointLightComponent>();
            pl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
            pl.m_Intensity = 40.0f;
            pl.m_Range = 40.0f;
        }

        void Capture(const std::string& poseName, std::vector<u8>& outPixels)
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kCameraPosition, kCameraYaw, kCameraPitch);
            m_CaptureViewProjection = camera.GetViewProjection();

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL returns rows bottom-up; the PNG and the sampling below both treat
            // row 0 as the top.
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

            // EVIDENCE, not goldens: written every run rather than compared
            // against a committed reference, because what this file asserts is the
            // equality of the two captures with EACH OTHER. A golden here would be
            // a tracked binary that moves with every unrelated lighting change and
            // would assert nothing this file does not assert more tightly.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            // FAIL, do not return quietly. This file's whole claim is that a human
            // can look at the frames afterwards; a run that silently wrote nothing
            // still reports green and the evidence simply is not there when
            // someone goes looking.
            if (ec)
            {
                ADD_FAILURE() << "could not create " << dir.string()
                              << " for visual evidence: " << ec.message();
                return;
            }
            const std::string path = (dir / ("ReSTIRGI_" + poseName + ".png")).string();
            const int written = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                 static_cast<int>(kHeight), 4, outPixels.data(),
                                                 static_cast<int>(kWidth) * 4);
            EXPECT_NE(written, 0) << "stbi_write_png failed for " << path
                                  << " - this test's evidence was not produced";
        }

        [[nodiscard]] bool ProjectWorldToPixel(const glm::vec3& world, u32& outX, u32& outY) const
        {
            const glm::vec4 clip = m_CaptureViewProjection * glm::vec4(world, 1.0f);
            if (!(clip.w > 1.0e-4f))
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
                return false;
            const f32 u = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth);
            const f32 v = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight);
            outX = static_cast<u32>(std::clamp(u, 0.0f, static_cast<f32>(kWidth - 1u)));
            outY = static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(kHeight - 1u)));
            return true;
        }

        [[nodiscard]] static f64 MeanLuma(const std::vector<u8>& pixels, u32 x0, u32 y0, u32 x1, u32 y1)
        {
            u64 sum = 0;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sum += static_cast<u64>(pixels[idx + 0]) + pixels[idx + 1] + pixels[idx + 2];
                    ++count;
                }
            }
            return count ? static_cast<f64>(sum) / (static_cast<f64>(count) * 3.0) : 0.0;
        }

        [[nodiscard]] static f64 MeanLumaAround(const std::vector<u8>& pixels, u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = cx > halfExtent ? cx - halfExtent : 0u;
            const u32 y0 = cy > halfExtent ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent + 1u, kWidth);
            const u32 y1 = std::min(cy + halfExtent + 1u, kHeight);
            return MeanLuma(pixels, x0, y0, x1, y1);
        }

        [[nodiscard]] static std::size_t CountDifferingPixels(const std::vector<u8>& a,
                                                              const std::vector<u8>& b)
        {
            std::size_t differing = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                if (a[i + 0] != b[i + 0] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
                    ++differing;
            }
            return differing;
        }

        glm::mat4 m_CaptureViewProjection{ 1.0f };
    };

    TEST_F(ReSTIRGIVisualEvidenceTest, ArmingTheTierOnANonRTDeviceStaysInsideTheRendererNoiseFloor)
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

        auto& settings = Renderer3D::GetPostProcessSettings();
        struct ScopedSettings
        {
            ReSTIRGISettings& Live;
            ReSTIRGISettings Original;
            ~ScopedSettings()
            {
                Live = Original;
            }
        } scopedSettings{ settings.ReSTIRGI, settings.ReSTIRGI };

        // Arm A — the probe ladder, with this tier explicitly off. The reference
        // frame, and the one the luminance assertion runs on.
        settings.ReSTIRGI.Enabled = false;
        std::vector<u8> ladder;
        Capture("ProbeLadder", ladder);
        ASSERT_FALSE(ladder.empty());

        // The NOISE FLOOR: the same setting captured twice. Whatever moves here is
        // the renderer settling, not this feature, and it is what the flip below
        // has to stay inside.
        std::vector<u8> ladderAgain;
        Capture("ProbeLadderRepeat", ladderAgain);
        ASSERT_FALSE(ladderAgain.empty());
        const std::size_t noiseFloor = CountDifferingPixels(ladder, ladderAgain);

        // Arm B — the tier ARMED. On this GL context the shaders were never
        // created, so the pass reports itself unavailable, the graph declares no
        // radiance target, the lighting shader's lane stays at zero and the
        // ambient ladder runs exactly as before.
        settings.ReSTIRGI.Enabled = true;
        std::vector<u8> armed;
        Capture("ArmedOnNonRTDevice", armed);
        ASSERT_FALSE(armed.empty());

        const std::size_t flipDifference = CountDifferingPixels(ladderAgain, armed);
        EXPECT_LE(flipDifference, noiseFloor)
            << "arming ReSTIR GI on a device that cannot run it moved " << flipDifference
            << " pixels, above the measured renderer noise floor of " << noiseFloor
            << ". The fallback is not free: something bound a texture, cleared a target, changed a uniform "
               "or declared a graph resource it should not have.";

        // The frame it is identical TO must actually have indirect light in the
        // shadowed alcove, or two black frames would satisfy the check above
        // perfectly and this file would be asserting nothing.
        u32 probeX = 0;
        u32 probeY = 0;
        ASSERT_TRUE(ProjectWorldToPixel(kShadowedReferencePoint, probeX, probeY))
            << "the shadowed reference point is off screen - the camera framing changed and this test's "
               "premise is gone";
        const f64 indirectLuma = MeanLumaAround(ladder, probeX, probeY, 10u);
        EXPECT_GT(indirectLuma, 4.0)
            << "the reference frame is essentially black at the shadowed probe (luma " << indirectLuma
            << "), so the identity assertion above compared two frames that prove nothing about the term "
               "this tier owns";

        // And the tier must say WHY it stood down, by name — a stood-down tier
        // that reports nothing is the failure the whole seam exists to prevent.
        // ASSERTED, not guarded on: wrapping this in `if (pass != nullptr)` would
        // make the entire check vanish on any build where the pass was not
        // constructed, and the test would still pass having verified nothing.
        const ReSTIRGIPass* pass = Renderer3D::GetReSTIRGIPass();
        ASSERT_NE(pass, nullptr) << "Renderer3D has no ReSTIRGIPass, so the fallback reason this test "
                                    "exists to check was never computed";
        const ReSTIRGIStats& stats = pass->GetStats();
        EXPECT_FALSE(stats.Active);
        // The scene has a light, so the reason must NOT be the MEASURED
        // stand-down: if it were, the request never reached the device check and
        // this test proved the fallback for the wrong reason.
        EXPECT_NE(stats.Fallback, ReSTIRGIFallbackReason::NoIndirectSourceInScene)
            << "the scene reported no indirect source (lights=" << stats.Engagement.LightCount
            << ", emissive=" << stats.Engagement.EmissiveTriangles
            << ", environment=" << stats.Engagement.EnvironmentAvailable
            << "), so this test never exercised the device fallback";
        EXPECT_NE(ToString(stats.Fallback), "unknown");

        // THE HAND-OFF, IN THE DIRECTION A FALLBACK CAN GET WRONG. Standing SSGI
        // down is correct while this tier owns the indirect diffuse term; doing it
        // while the tier is INERT is a silent loss of indirect light that no
        // pixel comparison in this file would catch, because both arms would lose
        // it equally.
        EXPECT_TRUE(stats.Sources.DDGIAtPrimary)
            << "the probe ladder was not given the indirect diffuse term even though this tier stood down";
        EXPECT_FALSE(stats.Sources.ReSTIRGIAtPrimary);
        EXPECT_FALSE(stats.Sources.DDGIAtSecondary)
            << "the probe cache was reported read at a bounce vertex on a device that traced no bounce";
        EXPECT_EQ(stats.SSGIStoodDown, 0u)
            << "SSGI was stood down while ReSTIR GI was inert - that is indirect light nobody added";
    }
} // namespace OloEngine::Tests
