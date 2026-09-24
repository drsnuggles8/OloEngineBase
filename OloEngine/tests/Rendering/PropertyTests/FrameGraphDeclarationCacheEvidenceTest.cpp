// OLO_TEST_LAYER: integration

// =============================================================================
// Render-graph declaration cache, end to end (issue #1333).
//
// PopulateBlackboard and BuildFrameGraph are served from a cache while the
// declaration configuration's key holds, so a declaration input missing from
// the key renders a stale graph with no error, and an execution-only input
// present in it rebuilds the graph on frames that declare nothing new. The CPU
// tests (RenderGraphFingerprintTest) pin the key; these drive the real
// pipeline through the sequences where it has gone wrong before and compare the
// cache against a forced rebuild:
//
//   * CachedFramesMatchAForcedRebuild: after each operation (feature toggles,
//     a resize, MSAA, upscale, a path switch, a scene reload), a frame served
//     from the cache and a frame compiled from scratch must be the same image,
//     on the composite AND on the intermediate targets. The rebuild is the
//     verify lever (OLO_RG_VERIFY_DECLARATION_CACHE) applied to one frame. A
//     control pair of two CACHED frames measures the frame-to-frame noise
//     floor first, so a difference is attributed rather than assumed.
//   * VerifyModeFindsNoStaleCacheAcrossSequences: the same operations, plus a
//     shader hot reload, with the verifier on for every frame. Each cache hit
//     is rebuilt and its compiled plan (declarations, descriptors, imported
//     identities) compared with the cached one; any difference is a
//     stale-cache detection naming the passes and resources that differ.
//   * AHistoryInvalidatedWhileInvalidComesBackOnACachedGraph: history
//     generations are execution data and stay out of the key, so the sinks
//     that latched a token when the blackboard was populated must follow later
//     invalidations themselves, or continuous motion under an accumulating
//     history leaves it invalid for the rest of the session.
//   * CameraAndObjectMotionDoNotRecompile: motion is execution data. Sixty
//     frames of it must be sixty cache hits, with the CPU cost of a hit and of
//     a compile measured and reported.
//   * ARepopulatedBlackboardNeverServesACachedGraph: the two cache layers
//     share a key but can disagree, and a re-populated blackboard under a
//     cached graph hands every pass stale handles. Deferred + SSR + GTAO under
//     upscale toggles, with the verifier OFF because it hides this.
//
// Evidence PNGs: DeclarationCache_GL_<Path>.png (a cached frame) and
// DeclarationCacheRebuilt_GL_<Path>.png (the forced rebuild of the next
// frame). The backend is named on purpose: every Vulkan cell of this matrix is
// live-only and is not produced here.
//
// Every test SKIPs without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 320u;
        constexpr u32 kHeight = 180u;

        struct PixelDiff
        {
            u32 Pixels = 0;
            u32 MaxDelta = 0;
        };

        [[nodiscard]] PixelDiff Compare(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            PixelDiff out;
            if (a.size() != b.size())
            {
                out.Pixels = ~0u;
                out.MaxDelta = 255u;
                return out;
            }
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                u32 delta = 0;
                for (sizet c = 0; c < 4; ++c)
                    delta = std::max<u32>(delta, static_cast<u32>(std::abs(static_cast<i32>(a[i + c]) - static_cast<i32>(b[i + c]))));
                if (delta != 0u)
                {
                    ++out.Pixels;
                    out.MaxDelta = std::max(out.MaxDelta, delta);
                }
            }
            return out;
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
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

        // Turns the verifier on for a scope and always turns it back off: a
        // leaked lever would make every later test in the process rebuild its
        // graph every frame.
        class ScopedVerifyLever
        {
          public:
            explicit ScopedVerifyLever(bool on) : m_Previous(Levers::VerifyDeclarationCache())
            {
                Levers::SetVerifyDeclarationCache(on);
            }
            ~ScopedVerifyLever()
            {
                Levers::SetVerifyDeclarationCache(m_Previous);
            }
            ScopedVerifyLever(const ScopedVerifyLever&) = delete;
            ScopedVerifyLever& operator=(const ScopedVerifyLever&) = delete;

          private:
            bool m_Previous;
        };

        struct Operation
        {
            std::string Name;
            std::function<void()> Apply;
        };
    } // namespace

    class FrameGraphDeclarationCacheEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            PopulateScene();
        }

        // Also the "scene reload": every entity is destroyed and rebuilt, so the
        // GPU scene's records, the draw submission and every per-entity
        // resource are new while the configuration is the same.
        void PopulateScene()
        {
            Scene& scene = GetScene();
            {
                Entity light = scene.CreateEntity("Sun");
                light.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& sun = light.AddComponent<DirectionalLightComponent>();
                sun.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                sun.m_Color = glm::vec3(1.0f);
                sun.m_Intensity = 2.5f;
            }
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& transform = ground.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, -1.0f, 0.0f };
                transform.Scale = { 30.0f, 1.0f, 30.0f };
                auto& mesh = ground.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                    mesh.m_MeshSource = plane->GetMeshSource();
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.45f, 0.45f, 0.48f, 1.0f));
            }
            const std::array<glm::vec3, 3> positions{ glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.5f, -1.5f),
                                                      glm::vec3(3.0f, 0.5f, 0.5f) };
            const std::array<glm::vec4, 3> colors{ glm::vec4(0.8f, 0.2f, 0.15f, 1.0f), glm::vec4(0.2f, 0.7f, 0.25f, 1.0f),
                                                   glm::vec4(0.2f, 0.3f, 0.85f, 1.0f) };
            for (u32 i = 0; i < positions.size(); ++i)
            {
                Entity cube = scene.CreateEntity(i == 0 ? "Mover" : "Cube");
                auto& transform = cube.GetComponent<TransformComponent>();
                transform.Translation = positions[i];
                transform.Scale = glm::vec3(1.5f);
                auto& mesh = cube.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> box = MeshPrimitives::CreateCube())
                    mesh.m_MeshSource = box->GetMeshSource();
                cube.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(colors[i]);
                if (i == 0)
                    m_Mover = cube;
            }
        }

        void ReloadScene()
        {
            Scene& scene = GetScene();
            std::vector<Entity> entities;
            for (const auto entity : scene.GetAllEntitiesWith<TransformComponent>())
                entities.emplace_back(entity, &scene);
            for (Entity entity : entities)
                scene.DestroyEntity(entity);
            m_Mover = {};
            PopulateScene();
        }

        [[nodiscard]] EditorCamera MakeCamera() const
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 400.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 4.0f, 12.0f }, 0.0f, 0.3f);
            return camera;
        }

        // Temporal effects accumulate across frames, so two consecutive frames
        // of a converged TAA or motion blur are not the same image and a
        // cached/rebuilt comparison could not tell a cache fault from
        // accumulation. Everything else stays at its defaults.
        static void UseDeterministicPost()
        {
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = false;
            post.MotionBlurEnabled = false;
        }

        static void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        // The operations every sequence walks, per path. Each is followed by
        // settle frames before anything is compared.
        [[nodiscard]] std::vector<Operation> Operations(RenderingPath path)
        {
            std::vector<Operation> ops;
            ops.push_back({ "baseline", [] {} });
            ops.push_back({ "bloom on", []
                            { Renderer3D::GetPostProcessSettings().BloomEnabled = true; } });
            ops.push_back({ "FXAA on", []
                            { Renderer3D::GetPostProcessSettings().FXAAEnabled = true; } });
            ops.push_back({ "GTAO on", []
                            {
                                auto& post = Renderer3D::GetPostProcessSettings();
                                post.ActiveAOTechnique = AOTechnique::GTAO;
                                post.GTAOEnabled = true;
                                post.GTAODenoiseEnabled = false;
                                Renderer3D::ApplyRendererSettings();
                            } });
            // #1333, U3: the gate the old fingerprint did not hash.
            ops.push_back({ "GTAO denoise on", []
                            {
                                auto& post = Renderer3D::GetPostProcessSettings();
                                post.GTAODenoiseEnabled = true;
                                post.GTAODenoisePasses = std::max(post.GTAODenoisePasses, 1);
                            } });
            ops.push_back({ "vignette + colour grading on", []
                            {
                                auto& post = Renderer3D::GetPostProcessSettings();
                                post.VignetteEnabled = true;
                                post.ColorGradingEnabled = true;
                            } });
            ops.push_back({ "resize", [this]
                            { ResizeRenderTarget(kWidth + 64u, kHeight + 36u); } });
            ops.push_back({ "resize back", [this]
                            { ResizeRenderTarget(kWidth, kHeight); } });
            if (path == RenderingPath::Deferred)
            {
                ops.push_back({ "MSAA 4", []
                                {
                                    Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 4u;
                                    Renderer3D::ApplyRendererSettings();
                                } });
                ops.push_back({ "MSAA 1", []
                                {
                                    Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 1u;
                                    Renderer3D::ApplyRendererSettings();
                                } });
            }
            // #1333, U1: the display size and the scene band move separately.
            ops.push_back({ "upscale performance", []
                            { Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Performance; } });
            ops.push_back({ "resize under upscale", [this]
                            { ResizeRenderTarget(kWidth + 1u, kHeight); } });
            ops.push_back({ "upscale off", [this]
                            {
                                Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
                                ResizeRenderTarget(kWidth, kHeight);
                            } });
            ops.push_back({ "scene reload", [this]
                            { ReloadScene(); } });
            ops.push_back({ "features off", []
                            {
                                auto& post = Renderer3D::GetPostProcessSettings();
                                post.BloomEnabled = false;
                                post.FXAAEnabled = false;
                                post.VignetteEnabled = false;
                                post.ColorGradingEnabled = false;
                                post.GTAODenoiseEnabled = false;
                            } });
            return ops;
        }

        struct Capture
        {
            std::vector<u8> Composite;
            std::vector<std::pair<std::string, std::vector<u8>>> Intermediates;
        };

        // The composite plus the intermediate targets the path has: the lit
        // scene colour, the tone-mapped image and, on deferred, the resolved
        // G-Buffer albedo. A stale graph can leave the composite plausible and
        // one of these wrong.
        [[nodiscard]] Capture CaptureTargets()
        {
            Capture out;
            u32 w = 0;
            u32 h = 0;
            EXPECT_TRUE(ReadbackComposite(out.Composite, w, h));
            for (const std::string_view name : { ResourceNames::SceneColor, ResourceNames::ToneMapColor, ResourceNames::GBufferResolved })
            {
                const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name);
                if (!framebuffer)
                    continue;
                const auto& spec = framebuffer->GetSpecification();
                const u32 textureId = framebuffer->GetColorAttachmentRendererID(0);
                if (textureId == 0u || spec.Width == 0u || spec.Height == 0u)
                    continue;
                std::vector<u8> pixels;
                ReadbackRgba8(textureId, spec.Width, spec.Height, pixels);
                out.Intermediates.emplace_back(std::string(name), std::move(pixels));
            }
            return out;
        }

        // The readback is bottom-up; the PNG is written the right way up.
        static void WritePng(const std::string& fileName, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            ASSERT_EQ(rgba.size(), static_cast<sizet>(width) * height * 4u) << fileName;
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            std::vector<u8> flipped(rgba.size());
            for (u32 y = 0; y < height; ++y)
                std::memcpy(flipped.data() + (static_cast<sizet>(y) * rowBytes),
                            rgba.data() + (static_cast<sizet>(height - 1u - y) * rowBytes), rowBytes);

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / fileName).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               flipped.data(), static_cast<int>(rowBytes));
            EXPECT_NE(wrote, 0) << "failed to write evidence PNG " << path;
        }

        Entity m_Mover;
    };

    // AC2: cached execution and a forced full rebuild produce equivalent images
    // and resources over toggle / resize / MSAA / upscale / reload /
    // path-switch sequences.
    TEST_F(FrameGraphDeclarationCacheEvidenceTest, CachedFramesMatchAForcedRebuild)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UseDeterministicPost();
        const EditorCamera camera = MakeCamera();

        u32 checkpoints = 0;
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SetPath(path);
            for (const Operation& op : Operations(path))
            {
                SCOPED_TRACE(std::string(PathName(path)) + " / " + op.Name);
                op.Apply();
                RunEditorFrames(camera, 3); // compile, then settle into the cache

                // Control: two consecutive CACHED frames. This is the noise
                // floor a cache fault has to be distinguished from.
                RunEditorFrames(camera, 1);
                const Capture a = CaptureTargets();
                RunEditorFrames(camera, 1);
                const Capture b = CaptureTargets();

                // The same frame again, compiled from scratch.
                const auto statsBefore = Renderer3D::GetFrameGraphDeclarationStats();
                {
                    ScopedVerifyLever verify(true);
                    RunEditorFrames(camera, 1);
                }
                const auto statsAfter = Renderer3D::GetFrameGraphDeclarationStats();
                const Capture c = CaptureTargets();
                if (::testing::Test::HasFatalFailure())
                    return;

                ASSERT_EQ(statsAfter.VerifiedHits, statsBefore.VerifiedHits + 1u)
                    << "The rebuild frame was not a verified cache hit, so it compared nothing: the cache did "
                       "not hold across three settle frames, which is itself a spurious recompile.";
                EXPECT_EQ(statsAfter.StaleCacheDetections, statsBefore.StaleCacheDetections)
                    << "Stale cache: " << statsAfter.LastStaleCacheDetail;

                const PixelDiff control = Compare(a.Composite, b.Composite);
                const PixelDiff rebuilt = Compare(b.Composite, c.Composite);
                EXPECT_LE(rebuilt.Pixels, control.Pixels)
                    << "The forced rebuild differs from the cached frame on " << rebuilt.Pixels
                    << " composite pixels (max delta " << rebuilt.MaxDelta << "), above the cached-to-cached "
                    << "noise floor of " << control.Pixels << " (max " << control.MaxDelta << ").";

                ASSERT_EQ(b.Intermediates.size(), c.Intermediates.size())
                    << "The rebuild resolved a different set of intermediate targets than the cache.";
                ASSERT_EQ(a.Intermediates.size(), b.Intermediates.size())
                    << "Two cached frames resolved different intermediate targets.";
                for (sizet i = 0; i < b.Intermediates.size(); ++i)
                {
                    ASSERT_EQ(a.Intermediates[i].first, b.Intermediates[i].first);
                    ASSERT_EQ(b.Intermediates[i].first, c.Intermediates[i].first);
                    const PixelDiff targetControl = Compare(a.Intermediates[i].second, b.Intermediates[i].second);
                    const PixelDiff targetRebuilt = Compare(b.Intermediates[i].second, c.Intermediates[i].second);
                    EXPECT_LE(targetRebuilt.Pixels, targetControl.Pixels)
                        << b.Intermediates[i].first << ": the rebuild differs on " << targetRebuilt.Pixels
                        << " pixels (max " << targetRebuilt.MaxDelta << "), the cached-to-cached control on "
                        << targetControl.Pixels << ".";
                }
                ++checkpoints;

                if (op.Name == "features off")
                {
                    WritePng("DeclarationCache_GL_" + std::string(PathName(path)) + ".png", b.Composite, kWidth, kHeight);
                    WritePng("DeclarationCacheRebuilt_GL_" + std::string(PathName(path)) + ".png", c.Composite, kWidth,
                             kHeight);
                }
            }
        }
        EXPECT_GE(checkpoints, 40u);
    }

    // AC1 / AC2 by accounting: with the verifier on for EVERY frame, every
    // cache hit across the sequences is rebuilt and its compiled plan compared
    // with the cached one. No difference may appear anywhere.
    TEST_F(FrameGraphDeclarationCacheEvidenceTest, VerifyModeFindsNoStaleCacheAcrossSequences)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UseDeterministicPost();
        const EditorCamera camera = MakeCamera();

        Renderer3D::ResetFrameGraphDeclarationStats();
        {
            ScopedVerifyLever verify(true);
            for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred,
                                              RenderingPath::Forward })
            {
                SetPath(path);
                for (const Operation& op : Operations(path))
                {
                    op.Apply();
                    RunEditorFrames(camera, 3);
                }
            }
            // Shader hot reload: every library shader recompiles, and pass
            // readiness can flip while it does.
            Renderer3D::GetShaderLibrary().ReloadShaders();
            RunEditorFrames(camera, 4);
        }
        const auto stats = Renderer3D::GetFrameGraphDeclarationStats();

        std::cout << "[DeclarationCache] verify sequence: frames=" << stats.Frames << " compiles=" << stats.Compiles
                  << " cacheHits=" << stats.CacheHits << " verifiedHits=" << stats.VerifiedHits
                  << " redundantCompiles=" << stats.RedundantCompiles << " stale=" << stats.StaleCacheDetections << "\n";
        EXPECT_GT(stats.VerifiedHits, 50u) << "Too few verified frames for the sequence to have proven anything.";
        EXPECT_EQ(stats.StaleCacheDetections, 0u)
            << "A cached build differed from a forced rebuild of the same configuration: "
            << stats.LastStaleCacheDetail;
    }

    // AC4: an unchanged configuration reuses the graph. Camera and object
    // motion are execution data and must not recompile anything; the CPU cost
    // of a cache hit and of a compile is measured and reported.
    TEST_F(FrameGraphDeclarationCacheEvidenceTest, CameraAndObjectMotionDoNotRecompile)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        UseDeterministicPost();
        ASSERT_TRUE(static_cast<bool>(m_Mover));

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            SCOPED_TRACE(PathName(path));
            SetPath(path);
            EditorCamera camera = MakeCamera();
            RunEditorFrames(camera, 4);

            constexpr u32 kMotionFrames = 60u;
            Renderer3D::ResetFrameGraphDeclarationStats();
            for (u32 frame = 0; frame < kMotionFrames; ++frame)
            {
                const f32 t = static_cast<f32>(frame) / static_cast<f32>(kMotionFrames);
                camera.SetPose({ -4.0f + (8.0f * t), 3.0f + t, 12.0f - (3.0f * t) }, 0.25f - (0.5f * t), 0.3f);
                m_Mover.GetComponent<TransformComponent>().Translation = { -3.0f + (2.0f * t), 0.5f, 2.0f * t };
                RunEditorFrames(camera, 1);
            }
            const auto motion = Renderer3D::GetFrameGraphDeclarationStats();
            EXPECT_EQ(motion.Compiles, 0u)
                << "Camera/object motion recompiled the frame graph " << motion.Compiles
                << " times; last cause: " << motion.LastCompileCause;
            EXPECT_EQ(motion.CacheHits, kMotionFrames);

            // Cost of a compile for the same configuration: toggle a topology
            // input every frame so each one compiles.
            Renderer3D::ResetFrameGraphDeclarationStats();
            constexpr u32 kCompileFrames = 20u;
            for (u32 frame = 0; frame < kCompileFrames; ++frame)
            {
                Renderer3D::GetPostProcessSettings().FXAAEnabled = (frame % 2u) == 0u;
                RunEditorFrames(camera, 1);
            }
            Renderer3D::GetPostProcessSettings().FXAAEnabled = false;
            const auto compiles = Renderer3D::GetFrameGraphDeclarationStats();
            EXPECT_EQ(compiles.Compiles, kCompileFrames) << "Each FXAA toggle must compile exactly once.";
            EXPECT_EQ(compiles.RedundantCompiles, 0u)
                << "An FXAA toggle changes the declarations; a compile whose plan did not change is over-invalidation.";

            const f64 hitMicros = motion.CacheHitMicrosTotal / static_cast<f64>(std::max<u64>(motion.CacheHits, 1u));
            const f64 compileMicros = compiles.CompileMicrosTotal / static_cast<f64>(std::max<u64>(compiles.Compiles, 1u));
            std::cout << "[DeclarationCache] " << PathName(path) << ": cache hit " << hitMicros << " us/frame over "
                      << motion.CacheHits << " frames, compile " << compileMicros << " us/frame over "
                      << compiles.Compiles << " frames (x" << (compileMicros / std::max(hitMicros, 0.001))
                      << "), of which the plan digest "
                      << (compiles.DigestMicrosTotal / static_cast<f64>(std::max<u64>(compiles.Compiles, 1u))) << " us\n";
            ::testing::Test::RecordProperty(std::string("HitMicros_") + PathName(path), std::to_string(hitMicros));
            ::testing::Test::RecordProperty(std::string("CompileMicros_") + PathName(path), std::to_string(compileMicros));
            EXPECT_LT(hitMicros, compileMicros) << "A cache hit must be cheaper than the compile it skips.";
        }
    }

    // History validity is keyed, generations are not (issue #1333). A history
    // sink latches its registry token when PopulateBlackboard acquires the
    // history, and PopulateBlackboard is skipped while the key holds. So a
    // history invalidated again while it is ALREADY invalid (every frame of
    // camera motion under the path tracer) leaves the key unchanged, the frame
    // is served from the cache, and a latched token would now be stale:
    // MarkProduced rejects it on every later frame and the history never comes
    // back. The verifier cannot see this, because its forced repopulate hands
    // the sink a fresh token; this test is the guard for it.
    TEST_F(FrameGraphDeclarationCacheEvidenceTest, AHistoryInvalidatedWhileInvalidComesBackOnACachedGraph)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetPath(RenderingPath::Deferred);
        Renderer3D::GetPostProcessSettings().TAAEnabled = true;
        const EditorCamera camera = MakeCamera();

        const auto surfaceHistory = []() -> std::optional<TemporalHistorySnapshot>
        {
            const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
            if (!graph)
                return std::nullopt;
            for (const auto& history : graph->GetTemporalHistoryRegistry().Snapshot())
            {
                if (history.Key.Effect == TemporalHistoryEffect::TAA &&
                    history.Key.Plane == TemporalHistoryPlane::SurfaceGeometry)
                {
                    return history;
                }
            }
            return std::nullopt;
        };

        RunEditorFrames(camera, 6);
        const auto warm = surfaceHistory();
        ASSERT_TRUE(warm.has_value()) << "Deferred + TAA should own a surface history; the premise is missing.";
        ASSERT_TRUE(warm->Valid) << "The history must be valid before the invalidations for the test to mean anything.";

        // Three frames in a row, each invalidated before it runs: the first
        // flips valid true -> false (the key moves), the next two invalidate a
        // history that is already invalid (the key does not).
        Renderer3D::ResetFrameGraphDeclarationStats();
        for (u32 frame = 0; frame < 3u; ++frame)
        {
            ASSERT_GT(Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::Manual, TemporalHistoryEffect::TAA),
                      0u);
            RunEditorFrames(camera, 1);
        }
        const auto invalidated = Renderer3D::GetFrameGraphDeclarationStats();
        EXPECT_GE(invalidated.CacheHits, 1u)
            << "The repeated invalidations were all compiles, so this run never exercised a cached frame.";

        RunEditorFrames(camera, 3);
        const auto recovered = surfaceHistory();
        ASSERT_TRUE(recovered.has_value());
        EXPECT_TRUE(recovered->Valid)
            << "TAA's surface history stayed invalid after the invalidations stopped: a history sink kept a token "
               "from before the repeated invalidation and every MarkProduced since has been rejected. "
               "RenderGraph::RefreshHistorySinkTokens must run every frame after the capture.";
        EXPECT_GT(recovered->Token.Generation, warm->Token.Generation);
    }

    // A repopulated blackboard must re-run every pass's Setup(). Found live
    // while verifying #1397: in Deferred with SSR on, an upscale toggle resizes
    // the SSR history inside PopulateBlackboard, which invalidates BOTH caches
    // -- but the build cache was invalidated before that frame's own
    // BuildFrameGraph, which re-armed it under the same key. The next frame's
    // key matched, so the blackboard re-populated (ClearImportedResources makes
    // every view handle stale) while BuildFrameGraph served the cached Setup()
    // output. GTAO's depth/normals and AOApply's input then resolved as stale
    // handles, and AOApply's assert took the editor down.
    //
    // The verify lever stays OFF: it rebuilds every cache hit, which is exactly
    // what hides this.
    TEST_F(FrameGraphDeclarationCacheEvidenceTest, ARepopulatedBlackboardNeverServesACachedGraph)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedVerifyLever verifyOff(false);
        UseDeterministicPost();
        SetPath(RenderingPath::Deferred);
        auto& post = Renderer3D::GetPostProcessSettings();
        post.SSREnabled = true;
        post.ActiveAOTechnique = AOTechnique::GTAO;
        post.GTAOEnabled = true;
        // The AO technique reaches the graph only through ApplyRendererSettings,
        // which reconciles it with the one SetPath configured; without it GTAO
        // and AOApply are never wired in and half the coverage is imaginary.
        Renderer3D::ApplyRendererSettings();
        const EditorCamera camera = MakeCamera();

        const auto failuresThisFrame = []() -> std::string
        {
            const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
            if (!graph)
                return "no active graph";
            std::string out;
            for (const auto& failure : graph->GetResolveFailures())
                out += failure.PassName.ToStdString() + ": " + failure.Reason.ToStdString() + "\n";
            return out;
        };

        RunEditorFrames(camera, 4);
        ASSERT_EQ(failuresThisFrame(), "") << "the settled native frame already fails to resolve";

        constexpr std::array kModes{ UpscaleMode::Performance, UpscaleMode::Off, UpscaleMode::Quality, UpscaleMode::Off };
        Renderer3D::ResetFrameGraphDeclarationStats();
        for (const UpscaleMode mode : kModes)
        {
            post.Upscale = mode;
            for (u32 frame = 0; frame < 3u; ++frame)
            {
                RunEditorFrames(camera, 1);
                EXPECT_EQ(failuresThisFrame(), "")
                    << "frame " << frame << " after switching upscale to " << static_cast<int>(mode)
                    << " resolved stale graph handles: the blackboard re-populated under a cached graph";
            }
        }

        // The premise: the SSR history resize re-populated the blackboard on a
        // frame whose key did NOT move. Each toggle moves the key once; the
        // out-of-band re-populate is the compile beyond that. Without one, the
        // assertions above passed on ordinary key changes and proved nothing.
        const auto stats = Renderer3D::GetFrameGraphDeclarationStats();
        EXPECT_GT(stats.Compiles, static_cast<u64>(kModes.size()))
            << "no toggle produced a second compile, so the re-populate-under-an-unchanged-key frame this test "
               "exists for never happened (is SSR still declared on Deferred?)";
    }
} // namespace OloEngine::Tests
