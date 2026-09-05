// OLO_TEST_LAYER: L8
// =============================================================================
// SSGIDenoiserVisualEvidenceTest.cpp
//
// The three acceptance criteria of issue #708, each as a measured, repeatable
// GPU contract with PNG evidence written to
//   OloEditor/assets/tests/visual/SSGIDenoise_<tag>.png
//
//   1. HalfRayCountDenoisedIsQuieter — SSGI at HALF the ray count with the
//      denoiser chain on must be perceptually at least as good as the full ray
//      count without it. Measured as the spatial noise (local standard
//      deviation of luminance) of a flat lit floor patch, which is pure
//      estimator noise on a surface that should be perfectly smooth.
//   2. ContactDetailSurvivesTheChain — the darkening where an object meets the
//      floor is the detail a naive blur destroys, so the chain must keep most
//      of it. Measured as the contrast between a band at the contact and a band
//      away from it, from two poses.
//   3. ScriptedCameraPathLeavesNoGhost — a SCRIPTED pan (not a hand-flown one,
//      so it is repeatable) followed by a settle must land on the same image as
//      settling at the destination pose from a dropped history. Any lingering
//      difference IS the ghost.
//
// All three are differential and golden-free, so they hold across GPUs and need
// no committed reference image — the same design as SSGIVisualEvidenceTest,
// which covers the colour-bleed contract this file deliberately does not repeat.
//
// The cheap denoiser MATH contracts (plane/normal weights, the radius guides,
// the quad ray distribution) live in ScreenSpaceDenoiseMathTest.cpp.
//
// Runs in the normal suite and SKIPs (not fails) without a GL 4.6 context.
// SSGI is deferred-only, so the fixture forces the deferred render path.
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
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
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

        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 600;
        constexpr f32 kCaptureTime = 4.0f; // freeze the clock for deterministic frames

        // Frames to let the temporal resolve converge before measuring. At the
        // default feedback of 0.92 the initial frame's weight is 0.92^24 ~ 0.14,
        // so 24 frames leaves the estimate dominated by accumulated samples
        // rather than by whatever the history started from. Measuring earlier
        // would compare two arms at different points on their convergence curve
        // and attribute the difference to the denoiser.
        constexpr u32 kSettleFrames = 24;

        [[nodiscard]] f64 Luminance(const u8* px)
        {
            return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
        }

        // Mean luminance over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] f64 BandMean(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
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
                    sum += Luminance(&px[idx]);
                    ++count;
                }
            }
            return count > 0 ? sum / static_cast<f64>(count) : 0.0;
        }

        // Spatial noise: the mean over the band of |pixel - mean(3x3 neighbourhood)|.
        //
        // A LOCAL residual, not the band's global standard deviation, and the
        // difference matters. A global deviation counts the scene's own smooth
        // shading gradient — the floor really is brighter near the light — and
        // would report a denoiser that flattened that gradient as an
        // improvement. The high-pass residual below sees only what varies
        // pixel-to-pixel, which on a flat lit surface is estimator noise and
        // nothing else.
        [[nodiscard]] f64 BandLocalNoise(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = std::max(1u, static_cast<u32>(x0 * kWidth));
            const u32 ix1 = std::min(kWidth - 1u, static_cast<u32>(x1 * kWidth));
            const u32 iy0 = std::max(1u, static_cast<u32>(y0 * kHeight));
            const u32 iy1 = std::min(kHeight - 1u, static_cast<u32>(y1 * kHeight));
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    f64 neighbourhood = 0.0;
                    for (i32 dy = -1; dy <= 1; ++dy)
                    {
                        for (i32 dx = -1; dx <= 1; ++dx)
                        {
                            const std::size_t idx =
                                (static_cast<std::size_t>(static_cast<i32>(y) + dy) * kWidth +
                                 static_cast<std::size_t>(static_cast<i32>(x) + dx)) *
                                4u;
                            neighbourhood += Luminance(&px[idx]);
                        }
                    }
                    const std::size_t center = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sum += std::abs(Luminance(&px[center]) - neighbourhood / 9.0);
                    ++count;
                }
            }
            return count > 0 ? sum / static_cast<f64>(count) : 0.0;
        }

        // Mean absolute luminance difference between two frames of equal size.
        [[nodiscard]] f64 FrameDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::infinity();
            f64 sum = 0.0;
            const std::size_t pixels = a.size() / 4u;
            for (std::size_t i = 0; i < pixels; ++i)
                sum += std::abs(Luminance(&a[i * 4u]) - Luminance(&b[i * 4u]));
            return sum / static_cast<f64>(pixels);
        }
    } // namespace

    class SSGIDenoiserVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.85f, 0.3f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.2f;
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

            // Neutral diffuse floor — the flat surface the noise metric reads.
            {
                Entity floor = addMesh("WhiteFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.85f, 0.85f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // Bright emissive wall — the indirect light source, so the floor
            // carries a real SSGI signal rather than a near-zero one whose noise
            // would be meaningless.
            {
                Entity wall = addMesh("RedWall", MeshPrimitive::Cube, { 0.0f, 5.0f, -9.0f },
                                      { 40.0f, 10.0f, 1.0f });
                auto& mat = wall.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(4.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // A box standing ON the floor, well in front of the wall. Where it
            // meets the floor the indirect light is occluded, and that narrow
            // dark contact band is exactly the detail a fixed wide blur wipes
            // out — criterion 2 measures whether the chain keeps it.
            {
                Entity box = addMesh("ContactBox", MeshPrimitive::Cube, { 0.0f, 1.0f, -2.0f },
                                     { 2.0f, 2.0f, 2.0f });
                auto& mat = box.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        // The SSGI trace settings both arms share. Only the ray count and the
        // denoiser knobs differ between arms, so any measured difference is the
        // denoiser and not a different trace.
        static void ApplySharedTraceSettings()
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSGIEnabled = true;
            pp.SSGIIntensity = 2.5f;
            pp.SSGIMaxDistance = 30.0f;
            pp.SSGIThickness = 1.5f;
            pp.SSGIStride = 0.6f;
            pp.SSGIMaxSteps = 24;
            pp.SSGIEdgeFade = 0.1f;
            pp.SSGITemporalResolve = true;
        }

        // The pre-#708 arm: full ray count, no spatial stages, no half
        // resolution, no ray distribution. This is literally the trace ->
        // resolve -> composite pass as it shipped, reached through settings.
        static void ApplyBaselineArm(i32 rayCount)
        {
            ApplySharedTraceSettings();
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSGIRayCount = rayCount;
            pp.SSGIHalfResolution = false;
            pp.SSGIRayDistribution = false;
            pp.SSGIPreBlurRadius = 0.0f;
            pp.SSGIPostBlurRadius = 0.0f;
        }

        // The #708 arm: the whole chain, at whatever ray count the caller asks.
        static void ApplyDenoisedArm(i32 rayCount)
        {
            ApplySharedTraceSettings();
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSGIRayCount = rayCount;
            pp.SSGIHalfResolution = true;
            pp.SSGIRayDistribution = true;
            pp.SSGIPreBlurRadius = 1.0f;
            pp.SSGIPostBlurRadius = 4.0f;
        }

        // Drop every SSGI temporal history so the next capture starts from a
        // known state. Toggling the feature is what the pipeline already treats
        // as an invalidation (see RenderPipeline.cpp), so this needs no test-only
        // back door into the history registry.
        void DropHistories(const EditorCamera& camera)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            const bool wasEnabled = pp.SSGIEnabled;
            pp.SSGIEnabled = false;
            RunEditorFrames(camera, 1);
            pp.SSGIEnabled = wasEnabled;
        }

        [[nodiscard]] static EditorCamera MakeCamera(const glm::vec3& position, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            return camera;
        }

        // Read back the composited frame (top-down rows) and save it as PNG
        // evidence. Does NOT render — the caller decides how many frames ran and
        // from which poses, because that sequence is the experiment.
        void CaptureCurrentFrame(const std::string& tag, std::vector<u8>& outPixels)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

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

            const std::string path = (dir / ("SSGIDenoise_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        // Settle at one pose from a dropped history, then capture.
        void SettleAndCapture(const EditorCamera& camera, const std::string& tag,
                              std::vector<u8>& outPixels)
        {
            DropHistories(camera);
            RunEditorFrames(camera, kSettleFrames);
            CaptureCurrentFrame(tag, outPixels);
        }
    };

    // ---- Criterion 1 --------------------------------------------------------
    // Half the rays with the chain, against the full ray count without it.
    TEST_F(SSGIDenoiserVisualEvidenceTest, HalfRayCountDenoisedIsQuieter)
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

        const EditorCamera camera = MakeCamera({ 0.0f, 6.0f, 7.0f }, 0.0f, 0.45f);

        // A flat, lit, unoccluded stretch of floor to the left of the box, well
        // clear of the box's contact band and of the screen edges where the
        // trace fades out.
        constexpr f32 nx0 = 0.06f, nx1 = 0.30f, ny0 = 0.58f, ny1 = 0.80f;

        ApplyBaselineArm(8);
        std::vector<u8> baseline;
        SettleAndCapture(camera, "Baseline_8rays_NoChain", baseline);
        if (::testing::Test::HasFatalFailure())
            return;

        ApplyDenoisedArm(4);
        std::vector<u8> denoised;
        SettleAndCapture(camera, "Chain_4rays", denoised);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 baselineMean = BandMean(baseline, nx0, nx1, ny0, ny1);
        const f64 denoisedMean = BandMean(denoised, nx0, nx1, ny0, ny1);
        const f64 baselineNoise = BandLocalNoise(baseline, nx0, nx1, ny0, ny1);
        const f64 denoisedNoise = BandLocalNoise(denoised, nx0, nx1, ny0, ny1);

        // Printed, not just asserted: the acceptance criterion for issue #708 is
        // a MEASURED comparison, and a green test that reports no number cannot
        // be quoted as evidence for it.
        std::cout << "[SSGI denoiser A/B] 8 rays no chain: mean " << baselineMean << " noise "
                  << baselineNoise << "   |   4 rays with chain: mean " << denoisedMean << " noise "
                  << denoisedNoise << "   |   noise ratio "
                  << (baselineNoise > 0.0 ? denoisedNoise / baselineNoise : 0.0) << std::endl;

        // Catch a black / failed render before interpreting any ratio.
        EXPECT_GT(baselineMean, 5.0) << "baseline frame rendered (near-)black";
        EXPECT_GT(denoisedMean, 5.0) << "denoised frame rendered (near-)black";

        // The core criterion: half the rays, quieter image.
        EXPECT_LT(denoisedNoise, baselineNoise)
            << "SSGI at 4 rays with the #708 chain is noisier than 8 rays without it "
               "(baseline="
            << baselineNoise << " denoised=" << denoisedNoise
            << "). See SSGIDenoise_Baseline_8rays_NoChain.png / SSGIDenoise_Chain_4rays.png";

        // ENERGY, not just smoothness. A filter that simply darkened everything
        // would pass the noise test and be a regression, so the mean bounce has
        // to survive: the denoised arm must stay within 25% of the baseline's
        // brightness on the same patch.
        EXPECT_GT(denoisedMean, baselineMean * 0.75)
            << "the denoiser lost indirect energy (baseline=" << baselineMean
            << " denoised=" << denoisedMean << ")";
        EXPECT_LT(denoisedMean, baselineMean * 1.25)
            << "the denoiser gained indirect energy (baseline=" << baselineMean
            << " denoised=" << denoisedMean << ")";
    }

    // ---- Criterion 2 --------------------------------------------------------
    // The contact darkening under the box is the detail a naive blur destroys.
    TEST_F(SSGIDenoiserVisualEvidenceTest, ContactDetailSurvivesTheChain)
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

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
            // A band hugging the box's base, and one on open floor beside it.
            f32 ContactX0, ContactX1, ContactY0, ContactY1;
            f32 OpenX0, OpenX1, OpenY0, OpenY1;
        };

        // The bands were READ OFF the rendered frames (a per-column luminance
        // profile of SSGIDenoise_ContactRaw_*.png), not guessed from the scene
        // layout. The first attempt guessed, landed the "contact" band on the
        // box's own front face instead of the floor beside it, and measured a
        // contrast of 0.29 where the real one is ~7 — which would have made the
        // preservation ratio meaningless in either direction.
        const std::array<Pose, 2> poses = { {
            { "Frontal", { 0.0f, 6.0f, 7.0f }, 0.0f, 0.45f, 0.45f, 0.55f, 0.68f, 0.72f, 0.15f, 0.30f, 0.68f, 0.72f },
            { "Oblique", { 5.0f, 4.5f, 8.0f }, -0.35f, 0.35f, 0.33f, 0.45f, 0.56f, 0.60f, 0.55f, 0.70f, 0.56f, 0.60f },
        } };

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);
            const EditorCamera camera = MakeCamera(pose.Position, pose.Yaw, pose.Pitch);

            // BOTH arms at the SAME ray count here — this criterion is about
            // what the filter does to detail, not about the ray budget.
            ApplyBaselineArm(8);
            std::vector<u8> raw;
            SettleAndCapture(camera, std::string("ContactRaw_") + pose.Name, raw);
            if (::testing::Test::HasFatalFailure())
                return;

            ApplyDenoisedArm(8);
            std::vector<u8> filtered;
            SettleAndCapture(camera, std::string("ContactChain_") + pose.Name, filtered);
            if (::testing::Test::HasFatalFailure())
                return;

            const f64 rawContrast = BandMean(raw, pose.OpenX0, pose.OpenX1, pose.OpenY0, pose.OpenY1) -
                                    BandMean(raw, pose.ContactX0, pose.ContactX1, pose.ContactY0,
                                             pose.ContactY1);
            const f64 filteredContrast =
                BandMean(filtered, pose.OpenX0, pose.OpenX1, pose.OpenY0, pose.OpenY1) -
                BandMean(filtered, pose.ContactX0, pose.ContactX1, pose.ContactY0, pose.ContactY1);

            std::cout << "[SSGI contact detail " << pose.Name << "] raw contrast " << rawContrast
                      << "   chain contrast " << filteredContrast << "   preserved "
                      << (rawContrast > 0.0 ? filteredContrast / rawContrast : 0.0) << std::endl;

            // The scene must actually HAVE a contact darkening, or the test
            // below is vacuously true and would keep passing after a regression
            // that removed the effect entirely.
            ASSERT_GT(rawContrast, 2.0)
                << "the unfiltered arm shows no contact darkening to preserve — the band "
                   "coordinates or the scene have drifted. See SSGIDenoise_ContactRaw_"
                << pose.Name << ".png";

            // The chain is depth- and normal-guided precisely so it does not
            // blur across this boundary. 70% is the threshold: a fixed
            // unguided blur of the same radius collapses it far below that.
            EXPECT_GT(filteredContrast, rawContrast * 0.70)
                << "the denoiser chain washed out the contact detail (raw=" << rawContrast
                << " filtered=" << filteredContrast << "). See SSGIDenoise_ContactRaw_" << pose.Name
                << ".png / SSGIDenoise_ContactChain_" << pose.Name << ".png";
        }
    }

    // ---- Criterion 3 --------------------------------------------------------
    // Ghosting, on a SCRIPTED path so the result is repeatable.
    TEST_F(SSGIDenoiserVisualEvidenceTest, ScriptedCameraPathLeavesNoGhost)
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

        ApplyDenoisedArm(4);

        const glm::vec3 startPosition(-9.0f, 5.0f, 9.0f);
        const glm::vec3 endPosition(0.0f, 6.0f, 7.0f);
        constexpr f32 startYaw = 0.6f;
        constexpr f32 endYaw = 0.0f;
        constexpr f32 startPitch = 0.35f;
        constexpr f32 endPitch = 0.45f;
        constexpr u32 kPanFrames = 12; // ~0.2 s of hard motion at 60 Hz

        // ARM A — settle at the START pose, pan hard to the destination, then
        // hold there and let it settle again. If disocclusion rejection is
        // broken, this frame still carries content dragged in from the pan.
        const EditorCamera startCamera = MakeCamera(startPosition, startYaw, startPitch);
        DropHistories(startCamera);
        RunEditorFrames(startCamera, kSettleFrames);

        for (u32 i = 1; i <= kPanFrames; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kPanFrames);
            const EditorCamera step = MakeCamera(glm::mix(startPosition, endPosition, t),
                                                 glm::mix(startYaw, endYaw, t),
                                                 glm::mix(startPitch, endPitch, t));
            RunEditorFrames(step, 1);
        }

        const EditorCamera endCamera = MakeCamera(endPosition, endYaw, endPitch);
        RunEditorFrames(endCamera, kSettleFrames);
        std::vector<u8> afterPan;
        CaptureCurrentFrame("Ghost_AfterPan", afterPan);
        if (::testing::Test::HasFatalFailure())
            return;

        // ARM B — the SAME destination pose, settled from a dropped history, so
        // it can contain nothing from the pan. This is the reference the ghost
        // is measured against; there is no golden image involved.
        std::vector<u8> reference;
        SettleAndCapture(endCamera, "Ghost_Reference", reference);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 difference = FrameDifference(afterPan, reference);
        std::cout << "[SSGI ghosting] mean |luminance| difference after a scripted pan vs a "
                     "history-free settle at the same pose: "
                  << difference << std::endl;

        // Both arms converge to the same estimate, so what is left is residual
        // stochastic disagreement plus any ghost. 2.0 of 255 mean absolute
        // luminance is a little under 1% — comfortably above the estimator's own
        // residual at these settings and far below a visible smear, which drags
        // whole silhouettes and lands in the tens.
        EXPECT_LT(difference, 2.0)
            << "the frame after a scripted pan differs from the same pose settled with no history "
               "by "
            << difference
            << " mean absolute luminance — history from the pan is still on screen. "
               "See SSGIDenoise_Ghost_AfterPan.png / SSGIDenoise_Ghost_Reference.png";
    }
} // namespace OloEngine::Tests
