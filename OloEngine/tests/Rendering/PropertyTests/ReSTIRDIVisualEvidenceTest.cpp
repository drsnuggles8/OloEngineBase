// OLO_TEST_LAYER: integration
//
// =============================================================================
// ReSTIRDIVisualEvidenceTest.cpp — #1140.
//
// WHAT THIS TENANT CAN AND CANNOT PROVE, said first because it is the whole
// design of the file.
//
// The fixture renders through an OPENGL 4.6 context, and GL_EXT_ray_query has
// no OpenGL representation — so a ReSTIR reservoir can never be built here. The
// four shaders are not even created. That is not a gap this test works around;
// it is the single most important arm of the tier, because it is the arm every
// CI runner and every non-RT GPU takes. So this file proves the FALLBACK, and
// proves it the only way worth anything:
//
//   1. Arming the tier on a device that cannot deliver it moves NO MORE pixels
//      than the renderer moves on its own between two identical captures. This
//      matters more here than it did for #1056's shadow mask, because ReSTIR DI
//      REPLACES the deferred lighting pass's punctual and area-light loop when
//      it is live. A fallback that half-engaged would not be a missing
//      highlight; it would be an unlit scene. The measured noise floor is what
//      turns "the fallback is free" into a number.
//
//      It is phrased against a measured floor rather than against zero for the
//      reason live-verification-noise-floor.md gives: the two captures share one
//      renderer, so the second runs with more temporal history behind it.
//      Asserting a flat zero would be asserting that every temporal accumulator
//      in the engine has settled — somebody else's code — and would fail as a
//      flake read as this feature's bug.
//
//   2. The frame it is identical TO actually contains many-light direct
//      lighting. Two identical black frames would satisfy (1) perfectly, so the
//      lit floor is asserted on the same capture, and the scene is built with
//      the emitter count that would make the tier ENGAGE on a device that could
//      run it — so the request travels all the way through the settings, the
//      engagement criterion, the graph declaration and the pass.
//
//   3. The radiance texture slot is not silently sampled. The lighting shader's
//      MSAAParams.y lane must be off, which the identical-bytes result already
//      implies — and the PNGs are written so a human can look, per CLAUDE.md.
//
// THE RESAMPLED IMAGES THEMSELVES ARE NOT EVIDENCE FROM HERE. They come from a
// live editor run on the Vulkan backend, captured through the MCP diagnostics
// server, and are reported in the PR. Claiming otherwise from a GL test would be
// exactly the "green unit tests are not evidence" failure CLAUDE.md warns about.
// The ESTIMATOR's correctness is ReSTIRDIOracleTest's, measured against analytic
// and quadrature ground truth; neither this file nor that one substitutes for
// looking at a frame.
//
// Classification: integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/ReSTIRDIPass.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRDITechnique.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
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

        // MANY lights, deliberately. The engagement criterion only fires when the
        // scene has more emitters than clustered lighting samples, so a two-light
        // scene would take the BelowEngagementThreshold path and this test would
        // prove the fallback for the wrong reason — it would never have reached
        // the device check at all. 96 point lights in a grid puts the request
        // past the margin on any device.
        constexpr u32 kLightGridSide = 8;   // 8 x 8 = 64 point lights
        constexpr u32 kExtraLightRing = 32; // plus a ring, for 96 total
        constexpr f32 kLightGridExtent = 16.0f;
        constexpr f32 kLightHeight = 3.5f;

        const glm::vec3 kCameraPosition{ 0.0f, 12.0f, 20.0f };
        constexpr f32 kCameraYaw = 0.0f;
        constexpr f32 kCameraPitch = 0.5f;

        // A floor point the light grid definitely reaches, so the "the frame is
        // not black" assertion stands on lighting rather than on ambient.
        const glm::vec3 kLitReferencePoint{ 0.0f, 0.0f, 0.0f };
    } // namespace

    class ReSTIRDIVisualEvidenceTest : public RendererAttachedTest
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

            // A plain bright floor, so the luminance assertion reads lighting
            // rather than a texture.
            addPrimitive("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 44.0f, 1.0f, 44.0f },
                         { 0.82f, 0.82f, 0.84f });
            // A few blockers, so the frame has structure a human can judge in the
            // PNG — and so the live Vulkan captures reported in the PR use the
            // SAME scene rather than a different one.
            addPrimitive("BlockerA", MeshPrimitive::Cube, { -5.0f, 2.0f, 2.0f }, { 2.0f, 4.0f, 2.0f },
                         { 0.7f, 0.3f, 0.2f });
            addPrimitive("BlockerB", MeshPrimitive::Cube, { 5.0f, 1.5f, -3.0f }, { 3.0f, 3.0f, 3.0f },
                         { 0.2f, 0.5f, 0.75f });

            const auto addPointLight = [&scene](const char* name, const glm::vec3& position,
                                                const glm::vec3& colour, f32 intensity)
            {
                Entity light = scene.CreateEntity(name);
                light.GetComponent<TransformComponent>().Translation = position;
                auto& pl = light.AddComponent<PointLightComponent>();
                pl.m_Color = colour;
                pl.m_Intensity = intensity;
                pl.m_Range = 14.0f;
            };

            u32 index = 0;
            for (u32 iz = 0; iz < kLightGridSide; ++iz)
            {
                for (u32 ix = 0; ix < kLightGridSide; ++ix)
                {
                    const f32 fx = (static_cast<f32>(ix) / static_cast<f32>(kLightGridSide - 1u) - 0.5f) *
                                   2.0f * kLightGridExtent;
                    const f32 fz = (static_cast<f32>(iz) / static_cast<f32>(kLightGridSide - 1u) - 0.5f) *
                                   2.0f * kLightGridExtent;
                    // Varied colour and intensity: a grid of identical lights is
                    // the one case where a broken target function still looks
                    // right, because every candidate is interchangeable.
                    const f32 t = static_cast<f32>(index) / 63.0f;
                    addPointLight(("GridLight" + std::to_string(index)).c_str(),
                                  glm::vec3(fx, kLightHeight, fz),
                                  glm::vec3(0.4f + 0.6f * t, 0.9f - 0.4f * t, 0.3f + 0.5f * (1.0f - t)),
                                  1.5f + 2.5f * t);
                    ++index;
                }
            }
            for (u32 i = 0; i < kExtraLightRing; ++i)
            {
                const f32 angle = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(kExtraLightRing);
                addPointLight(("RingLight" + std::to_string(i)).c_str(),
                              glm::vec3(std::cos(angle) * 20.0f, 5.0f, std::sin(angle) * 20.0f),
                              glm::vec3(0.9f, 0.8f, 0.6f), 2.0f);
            }
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

            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, outPixels);
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
            // against a committed reference, because what this file asserts is
            // the equality of the two captures with EACH OTHER. A golden here
            // would be a tracked binary that moves with every unrelated lighting
            // change and would assert nothing this file does not assert more
            // tightly. See task-loop.md on not committing PNG churn.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            // FAIL, do not return quietly. This file's whole claim is that a
            // human can look at the frames afterwards; a run that silently wrote
            // nothing still reports green and the evidence simply is not there
            // when someone goes looking. It returned on `ec` and ignored
            // stbi_write_png's status before.
            if (ec)
            {
                ADD_FAILURE() << "could not create " << dir.string() << " for visual evidence: " << ec.message();
                return;
            }
            const std::string path = (dir / ("ReSTIRDI_" + poseName + ".png")).string();
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

        // A pixel "moved" when a channel changed by MORE than one 8-bit step. The
        // renderer is not bit-stable at a pixel whose value sits on a rounding
        // boundary: with nothing changed, two reference captures of this scene
        // disagreed at one pixel by exactly 1 (blue 69 vs 70) in 7 of 40 runs, and
        // the same flip landing on the armed capture alone failed the fallback
        // check in 2 of those 40 (#1484). What the check exists to catch, a bound
        // texture, a cleared target or a changed uniform, moves pixels by far more.
        static constexpr int kQuantisationSteps = 1;
        [[nodiscard]] static bool ChannelMoved(const u8 a, const u8 b)
        {
            return std::abs(static_cast<int>(a) - static_cast<int>(b)) > kQuantisationSteps;
        }

        [[nodiscard]] static std::size_t CountDifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            std::size_t differing = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                if (ChannelMoved(a[i + 0], b[i + 0]) || ChannelMoved(a[i + 1], b[i + 1]) ||
                    ChannelMoved(a[i + 2], b[i + 2]))
                    ++differing;
            }
            return differing;
        }

        // Where two frames first differ and by how much, for a failure message a
        // reader can act on without the PNGs.
        [[nodiscard]] static std::string FirstDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                if (ChannelMoved(a[i + 0], b[i + 0]) || ChannelMoved(a[i + 1], b[i + 1]) ||
                    ChannelMoved(a[i + 2], b[i + 2]))
                {
                    const std::size_t pixel = i / 4u;
                    return "first at (" + std::to_string(pixel % kWidth) + ", " + std::to_string(pixel / kWidth) +
                           "): " + std::to_string(a[i]) + "," + std::to_string(a[i + 1]) + "," +
                           std::to_string(a[i + 2]) + " vs " + std::to_string(b[i]) + "," + std::to_string(b[i + 1]) +
                           "," + std::to_string(b[i + 2]);
                }
            }
            return "none";
        }

        glm::mat4 m_CaptureViewProjection{ 1.0f };
    };

    TEST_F(ReSTIRDIVisualEvidenceTest, ArmingTheTierOnANonRTDeviceStaysInsideTheRendererNoiseFloor)
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
        const ReSTIRDISettings original = settings.ReSTIRDI;
        struct ScopedSettings
        {
            ReSTIRDISettings& Live;
            ReSTIRDISettings Original;
            ~ScopedSettings()
            {
                Live = Original;
            }
        } scopedSettings{ settings.ReSTIRDI, original };

        // Arm A — the clustered tier, explicitly off. The reference frame, and
        // the one the luminance assertion runs on.
        settings.ReSTIRDI.Enabled = false;
        std::vector<u8> clustered;
        Capture("Clustered", clustered);
        ASSERT_FALSE(clustered.empty());

        // The NOISE FLOOR and the flip are measured BRACKETED: a second reference,
        // then the armed frame, then the reference again. What moves between two
        // references is the renderer, not this feature, so the floor is the wider
        // of the two reference pairs and the armed frame is compared with the
        // closer of its two neighbours. A fallback that is not free differs from
        // both while the references agree (#1484).
        std::vector<u8> clusteredAgain;
        Capture("ClusteredRepeat", clusteredAgain);
        ASSERT_FALSE(clusteredAgain.empty());

        // Arm B — the tier ARMED. On this GL context the shaders were never
        // created, so the pass reports itself unavailable, the graph declares no
        // radiance target, the lighting shader's lane stays at zero and the
        // clustered loop runs exactly as before.
        settings.ReSTIRDI.Enabled = true;
        std::vector<u8> armed;
        Capture("ArmedOnNonRTDevice", armed);
        ASSERT_FALSE(armed.empty());

        settings.ReSTIRDI.Enabled = false;
        std::vector<u8> clusteredAfter;
        Capture("ClusteredAfterArming", clusteredAfter);
        ASSERT_FALSE(clusteredAfter.empty());

        const std::size_t noiseFloor =
            std::max(CountDifferingPixels(clustered, clusteredAgain), CountDifferingPixels(clusteredAgain, clusteredAfter));
        const std::size_t flipDifference =
            std::min(CountDifferingPixels(clusteredAgain, armed), CountDifferingPixels(armed, clusteredAfter));
        std::cout << "[ReSTIR DI fallback] renderer noise floor " << noiseFloor << " px, armed frame " << flipDifference
                  << " px from its closer reference" << std::endl;
        EXPECT_LE(flipDifference, noiseFloor)
            << "arming ReSTIR DI on a device that cannot run it moved " << flipDifference
            << " pixels, above the measured renderer noise floor of " << noiseFloor << " (reference before: "
            << FirstDifference(clusteredAgain, armed) << "; reference after: " << FirstDifference(armed, clusteredAfter)
            << ")"
            << ". The fallback is not free: something bound a texture, cleared a target, changed a uniform or "
               "declared a graph resource it should not have.";

        // The frame it is identical TO must actually be lit, or two black frames
        // would satisfy the check above perfectly.
        u32 litX = 0;
        u32 litY = 0;
        ASSERT_TRUE(ProjectWorldToPixel(kLitReferencePoint, litX, litY))
            << "the lit reference point is off screen - the camera framing changed and this test's premise is gone";
        const f64 litLuma = MeanLumaAround(clustered, litX, litY, 12u);
        EXPECT_GT(litLuma, 20.0)
            << "the reference frame is essentially black at the lit probe (luma " << litLuma
            << "), so the identity assertion above compared two frames that prove nothing";

        // And the tier must say WHY it stood down, by name — a stood-down tier
        // that reports nothing is the failure the whole seam exists to prevent.
        // ASSERTED, not guarded on. Wrapping this in `if (pass != nullptr)` made
        // the entire fallback check vanish on any build where the pass was not
        // constructed — the test still passed, having verified nothing about the
        // one behaviour it is named for. The pass is created unconditionally on
        // the deferred path, so a null here is itself the bug.
        const ReSTIRDIPass* pass = Renderer3D::GetReSTIRDIPass();
        ASSERT_NE(pass, nullptr) << "Renderer3D has no ReSTIRDIPass, so the fallback reason this test "
                                    "exists to check was never computed";
        {
            const ReSTIRDIStats& stats = pass->GetStats();
            EXPECT_FALSE(stats.Active);
            // The scene was built with far more emitters than clustered lighting
            // samples, so the reason must NOT be BelowEngagementThreshold: if it
            // were, the request never reached the device check and this test
            // proved the fallback for the wrong reason.
            EXPECT_NE(stats.Fallback, ReSTIRDIFallbackReason::BelowEngagementThreshold)
                << "the scene did not clear the engagement margin (" << stats.Engagement.CandidateLightCount
                << " candidates vs a budget of " << stats.Engagement.CandidateBudget
                << "), so this test never exercised the device fallback";
            EXPECT_NE(ToString(stats.Fallback), "unknown");
        }
    }

    // Compare one front-facing metal receiver with both direct estimators. The emitters
    // occupy disjoint solid angles and there is no intervening geometry.
    class SphereAreaReSTIRDI : public RendererAttachedTest
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        static constexpr u32 kSize = 128;
        static constexpr u32 kLightCount = 12;
        static constexpr u32 kPatchSize = 32;

        void BuildScene() override
        {
            if (!TestRenderBackend::IsVulkanProcess() || !RenderCommand::SupportsRayTracing())
                return;

            EnableRendering(kSize, kSize);
            RunInFrame([&]
                       {
                auto& rs = Renderer3D::GetRendererSettings();
                rs.Path = RenderingPath::Deferred;
                rs.ShowGrid = false;
                auto& pp = Renderer3D::GetPostProcessSettings();
                pp.SSAOEnabled = pp.GTAOEnabled = pp.SSGIEnabled = false;
                pp.ReSTIRDI = {};
                pp.ReSTIRDI.InitialCandidates = 4;
                pp.ReSTIRDI.EngagementMargin = 4;
                pp.ReSTIRDI.SpatialReuse = false;
                pp.ReSTIRDI.TemporalReuse = false;
                Renderer3D::ApplyRendererSettings();

                Entity surface = GetScene().CreateEntity("Metal receiver");
                surface.GetComponent<TransformComponent>().Scale = { 5.0f, 5.0f, 0.2f };
                auto mesh = MeshPrimitives::CreateCube();
                surface.AddComponent<MeshComponent>(mesh->GetMeshSource());
                auto& material = surface.AddComponent<MaterialComponent>().m_Material;
                material.SetBaseColorFactor({0.8f, 0.8f, 0.8f, 1.0f});
                material.SetMetallicFactor(1.0f);
                material.SetRoughnessFactor(0.5f);

                for (u32 i = 0; i < kLightCount; ++i)
                {
                    const f32 phi = glm::two_pi<f32>() * i / kLightCount;
                    Entity emitter = GetScene().CreateEntity("Sphere" + std::to_string(i));
                    emitter.GetComponent<TransformComponent>().Translation = { 0.9f * std::cos(phi), 0.9f * std::sin(phi), 3.0f };
                    auto& light = emitter.AddComponent<SphereAreaLightComponent>();
                    // Alternating spectra make the per-pixel sample noise distinguish the
                    // resolved reservoir from a silently retained deterministic light loop.
                    light.m_Color = i % 2 == 0 ? glm::vec3{1.0f, 0.25f, 0.1f} : glm::vec3{0.1f, 0.5f, 1.0f};
                    light.m_Range = 12.0f;
                    light.m_Radius = 0.10f;
                    light.m_Intensity = 0.25f;
                    light.m_CastShadows = false;
                } });
        }

        static void WriteLinearFrame(const std::vector<f32>& frame, u32 size, const char* label)
        {
            std::vector<u8> pixels(frame.size());
            for (sizet j = 0; j < frame.size(); j += 4)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    ASSERT_TRUE(std::isfinite(frame[j + c]));
                    const f32 value = std::max(0.0f, frame[j + c]);
                    pixels[j + c] = static_cast<u8>(255.0f * std::sqrt(value / (1.0f + value)));
                }
                pixels[j + 3] = 255;
            }
            const auto dir = std::filesystem::path("assets/tests/visual");
            std::filesystem::create_directories(dir);
            const auto file = dir / (TestRenderBackend::EvidenceName("SphereArea") + "Deferred_" +
                                     label + ".png");
            ASSERT_NE(stbi_write_png(file.string().c_str(), size, size, 4, pixels.data(), size * 4), 0);
        }

        // Snapshot the exact receiver, material and full-intensity sphere lights used
        // by the raster and DI captures. The black environment removes the ambient
        // contribution that the GPU measurements remove using the quarter-power arm.
        void TraceReferencePatch(const EditorCamera& camera, glm::dvec3& mean)
        {
            using namespace PathTracing;
            ReferenceSceneBuilder builder;
            builder.AddScene(GetScene(), {});
            auto scene = builder.Build({});
            ASSERT_TRUE(scene.IsBuilt());
            ASSERT_FALSE(scene.GetInstances().empty());
            ASSERT_EQ(scene.GetLights().size(), kLightCount);
            for (const auto& light : scene.GetLights())
            {
                ASSERT_EQ(light.Type, ReferenceLightType::SphereArea);
                ASSERT_FLOAT_EQ(light.Intensity, 1.0f);
            }
            const auto view = ReferenceCamera::FromViewProjection(camera.GetViewProjection(), camera.GetPosition());
            PathTracerSettings settings;
            // NEE samples all the spheres. One subsequent BSDF draw can hit an emitter,
            // completing the complementary MIS estimator. The only geometry is convex.
            settings.MaxBounces = 2;
            settings.RussianRouletteStartBounce = 0;
            settings.SamplesPerPixel = 32;
            settings.Seed = 1497;
            std::vector<f32> patch(kPatchSize * kPatchSize * 4, 1.0f);
            glm::dvec3 sum(0.0);
            for (u32 row = 0; row < kPatchSize; ++row)
            {
                for (u32 col = 0; col < kPatchSize; ++col)
                {
                    const u32 x = (kSize - kPatchSize) / 2 + col;
                    const u32 y = (kSize - kPatchSize) / 2 + row;
                    const Ray ray = view.GenerateRay((glm::vec2(x, y) + 0.5f) / static_cast<f32>(kSize));
                    glm::dvec3 pixel(0.0);
                    for (u32 sample = 0; sample < settings.SamplesPerPixel; ++sample)
                    {
                        PathSampler sampler(MakePixelSeed(x, y, settings.Seed), sample);
                        pixel += glm::dvec3(PathTracer::TracePath(scene, ray, settings, sampler));
                    }
                    pixel /= settings.SamplesPerPixel;
                    sum += pixel;
                    for (u32 c = 0; c < 3; ++c)
                        patch[(row * kPatchSize + col) * 4 + c] = static_cast<f32>(pixel[c]);
                }
            }
            mean = sum / static_cast<f64>(kPatchSize * kPatchSize);
            WriteLinearFrame(patch, kPatchSize, "CPUReferencePatch");
        }

        void Capture(const EditorCamera& camera, bool includeDI, const char* label,
                     std::vector<f32>& shaded, std::vector<f32>& resolved)
        {
            auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            auto deferred = graph->GetNode<RenderGraphNode>("DeferredLightingPass");
            ASSERT_TRUE(deferred);
            struct CaptureScope
            {
                RenderGraphPassSnapshot Snapshot;
                ~CaptureScope()
                {
                    Snapshot.Disarm();
                    Snapshot.ReleaseScratch();
                }
            } scope;
            TDoubleLinkedList<RenderGraphPassSnapshot::Request> requests;
            requests.AddTail(RenderGraphPassSnapshot::Request("Deferred output", [&]
                                                              {
                const auto target = deferred->GetTarget();
                return target ? target->GetColorAttachmentHandle(0) : RHI::NullResource; }));
            if (includeDI)
                requests.AddTail(RenderGraphPassSnapshot::Request("Resolved direct", [&]
                                                                  { return graph->ResolveTextureHandle(Renderer3D::GetReSTIRDIPass()->GetPrimaryOutputTextureHandle()); }));
            scope.Snapshot.Arm(graph.Raw(), "DeferredLightingPass", std::move(requests));
            RunEditorFrames(camera, 1);
            const auto& results = scope.Snapshot.GetResults();
            ASSERT_EQ(results.Num(), includeDI ? 2 : 1);
            for (int i = 0; i < results.Num(); ++i)
            {
                ASSERT_TRUE(results[i].Captured) << results[i].Error.ToStdString();
                auto& frame = i == 0 ? shaded : resolved;
                ReadbackRgbaFloat(results[i].Handle, kSize, kSize, frame);
                ASSERT_EQ(frame.size(), static_cast<sizet>(kSize) * kSize * 4);
                ASSERT_NO_FATAL_FAILURE(WriteLinearFrame(frame, kSize, i == 0 ? label : "DIResolved"));
            }
        }
    };

    TEST_F(SphereAreaReSTIRDI, DeferredSphereHighlightsAgreeWithActiveReservoirLighting)
    {
        if (!TestRenderBackend::IsVulkanProcess() || !RenderCommand::SupportsRayTracing())
            GTEST_SKIP() << "Requires --olo-rhi=vulkan and hardware ray queries.";
        EditorCamera camera(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetViewportSize(kSize, kSize);
        camera.SetPose({ 0.0f, 0.0f, 5.0f }, 0.0f, 0.0f);
        RunEditorFrames(camera, 8);
        std::vector<f32> dim, raster, active, resolved;
        ASSERT_NO_FATAL_FAILURE(Capture(camera, false, "RasterQuarter", dim, resolved));
        auto lights = GetScene().GetAllEntitiesWith<SphereAreaLightComponent>();
        for (auto entity : lights)
            lights.get<SphereAreaLightComponent>(entity).m_Intensity = 1.0f;
        RunEditorFrames(camera, 4);
        ASSERT_NO_FATAL_FAILURE(Capture(camera, false, "Raster", raster, resolved));

        auto* pass = Renderer3D::GetReSTIRDIPass();
        ASSERT_NE(pass, nullptr);
        EXPECT_FALSE(pass->GetStats().Active);
        auto& settings = Renderer3D::GetPostProcessSettings().ReSTIRDI;
        settings.Enabled = true;
        for (int i = 0; i < 32; ++i)
        {
            RunEditorFrames(camera, 1);
            if (pass->GetStats().Active)
                break;
        }
        ASSERT_TRUE(pass->GetStats().Active) << ToString(pass->GetStats().Fallback);
        ASSERT_NO_FATAL_FAILURE(Capture(camera, true, "DIShaded", active, resolved));
        const auto& stats = pass->GetStats();
        ASSERT_TRUE(stats.Active) << ToString(stats.Fallback);
        ASSERT_EQ(stats.SphereAreaLights, kLightCount);
        ASSERT_EQ(stats.Engagement.CandidateLightCount, kLightCount);
        ASSERT_TRUE(ReSTIRDIEngagePredicate(stats.Engagement, settings.EngagementMargin));
        ASSERT_EQ(stats.InitialCandidatesPerPixel, settings.InitialCandidates);

        // A central 32x32 patch of the flat front face. Subtract the ambient by
        // differencing two nonzero intensities (the light set stays identical).
        glm::dvec3 rasterSum(0.0), diSum(0.0);
        f64 compositionError = 0.0, clusterDifference = 0.0;
        constexpr u32 kSide = kPatchSize;
        for (u32 y = (kSize - kSide) / 2; y < (kSize + kSide) / 2; ++y)
            for (u32 x = (kSize - kSide) / 2; x < (kSize + kSide) / 2; ++x)
            {
                const sizet j = (y * kSize + x) * 4;
                ASSERT_GE(resolved[j + 3], 0.5f) << "the DI resolve fell back at " << x << ", " << y;
                for (u32 c = 0; c < 3; ++c)
                {
                    const f64 direct = (raster[j + c] - dim[j + c]) / 0.75;
                    const f64 ambient = raster[j + c] - direct;
                    const f64 di = resolved[j + c];
                    rasterSum[c] += direct;
                    diSum[c] += di;
                    compositionError += std::abs(active[j + c] - ambient - di);
                    clusterDifference += std::abs(direct - di);
                }
            }
        glm::dvec3 referenceMean{};
        ASSERT_NO_FATAL_FAILURE(TraceReferencePatch(camera, referenceMean));
        const f64 samples = kSide * kSide;
        for (u32 c = 0; c < 3; ++c)
        {
            const f64 rasterMean = rasterSum[c] / samples, diMean = diSum[c] / samples;
            RecordProperty("rasterMean" + std::to_string(c), std::to_string(rasterMean));
            RecordProperty("diMean" + std::to_string(c), std::to_string(diMean));
            RecordProperty("cpuMean" + std::to_string(c), std::to_string(referenceMean[c]));
            ASSERT_GT(referenceMean[c], 0.01);
            // Two independent integrators of the full emitter cone: compare their
            // regional mean before measuring the raster approximation.
            EXPECT_NEAR(diMean, referenceMean[c], 0.08 * referenceMean[c]);
            EXPECT_NEAR(rasterMean, referenceMean[c], 0.20 * referenceMean[c]);
            ASSERT_GT(rasterMean, 0.01);
            ASSERT_GT(diMean, 0.01);
            // At r=0.5, alpha=0.25; the source's angular radius ~0.033 gives
            // a peak ratio of 0.88. Allow the representative-point approximation
            // to differ from the sampled cone integral by 20% in this patch.
            EXPECT_NEAR(rasterMean, diMean, 0.20 * diMean);
        }
        const f64 diEnergy = diSum.r + diSum.g + diSum.b;
        RecordProperty("compositionRelativeError", std::to_string(compositionError / diEnergy));
        RecordProperty("clusterRelativeDifference", std::to_string(clusterDifference / diEnergy));
        // Adjacent frames differ in the reservoir's draws. Comparing the two
        // snapshots from the SAME frame detects both double counting and fallback.
        EXPECT_GT(clusterDifference, 0.03 * diEnergy);
        EXPECT_LT(compositionError, 0.01 * diEnergy);
    }
} // namespace OloEngine::Tests
