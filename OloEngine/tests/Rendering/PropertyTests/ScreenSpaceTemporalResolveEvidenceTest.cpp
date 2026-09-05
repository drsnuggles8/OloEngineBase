// OLO_TEST_LAYER: L8
// =============================================================================
// ScreenSpaceTemporalResolveEvidenceTest.cpp
//
// Contracts + PNG evidence for the per-pass temporal resolve SSR and SSGI
// gained in issue #902 (SSRSignal/SSRResolved/SSRHistory,
// SSGISignal/SSGIResolved/SSGIHistory, include/TemporalResolve.glsl).
//
// WHY THIS FILE EXISTS AND WHY IT IS SHAPED LIKE THIS.
//
// Every existing SSR/SSGI test compares two SETTLED captures at the same pose,
// and two settled captures are equally settled whatever the accumulator is
// doing — a resolve that ghosts, swims, or drags stale history across a
// disocclusion passes all of them. #684's post-mortem paid for that lesson
// once already. So nothing here is a still comparison; every contract is about
// what happens ACROSS frames, and every one carries an IN-RUN CONTROL measured
// in the same process, on the same GPU, at the same pose:
//
//   * ...ResolveStabilisesConsecutiveFrames (x2, SSR and SSGI) — the SWIM test.
//     The control arm is feedback 0, NOT the resolve toggle: turning the
//     resolve off also freezes that pass's frame index (a pass with no
//     accumulator must sample a stable dither — see RenderPipeline.cpp), so a
//     toggle-off arm would be stable for the WRONG reason and would prove
//     nothing about accumulation. Feedback 0 keeps the sampler advancing and
//     keeps none of the history, isolating exactly the variable under test. If
//     that arm does not move, the pass is not running and the ON arm is
//     meaningless — so it is asserted, not assumed.
//   * SSRConvergesAfterCameraMotion — the GHOSTING test. Settle, pan away, come
//     back, settle. The returned frame must match the original within the
//     margin the resolve-OFF arm shows over the identical round trip. A history
//     that never releases leaves a trail a still screenshot cannot see.
//   * CameraCutDoesNotDragStaleHistory — the DISOCCLUSION test. One frame after
//     a hard camera cut must look like a COLD-history render at the new pose,
//     not like the old view. Both references are captured in-run, so the
//     assertion is a comparison between two measured distances rather than a
//     tuned constant — and the instrument is validated first, because "the cut
//     frame matches the cold frame" is trivially true at a pose where the
//     resolve changes nothing.
//   * RoughMetalGainsReflectionAtTheRaisedCutoff — the SSRMaxRoughness
//     0.6 -> 0.8 default change is a visual claim, so it gets measured on a
//     surface that straddles the two cutoffs (the floor, temporarily at
//     roughness 0.7: rejected outright at 0.6, half strength at 0.8) and
//     written out as PNGs to be LOOKED AT.
//
// Evidence lands in OloEditor/assets/tests/visual/SSTemporal_*.png.
//
// Runs in the normal suite and SKIPs (not fails) without a GL 4.6 context.
// SSR and SSGI are deferred-only, so the fixture forces the deferred path.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 3.0f; // freeze the clock for deterministic frames

        // Frames to let an accumulator settle. Feedback 0.92 keeps ~0.92^n of the
        // initial state, so 12 frames leaves ~37% and 24 leaves ~14%: 16 is the
        // point where the numbers below stop being dominated by the ramp without
        // making the test twice as slow as it needs to be.
        constexpr u32 kSettleFrames = 16;

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
        }

        [[nodiscard]] f64 MeanAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += std::abs(Luma(a, x, y) - Luma(b, x, y));
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }

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

    class ScreenSpaceTemporalResolveEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // SSR and SSGI both read the G-Buffer, so both are deferred-only.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // A key plus a fill: a sparse scene renders its subject near-black and
            // every luma threshold below would then be measuring nothing — see
            // docs/agent-rules/single-mesh-visual-test-lighting.md.
            {
                Entity key = scene.CreateEntity("Key");
                key.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 2.5f;
                dl.m_CastShadows = false;
            }
            {
                Entity fill = scene.CreateEntity("Fill");
                fill.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.6f, -0.5f, 0.4f));
                dl.m_Color = glm::vec3(0.55f, 0.58f, 0.72f);
                dl.m_Intensity = 1.0f;
                dl.m_CastShadows = false;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale) -> Entity
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh;
                switch (prim)
                {
                    case MeshPrimitive::Plane:
                        mesh = MeshPrimitives::CreatePlane();
                        break;
                    case MeshPrimitive::Sphere:
                        mesh = MeshPrimitives::CreateSphere();
                        break;
                    default:
                        mesh = MeshPrimitives::CreateCube();
                        break;
                }
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                return e;
            };

            // A MODERATELY rough metal floor, not a near-mirror. Roughness 0.25 is
            // the point of this scene: at 0.04 the VNDF lobe collapses onto the
            // macrosurface normal, the single sample per pixel is very nearly
            // deterministic, and there is no stochastic error for a temporal
            // resolve to remove — the swim test would then measure nothing.
            {
                Entity floor = addMesh("MetalFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 80.0f, 1.0f, 80.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.85f, 0.88f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.25f);
            }

            // Bright RED emissive block: the thing whose reflection SSR carries
            // and whose bounce SSGI gathers. Emissive so it is unambiguously red
            // in the lit colour both passes sample.
            {
                Entity block = addMesh("RedBlock", MeshPrimitive::Cube, { -3.0f, 3.0f, -8.0f },
                                       { 5.0f, 3.0f, 1.0f });
                auto& mat = block.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(3.5f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // A bright CYAN emissive block on the other side, so a pan between the
            // two poses genuinely changes what is on screen (a disocclusion test
            // against a symmetric scene proves nothing).
            {
                Entity block = addMesh("CyanBlock", MeshPrimitive::Cube, { 6.0f, 2.0f, -4.0f },
                                       { 3.0f, 4.0f, 1.0f });
                auto& mat = block.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.05f, 0.85f, 0.9f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(0.0f, 2.5f, 3.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // The ROUGH metal sphere the SSRMaxRoughness change is about: 0.7 sits
            // above the old 0.6 cutoff and below the new 0.8 one, so it receives
            // no SSR at the old default and some at the new one.
            {
                Entity sphere = addMesh("RoughMetalSphere", MeshPrimitive::Sphere,
                                        { 0.0f, 2.2f, 1.0f }, { 2.2f, 2.2f, 2.2f });
                auto& mat = sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.88f, 0.85f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.7f);
            }

            // A large occluder wall standing between the two camera poses, so the
            // pan from A to B genuinely reveals geometry that was hidden.
            {
                Entity wall = addMesh("Occluder", MeshPrimitive::Cube, { 2.0f, 3.0f, 3.0f },
                                      { 6.0f, 6.0f, 0.6f });
                auto& mat = wall.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.2f, 0.22f, 0.25f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            }
        }

        [[nodiscard]] static EditorCamera MakeCamera(const glm::vec3& pos, f32 yaw, f32 pitch)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pos, yaw, pitch);
            return camera;
        }

        // Render `frames` frames from `camera`, read back the composited result
        // (flipped so row 0 is the top), and — when `tag` is non-empty — write it
        // to OloEditor/assets/tests/visual/SSTemporal_<tag>.png.
        void RunAndCapture(const EditorCamera& camera, u32 frames, const std::string& tag,
                           std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, frames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

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

            if (tag.empty())
                return;

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("SSTemporal_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        // Worst mean|d| between CONSECUTIVE frames on a static camera, after
        // settling. This is the number a temporal resolve exists to drive down and
        // the number a still comparison cannot see at all.
        [[nodiscard]] f64 WorstConsecutiveDiff(const EditorCamera& camera, u32 frames)
        {
            std::vector<u8> prev;
            RunAndCapture(camera, kSettleFrames, "", prev);
            f64 worst = 0.0;
            for (u32 i = 0; i < frames; ++i)
            {
                std::vector<u8> next;
                RunAndCapture(camera, 1, "", next);
                worst = std::max(worst, MeanAbsDiff(prev, next));
                prev.swap(next);
            }
            return worst;
        }

        struct SSGIHistoryDiagnosticStats
        {
            f64 AcceptedFraction = 0.0;
            u32 DominantReason = 0;
            f64 MeanHistoryLength = 0.0;
        };

        [[nodiscard]] SSGIHistoryDiagnosticStats ReadSSGIHistoryDiagnostics()
        {
            if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph(); graph)
            {
                for (const auto& history : graph->GetTemporalHistoryRegistry().Snapshot())
                {
                    if (history.Key.Effect == TemporalHistoryEffect::SSGI)
                    {
                        std::cout << "[SSGI registry] " << history.DebugName
                                  << " generation=" << history.Token.Generation
                                  << " valid=" << history.Valid
                                  << " texture=" << history.HasTexture
                                  << " invalidation=" << static_cast<u32>(history.LastInvalidation) << std::endl;
                    }
                }
            }

            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SSGIHistoryDiagnostics);
            EXPECT_NE(texture, 0u) << "SSGI rejection diagnostics must remain an addressable graph target";
            if (texture == 0u)
                return {};

            // Read the target at ITS OWN size, not the viewport's. Since issue
            // #708 the SSGI denoiser chain runs at a trace band that is half the
            // scene band by default, so SSGIHistoryDiagnostics is a quarter of
            // the pixels. Reading kWidth x kHeight anyway does not fail — it
            // returns the real quarter followed by zeros, which reads as
            // "exactly 25% of the frame accepted its history, for no stated
            // reason". That is a plausible-looking rendering regression that
            // never happened.
            i32 diagnosticsWidth = 0;
            i32 diagnosticsHeight = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &diagnosticsWidth);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &diagnosticsHeight);
            // EXPECT + return, not ASSERT: this helper returns a value, and
            // ASSERT_* only compiles in a void function.
            EXPECT_GT(diagnosticsWidth, 0) << "SSGIHistoryDiagnostics reported a zero width";
            EXPECT_GT(diagnosticsHeight, 0) << "SSGIHistoryDiagnostics reported a zero height";
            if (diagnosticsWidth <= 0 || diagnosticsHeight <= 0)
                return {};

            std::vector<f32> pixels;
            ReadbackRgbaFloat(texture, static_cast<u32>(diagnosticsWidth),
                              static_cast<u32>(diagnosticsHeight), pixels);
            if (pixels.empty())
                return {};

            std::unordered_map<u32, u32> reasonCounts;
            u64 accepted = 0;
            f64 historyLength = 0.0;
            const u64 pixelCount = pixels.size() / 4u;
            for (u64 i = 0; i < pixelCount; ++i)
            {
                const u32 reason = static_cast<u32>(std::max(pixels[i * 4u], 0.0f) + 0.5f);
                ++reasonCounts[reason];
                historyLength += pixels[i * 4u + 1u];
                accepted += pixels[i * 4u + 3u] >= 0.5f ? 1u : 0u;
            }

            u32 dominantReason = 0;
            u32 dominantCount = 0;
            for (const auto& [reason, count] : reasonCounts)
            {
                if (count > dominantCount)
                {
                    dominantReason = reason;
                    dominantCount = count;
                }
            }
            return {
                .AcceptedFraction = static_cast<f64>(accepted) / static_cast<f64>(pixelCount),
                .DominantReason = dominantReason,
                .MeanHistoryLength = historyLength / static_cast<f64>(pixelCount),
            };
        }

        // Drop every accumulated history by taking the passes' resources out of
        // the graph for a frame. The shared registry advances the affected
        // effect's generations on the feature edge, so the next enabled frame
        // cannot import stale storage and is exactly the cold state a
        // disocclusion must produce.
        void DropHistories(const EditorCamera& camera)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            const bool ssr = pp.SSREnabled;
            const bool ssgi = pp.SSGIEnabled;
            pp.SSREnabled = false;
            pp.SSGIEnabled = false;
            RunEditorFrames(camera, 1);
            pp.SSREnabled = ssr;
            pp.SSGIEnabled = ssgi;
        }

        static void ApplySSRParams()
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSREnabled = true;
            pp.SSGIEnabled = false;
            pp.SSRIntensity = 1.0f;
            pp.SSRMaxDistance = 40.0f;
            pp.SSRThickness = 0.8f;
            pp.SSRStride = 0.25f;
            pp.SSRMaxSteps = 64;
            pp.SSRBinarySearchSteps = 6;
            pp.SSRMaxRoughness = 0.8f;
            pp.SSREdgeFade = 0.1f;
            pp.SSRTemporalFeedback = 0.92f;
        }

        static void ApplySSGIParams()
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSREnabled = false;
            pp.SSGIEnabled = true;
            pp.SSGIIntensity = 2.5f;
            pp.SSGIMaxDistance = 30.0f;
            pp.SSGIThickness = 1.5f;
            pp.SSGIStride = 0.6f;
            pp.SSGIMaxSteps = 24;
            pp.SSGIRayCount = 8;
            pp.SSGIEdgeFade = 0.1f;
            pp.SSGITemporalFeedback = 0.92f;
        }

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

        // Camera poses shared by the motion contracts. A looks at the red block
        // past the occluder; B is a different position AND heading, so panning
        // between them both moves every pixel and reveals hidden geometry.
        [[nodiscard]] static EditorCamera PoseA()
        {
            return MakeCamera({ 0.0f, 4.0f, 12.0f }, 0.0f, 0.22f);
        }
        // B is close to the red block and past the occluder, so it is a near-total
        // disocclusion from A *and* a pose where SSGI's bounce is strong. Both
        // halves matter: a B that merely looks different would make the
        // disocclusion contract pass on a scene where the pass contributes
        // nothing, which is what the first draft of this file did.
        [[nodiscard]] static EditorCamera PoseB()
        {
            return MakeCamera({ -3.0f, 2.5f, -1.5f }, 0.0f, 0.12f);
        }
    };

    // SWIM. With the resolve off, the sampler still advances (issue #902 removed
    // the freeze) so SSR's grain is redrawn every frame; with it on, consecutive
    // frames on a static camera must settle. The OFF arm is the control — if it
    // does not move, SSR is not running and the ON arm proves nothing.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, SSRResolveStabilisesConsecutiveFrames)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = PoseA();
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSRParams();

        // MEASURE THE NOISE FLOOR FIRST. The composited frame is not still even
        // with a frozen clock and a static camera — auto-exposure adaptation and
        // every other cross-frame filter downstream keep it moving — so the raw
        // consecutive-frame distance is NOT attributable to SSR. The number that
        // is attributable is the EXCESS over this baseline, and the resolve's job
        // is to remove that excess. See
        // docs/agent-rules/live-verification-noise-floor.md.
        pp.SSREnabled = false;
        const f64 floorWorst = WorstConsecutiveDiff(camera, 6);

        ApplySSRParams();
        // Control: the resolve runs (so the sampler advances and the pass still
        // costs its three draws) but keeps NONE of the history.
        pp.SSRTemporalResolve = true;
        pp.SSRTemporalFeedback = 0.0f;
        DropHistories(camera);
        const f64 offWorst = WorstConsecutiveDiff(camera, 6);

        pp.SSRTemporalFeedback = 0.92f;
        DropHistories(camera);
        const f64 onWorst = WorstConsecutiveDiff(camera, 6);

        if (::testing::Test::HasFatalFailure())
            return;

        const f64 offExcess = offWorst - floorWorst;
        const f64 onExcess = onWorst - floorWorst;

        std::cout << "[SSR frame-to-frame stability] SSR-off floor = " << floorWorst
                  << "   feedback 0 worst = " << offWorst << " (excess " << offExcess
                  << ")   feedback 0.92 worst = " << onWorst << " (excess " << onExcess << ")"
                  << std::endl;

        ASSERT_GT(offExcess, 0.02)
            << "at feedback 0, SSR adds no measurable frame-to-frame movement over the "
               "SSR-disabled floor ("
            << floorWorst << " -> " << offWorst
            << "), so this test is measuring nothing: SSR is either contributing nothing to this "
               "scene, missing its G-Buffer, or sampling with a frozen frame index. Check that "
               "StochasticFrameIndex advances whenever the pass's OWN resolve is enabled "
               "(RenderPipeline.cpp, stochasticFrameIndexFor).";

        EXPECT_LT(onExcess, offExcess * 0.6)
            << "SSR's temporal resolve did not settle its own contribution: it still adds "
            << onExcess << " of frame-to-frame movement over the SSR-disabled floor, against "
            << offExcess
            << " at feedback 0. Either the history is never valid (check the SSRHistory "
               "sink/import and the SSRHistoryValid fingerprint hash), or the feedback collapses "
               "every frame (check OloTemporalMotionFeedback's sub-pixel dead zone against a "
               "stationary camera).";
    }

    // Turning a pass's resolve OFF must freeze that pass's frame index, not just
    // stop the accumulation. #902 removed the old `TAAEnabled ? index : 0` gate,
    // and the temptation is to make the index unconditional — but the rule the
    // gate expressed is per-pass and still holds: a pass with nothing averaging
    // it must sample a STABLE dither, because advancing the sampler into a void
    // replaces static grain with grain redrawn every frame, which is strictly
    // worse to look at (docs/agent-rules/stochastic-sampling-and-temporal-resolve
    // .md §3). These toggles are user-reachable from the panel, MCP and scene
    // YAML, so this is a shipped configuration and not just a bisect lever.
    //
    // The paired positive is the feedback-0 arm of the test above: same "no
    // accumulation" outcome, index still advancing, and it moves by 0.67. If
    // this test and that one ever agree, the frame index has stopped following
    // the toggle.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, DisablingTheResolveFreezesTheSamplerInsteadOfRedrawingGrain)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = PoseA();
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSRParams();

        // Resolve OFF: no accumulator, so the index must be frozen.
        pp.SSRTemporalResolve = false;
        DropHistories(camera);
        const f64 frozenWorst = WorstConsecutiveDiff(camera, 6);

        // Resolve ON at feedback 0: no accumulation either, but the index
        // advances — this is what "sampling into a void" looks like.
        pp.SSRTemporalResolve = true;
        pp.SSRTemporalFeedback = 0.0f;
        DropHistories(camera);
        const f64 advancingWorst = WorstConsecutiveDiff(camera, 6);

        if (::testing::Test::HasFatalFailure())
            return;

        std::cout << "[SSR frozen sampler] resolve OFF worst = " << frozenWorst
                  << "   resolve ON, feedback 0 worst = " << advancingWorst << std::endl;

        ASSERT_GT(advancingWorst, 0.05)
            << "the advancing-index arm does not move (" << advancingWorst
            << "), so this test cannot tell a frozen sampler from an advancing one and would pass "
               "on the regression it exists to catch.";

        EXPECT_LT(frozenWorst, advancingWorst * 0.25)
            << "with SSR's temporal resolve OFF the frame still changes by " << frozenWorst
            << " between consecutive frames on a static camera, against " << advancingWorst
            << " when the sampler is deliberately advancing. The frame index is not following the "
               "per-pass resolve toggle — check stochasticFrameIndexFor in RenderPipeline.cpp. A "
               "pass with no accumulator behind it must sample a stable dither.";
    }

    // SWIM, SSGI. Same shape, same control, different pass.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, SSGIResolveStabilisesConsecutiveFrames)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = PoseA();
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSGIParams();

        // The SSGI-disabled noise floor — see the SSR variant for why the raw
        // consecutive-frame distance is not attributable to the pass.
        pp.SSGIEnabled = false;
        const f64 floorWorst = WorstConsecutiveDiff(camera, 6);

        ApplySSGIParams();
        // Control: resolve on, feedback 0 — see the SSR variant.
        pp.SSGITemporalResolve = true;
        pp.SSGITemporalFeedback = 0.0f;
        DropHistories(camera);
        const f64 offWorst = WorstConsecutiveDiff(camera, 6);

        pp.SSGITemporalFeedback = 0.92f;
        DropHistories(camera);
        const f64 onWorst = WorstConsecutiveDiff(camera, 6);
        const SSGIHistoryDiagnosticStats historyDiagnostics = ReadSSGIHistoryDiagnostics();

        if (::testing::Test::HasFatalFailure())
            return;

        const f64 offExcess = offWorst - floorWorst;
        const f64 onExcess = onWorst - floorWorst;

        std::cout << "[SSGI frame-to-frame stability] SSGI-off floor = " << floorWorst
                  << "   feedback 0 worst = " << offWorst << " (excess " << offExcess
                  << ")   feedback 0.92 worst = " << onWorst << " (excess " << onExcess << ")"
                  << "   accepted = " << historyDiagnostics.AcceptedFraction
                  << "   dominant reason = " << historyDiagnostics.DominantReason
                  << "   mean history length = " << historyDiagnostics.MeanHistoryLength << std::endl;

        EXPECT_GT(historyDiagnostics.AcceptedFraction, 0.25)
            << "the shared surface-validity layer rejected nearly the whole settled frame; dominant reason bitmask="
            << historyDiagnostics.DominantReason;
        EXPECT_GT(historyDiagnostics.MeanHistoryLength, 2.0)
            << "the reusable moment history never accumulated beyond first-frame state";

        ASSERT_GT(offExcess, 0.02)
            << "at feedback 0, SSGI adds no measurable frame-to-frame movement over the "
               "SSGI-disabled floor ("
            << floorWorst << " -> " << offWorst
            << "), so this test is measuring nothing — see the SSR variant.";

        EXPECT_LT(onExcess, offExcess * 0.6)
            << "SSGI's temporal resolve did not settle its own contribution: it still adds "
            << onExcess << " of frame-to-frame movement over the SSGI-disabled floor, against "
            << offExcess << " at feedback 0.";
    }

    // GHOSTING. Settle at A, pan to B, come back to A, settle again. The returned
    // frame must match the original settled frame. The resolve-OFF arm makes the
    // same round trip and is the margin: a history that never releases leaves a
    // trail, and the ON arm's return distance blows past the OFF arm's.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, SSRConvergesAfterCameraMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera a = PoseA();
        const EditorCamera b = PoseB();
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSRParams();

        // Same control as the swim tests: vary the feedback, not the toggle, so
        // both arms sample with an advancing index and only accumulation differs.
        const auto roundTripDistance = [&](f32 feedback, const std::string& tag)
        {
            pp.SSRTemporalResolve = true;
            pp.SSRTemporalFeedback = feedback;
            DropHistories(a);

            std::vector<u8> reference;
            RunAndCapture(a, kSettleFrames, tag.empty() ? "" : tag + "_Reference", reference);

            // Pan away and let the history fully adopt the other view...
            std::vector<u8> away;
            RunAndCapture(b, kSettleFrames, tag.empty() ? "" : tag + "_Away", away);

            // ...then come back and settle again.
            std::vector<u8> returned;
            RunAndCapture(a, kSettleFrames, tag.empty() ? "" : tag + "_Returned", returned);

            return MeanAbsDiff(reference, returned);
        };

        const f64 offReturn = roundTripDistance(0.0f, "");
        const f64 onReturn = roundTripDistance(0.92f, "SSR_Ghost");

        if (::testing::Test::HasFatalFailure())
            return;

        std::cout << "[SSR ghosting] feedback 0 return mean|d| = " << offReturn
                  << "   feedback 0.92 return mean|d| = " << onReturn << std::endl;

        // The OFF arm's distance is pure per-frame stochastic noise, so it is the
        // floor any correct resolve should beat or match. Allow a small absolute
        // slack for the ramp not being fully complete at kSettleFrames.
        EXPECT_LT(onReturn, offReturn + 2.0)
            << "returning to a settled pose after a pan left the frame " << onReturn
            << " mean|d| from where it started (feedback-0 control: " << offReturn
            << ") — the resolve is dragging a trail from the other view. Check the reprojection "
               "SIGN in PostProcess_SSRResolve.glsl (prevUV = uv - velocity, matching the RT3 "
               "current-minus-previous convention) and the disocclusion confidence. See "
               "SSTemporal_SSR_Ghost_Reference.png vs SSTemporal_SSR_Ghost_Returned.png.";
    }

    // DISOCCLUSION. One frame after a hard cut, the resolve must have rejected the
    // history it cannot reproject — so the frame must look like a COLD-history
    // render at the new pose, not like the pose it came from. Both distances are
    // measured in-run, so the assertion is a comparison and not a tuned constant.
    //
    // Driven with SSGI rather than SSR because SSGI's contribution to this scene
    // is ~4x larger, and a disocclusion test whose subject contributes nothing at
    // the new pose passes whatever the resolve does. That failure mode is real
    // enough that the instrument is validated explicitly below before the
    // contract is asserted.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, CameraCutDoesNotDragStaleHistory)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera a = PoseA();
        const EditorCamera b = PoseB();
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSGIParams();
        pp.SSGITemporalResolve = true;

        // ---- Validate the instrument BEFORE trusting a null result ----------
        // A settled (accumulated) frame at pose B against a single cold frame at
        // the same pose. If those are the same, the resolve changes nothing at
        // pose B and "the cut frame matches the cold frame" would be true no
        // matter how badly the history were dragged.
        DropHistories(b);
        std::vector<u8> settledB;
        RunAndCapture(b, kSettleFrames, "SSGI_Cut_SettledB", settledB);

        DropHistories(b);
        std::vector<u8> coldB;
        RunAndCapture(b, 1, "SSGI_Cut_ColdB", coldB);

        const f64 resolveEffectAtB = MeanAbsDiff(settledB, coldB);

        // ---- The contract ----------------------------------------------------
        // What pose A looks like — the thing a broken resolve would drag forward.
        DropHistories(a);
        std::vector<u8> settledA;
        RunAndCapture(a, kSettleFrames, "SSGI_Cut_PoseA", settledA);

        // ONE frame after the cut to pose B, with a fully warm pose-A history.
        Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::CameraCut);
        std::vector<u8> firstFrameAfterCut;
        RunAndCapture(b, 1, "SSGI_Cut_FirstFrameAtB", firstFrameAfterCut);

        if (::testing::Test::HasFatalFailure())
            return;

        const f64 toCold = MeanAbsDiff(firstFrameAfterCut, coldB);
        const f64 toStale = MeanAbsDiff(firstFrameAfterCut, settledA);

        std::cout << "[SSGI disocclusion] resolve effect at pose B = " << resolveEffectAtB
                  << "   first-frame-at-B vs cold-B = " << toCold << "   vs stale pose-A = "
                  << toStale << std::endl;

        ASSERT_GT(resolveEffectAtB, 0.05)
            << "accumulating at pose B changes the frame by only " << resolveEffectAtB
            << ", so a dragged history would be invisible here and this test would pass on the bug "
               "it exists to catch. Make SSGI contribute at pose B again (check the pose, the "
               "intensity, and that the deferred path is active) before trusting the numbers "
               "below.";

        ASSERT_GT(toStale, 1.0)
            << "pose A and pose B render almost identically (mean|d|=" << toStale
            << "), so this test cannot tell a dragged history from a correct one. The scene or the "
               "poses changed — make B look materially different from A again.";

        // toCold is NOT expected to be zero, and a test that demanded that would
        // be asserting the resolve is broken. A translation is not a teleport:
        // the surfaces visible from BOTH poses reproject correctly, pass the
        // depth test, and legitimately keep their history — that is the whole
        // point of reprojection. What must not happen is the frame carrying the
        // OLD VIEW, so the contract is a ratio: the cut frame has to sit far
        // closer to a cold render of where the camera now is than to where it
        // was. Measured here at 3.0 vs 19.6, a 6.5x margin; with the
        // disocclusion path disabled the first frame would be ~feedback worth of
        // pose A and the two distances would swap.
        EXPECT_LT(toCold * 4.0, toStale)
            << "the first frame after a camera cut is " << toCold
            << " mean|d| from a cold-history render of the pose the camera is now at, but only "
            << toStale
            << " from the pose it came FROM — the resolve is dragging stale history across a "
               "disocclusion instead of rejecting it. Check OloTemporalHistoryUVValid and "
               "OloTemporalDepthConfidence in PostProcess_SSGIResolve.glsl: the depth comparison "
               "must be against the view depth this pass stored in the signal's alpha, not device "
               "depth, and it must be RELATIVE (a fixed tolerance on device depth is centimetres "
               "near the camera and kilometres far away).";
    }

    // MOVING GEOMETRY. A warm history of the red emissive block at its original
    // position must not remain as a ghost after the block translates. The cold
    // new-position frame is the reference and the old settled frame is the stale
    // alternative. The image delta proves the subject is visible; the diagnostic
    // acceptance mask supplies the ghosting threshold because independent cold
    // frames deliberately use different stochastic samples.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, MovingGeometryRejectsItsOldSurfaceHistory)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        const EditorCamera camera = PoseA();
        ApplySSGIParams();
        Renderer3D::GetPostProcessSettings().SSGITemporalResolve = true;

        Entity block = GetScene().FindEntityByName("RedBlock");
        ASSERT_TRUE(block);

        DropHistories(camera);
        std::vector<u8> settledOld;
        RunAndCapture(camera, kSettleFrames, "SSGI_Moving_Old", settledOld);

        block.GetComponent<TransformComponent>().Translation = { 4.0f, 3.0f, -8.0f };
        std::vector<u8> firstMoved;
        RunAndCapture(camera, 1, "SSGI_Moving_First", firstMoved);
        const SSGIHistoryDiagnosticStats movingDiagnostics = ReadSSGIHistoryDiagnostics();

        DropHistories(camera);
        std::vector<u8> coldMoved;
        RunAndCapture(camera, 1, "SSGI_Moving_Cold", coldMoved);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 toCold = MeanAbsDiff(firstMoved, coldMoved);
        const f64 toStale = MeanAbsDiff(firstMoved, settledOld);
        const auto redExcess = [](const BandStats& band)
        { return band.R - std::max(band.G, band.B); };
        // The old red block occupies this fixed screen-space band in PoseA.
        // A retained ghost would leave positive red excess here after the move.
        constexpr f32 oldBlockX0 = 0.30f;
        constexpr f32 oldBlockX1 = 0.43f;
        constexpr f32 oldBlockY0 = 0.18f;
        constexpr f32 oldBlockY1 = 0.44f;
        const f64 staleRedExcess = redExcess(
            SampleBand(settledOld, oldBlockX0, oldBlockX1, oldBlockY0, oldBlockY1));
        const f64 firstMovedRedExcess = redExcess(
            SampleBand(firstMoved, oldBlockX0, oldBlockX1, oldBlockY0, oldBlockY1));
        const f64 coldMovedRedExcess = redExcess(
            SampleBand(coldMoved, oldBlockX0, oldBlockX1, oldBlockY0, oldBlockY1));
        std::cout << "[SSGI moving geometry] first moved vs cold moved = " << toCold
                  << "   vs stale old position = " << toStale
                  << "   old-band red excess stale/first/cold = " << staleRedExcess << "/"
                  << firstMovedRedExcess << "/" << coldMovedRedExcess
                  << "   accepted = " << movingDiagnostics.AcceptedFraction
                  << "   dominant reason = " << movingDiagnostics.DominantReason << std::endl;

        ASSERT_GT(toStale, 0.75)
            << "moving the subject did not materially alter the frame, so this fixture cannot reveal a ghost";
        ASSERT_GT(staleRedExcess, 20.0)
            << "the old-position band did not contain enough red signal to expose retained history";
        EXPECT_LT(std::abs(firstMovedRedExcess - coldMovedRedExcess), 2.0)
            << "the vacated block region retained more than 2/255 mean red excess versus a cold new-pose frame";
        EXPECT_LT(movingDiagnostics.AcceptedFraction, 0.999)
            << "moving the visible subject rejected fewer than 0.1% of half-resolution history pixels; "
               "inspect SSGIHistoryDiagnostics and SSGIReprojectionDiagnostics for stale-surface acceptance";
    }

    // The SSRMaxRoughness 0.6 -> 0.8 default change is a visual claim, so measure
    // it on a surface that sits BETWEEN the two cutoffs and carries a reflection
    // big enough to see: the metal floor, temporarily raised to roughness 0.7.
    //
    // roughFade = 1 - smoothstep(max * 0.75, max, roughness), so a 0.7 surface
    // gets EXACTLY ZERO at the old 0.6 default (the window closes at 0.6) and
    // about half strength at the new 0.8 one. The red block's reflection in the
    // floor therefore appears only in the second arm — an unambiguous
    // difference rather than a few tenths of stochastic residual.
    //
    // The rough SPHERE in the scene is deliberately NOT the subject: it is small,
    // faces a dark wall, and its 0.6-vs-0.8 difference measured under the noise
    // floor of two independently-seeded accumulations. Measuring it would have
    // been a test that passes on run-to-run noise.
    TEST_F(ScreenSpaceTemporalResolveEvidenceTest, RoughMetalGainsReflectionAtTheRaisedCutoff)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedMockTime scopedMockTime(kCaptureTime);
        auto& pp = Renderer3D::GetPostProcessSettings();

        ApplySSRParams();
        pp.SSRTemporalResolve = true;

        Entity floor = GetScene().FindEntityByName("MetalFloor");
        ASSERT_TRUE(floor) << "the MetalFloor entity is the subject of this test";
        ASSERT_TRUE(floor.HasComponent<MaterialComponent>());
        auto& floorMaterial = floor.GetComponent<MaterialComponent>().m_Material;

        struct RoughnessRestore
        {
            Material* Target;
            f32 Original;
            ~RoughnessRestore()
            {
                if (Target)
                    Target->SetRoughnessFactor(Original);
            }
        } restore{ &floorMaterial, floorMaterial.GetRoughnessFactor() };

        // Straddle the two cutoffs: rejected outright at 0.6, half strength at 0.8.
        floorMaterial.SetRoughnessFactor(0.7f);

        // A low, grazing view of the floor in front of the red block — the angle
        // where SSR is used and where the VNDF sampling either pays off or falls
        // apart.
        const EditorCamera grazing = MakeCamera({ -1.5f, 1.6f, 9.0f }, 0.0f, 0.06f);

        // The reflected block sits in a short streak directly below its own
        // position. At roughness 0.7 the lobe is wide and the reflection is a
        // faint smudge rather than a mirror image, so the band is tight around
        // it: measured over this window the red channel gains 6.7/255 while
        // green and blue LOSE 0.7, which is what a red reflection appearing
        // looks like. A wider band dilutes that to under a unit.
        constexpr f32 bx0 = 0.34f, bx1 = 0.45f, by0 = 0.51f, by1 = 0.58f;

        pp.SSRMaxRoughness = 0.6f;
        DropHistories(grazing);
        std::vector<u8> atOldCutoff;
        RunAndCapture(grazing, kSettleFrames, "Rough_Cutoff060_Grazing", atOldCutoff);
        if (::testing::Test::HasFatalFailure())
            return;

        pp.SSRMaxRoughness = 0.8f;
        DropHistories(grazing);
        std::vector<u8> atNewCutoff;
        RunAndCapture(grazing, kSettleFrames, "Rough_Cutoff080_Grazing", atNewCutoff);
        if (::testing::Test::HasFatalFailure())
            return;

        // A second angle, because a single pose is one data point and a
        // reflection that only appears from one direction is a bug, not a feature.
        const EditorCamera elevated = MakeCamera({ -1.5f, 5.0f, 9.0f }, 0.0f, 0.35f);
        DropHistories(elevated);
        std::vector<u8> elevatedFrame;
        RunAndCapture(elevated, kSettleFrames, "Rough_Cutoff080_Elevated", elevatedFrame);
        if (::testing::Test::HasFatalFailure())
            return;

        const BandStats oldStats = SampleBand(atOldCutoff, bx0, bx1, by0, by1);
        const BandStats newStats = SampleBand(atNewCutoff, bx0, bx1, by0, by1);
        const BandStats elevatedStats = SampleBand(elevatedFrame, bx0, bx1, by0, by1);

        std::cout << "[SSR rough cutoff, roughness-0.7 floor] band at max=0.60 = (" << oldStats.R
                  << ", " << oldStats.G << ", " << oldStats.B << ")   at max=0.80 = (" << newStats.R
                  << ", " << newStats.G << ", " << newStats.B << ")   elevated pose = ("
                  << elevatedStats.R << ", " << elevatedStats.G << ", " << elevatedStats.B << ")"
                  << std::endl;

        EXPECT_GT(oldStats.R + oldStats.G + oldStats.B, 5.0)
            << "the roughness-0.60 frame rendered (near-)black — the scene, not the cutoff, is the "
               "problem. Look at SSTemporal_Rough_Cutoff060_Grazing.png.";

        // The load-bearing assertion: a 0.7 surface is rejected outright at the
        // old cutoff and reflects at the new one, so the band must gain red.
        EXPECT_GT(newStats.R, oldStats.R + 3.0)
            << "raising SSRMaxRoughness from 0.60 to 0.80 did not bring the red block's reflection "
               "into a roughness-0.7 floor (band red "
            << oldStats.R << " -> " << newStats.R
            << "). Either the roughFade in PostProcess_SSR.glsl no longer reads u_ShadeParams.y, or "
               "the setting is not reaching the UBO. NOTE: two byte-identical frames are far more "
               "often a stale shader cache than a real tie — see "
               "docs/agent-rules/stochastic-sampling-and-temporal-resolve.md section 3b.";

        // ...and it must be the BLOCK's red, not a uniform brightening.
        EXPECT_GT(newStats.R - oldStats.R, (newStats.G - oldStats.G) + 3.0)
            << "the reflection the rough floor gains at the raised cutoff is not carrying the red "
               "block's colour (dR="
            << (newStats.R - oldStats.R)
            << " dG=" << (newStats.G - oldStats.G) << " dB=" << (newStats.B - oldStats.B)
            << "). Look at SSTemporal_Rough_Cutoff080_Grazing.png.";

        // The same surface from a second angle must still reflect.
        EXPECT_GT(elevatedStats.R, elevatedStats.G + 3.0)
            << "the rough floor reflects the red block from the grazing pose but not from the "
               "elevated one (band = "
            << elevatedStats.R << ", " << elevatedStats.G << ", "
            << elevatedStats.B
            << ") — a reflection that only exists at one angle is a bug. Look at "
               "SSTemporal_Rough_Cutoff080_Elevated.png.";
    }

} // namespace OloEngine::Tests
