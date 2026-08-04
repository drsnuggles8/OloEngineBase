// OLO_TEST_LAYER: L8
// =============================================================================
// ShaderDebugDrawVisualTest.cpp — issue #725
//
// Visual evidence (PNG) that the GPU-pushable debug-draw channels actually reach
// the screen. Pushes one of every primitive type — line, circle, rectangle,
// AABB, box, cone, sphere — into the shared channels, renders the full pipeline
// from three camera poses, and writes each frame to
//   OloEditor/assets/tests/visual/ShaderDebugDraw_<pose>.png
//
// WHY A PIXEL TEST AND NOT JUST THE CONTRACT TESTS. Every part of this feature
// can be correct on the CPU and still draw nothing: the indirect args can be
// right while the barrier is missing, the expansion can be right while the
// screen-space quad math collapses the quad to zero width, the entries can be
// uploaded while the wrong SSBO is bound. All of those pass
// ShaderDebugDrawContractTest and ShaderDebugDrawExpansionTest and produce an
// empty frame. So the assertions here are about PIXELS: each primitive is pushed
// in a colour no other object in the scene can produce, and the test counts how
// many pixels of that colour survive to the composited image.
//
// The pushes go through the CPU appender, which writes into the SAME buffers the
// GLSL helpers append to — that shared buffer is the issue's explicit
// requirement, and it means this test covers the whole draw side (upload,
// indirect args, barrier, expansion, quad math, depth test, blending) even
// though the pushes originate on the CPU. The GPU-side atomic append protocol is
// covered separately by ShaderDebugDrawGpuPushTest, which drives a real compute
// shader.
//
// Runs in the normal suite and SKIPs cleanly without a GL 4.6 context, per the
// issue #258 fixture rules.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
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

        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 600;

        // One saturated primary per primitive type, chosen so that no lit surface
        // in the scene (a grey ground plane under a white sun) can reach them:
        // a debug line writes its colour UNSHADED, so a strongly saturated hue is
        // a signature nothing else in the frame produces.
        //
        // These are pre-tonemap values. The debug pass writes into SceneColor,
        // which the ToneMap pass then processes, so the value that lands in the
        // PNG is darker and slightly desaturated — hence the deliberately loose
        // "is this hue dominant" test below rather than an exact match.
        constexpr glm::vec3 kLineColor{ 1.0f, 0.0f, 0.0f };      // red
        constexpr glm::vec3 kCircleColor{ 0.0f, 1.0f, 0.0f };    // green
        constexpr glm::vec3 kRectangleColor{ 0.0f, 0.0f, 1.0f }; // blue
        constexpr glm::vec3 kAABBColor{ 1.0f, 1.0f, 0.0f };      // yellow
        constexpr glm::vec3 kBoxColor{ 1.0f, 0.0f, 1.0f };       // magenta
        constexpr glm::vec3 kConeColor{ 0.0f, 1.0f, 1.0f };      // cyan
        constexpr glm::vec3 kSphereColor{ 1.0f, 0.5f, 0.0f };    // orange

        // Does this pixel carry the given hue?
        //
        // DIRECTIONAL, not per-channel-threshold. The obvious implementation —
        // "the channels the hue wants are bright, the ones it does not are dark"
        // — silently conflates hues that share a dominant channel: under it a
        // GREEN pixel counts as ORANGE, because orange wants red *and* (at 0.5)
        // green, and green pixels have a bright green channel and a dark blue
        // one. That bug made the depth-test assertion below pass vacuously by
        // matching the control sphere, which is exactly the class of
        // false-negative a "did it draw?" test must not have.
        //
        // Comparing DIRECTION in RGB space instead separates them cleanly:
        // normalised green vs normalised orange is a 55° angle. Tone mapping is a
        // per-channel monotonic curve, so it moves a hue's magnitude far more than
        // its direction — which is what makes this robust to the pre/post-tonemap
        // difference without pinning the curve.
        [[nodiscard]] bool IsHue(const u8* px, const glm::vec3& hue)
        {
            const glm::vec3 pixel(static_cast<f32>(px[0]), static_cast<f32>(px[1]), static_cast<f32>(px[2]));
            const f32 brightest = std::max({ pixel.r, pixel.g, pixel.b });
            const f32 darkest = std::min({ pixel.r, pixel.g, pixel.b });

            // Bright enough to be geometry rather than noise, and saturated enough
            // that the grey ground/sky can never match any hue at all.
            if (brightest < 60.0f || (brightest - darkest) < 40.0f)
                return false;

            const f32 length = glm::length(pixel);
            if (length < 1e-3f)
                return false;
            return glm::dot(pixel / length, glm::normalize(hue)) > 0.96f;
        }

        [[nodiscard]] u32 CountHue(const std::vector<u8>& pixels, const glm::vec3& hue)
        {
            u32 count = 0;
            for (sizet i = 0; i + 3 < pixels.size(); i += 4)
            {
                if (IsHue(pixels.data() + i, hue))
                    ++count;
            }
            return count;
        }
    } // namespace

    class ShaderDebugDrawVisualTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& transform = light.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, 20.0f, 0.0f };
                auto& sun = light.AddComponent<DirectionalLightComponent>();
                sun.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.4f));
                sun.m_Color = glm::vec3(1.0f);
                sun.m_Intensity = 2.0f;
            }

            // A ground plane, for two reasons: it gives the depth test something
            // to test AGAINST (a debug primitive below it must be hidden), and a
            // sparse scene with no static geometry renders the subject near-black
            // (docs/agent-rules/single-mesh-visual-test-lighting.md).
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& transform = ground.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, -2.0f, 0.0f };
                transform.Scale = { 30.0f, 1.0f, 30.0f };
                auto& mesh = ground.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                    mesh.m_MeshSource = plane->GetMeshSource();
                auto& material = ground.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor(glm::vec4(0.35f, 0.35f, 0.38f, 1.0f));
            }
        }

        // Push one of every primitive. Called immediately before each rendered
        // frame: `ShaderDebugDraw::BeginFrame` (inside Renderer3D::EndScene)
        // consumes and clears the CPU staging list, so a push survives exactly
        // one frame — which is the frame-scoped lifetime the feature promises.
        static void PushEveryPrimitive()
        {
            ShaderDebugDraw::DrawLine({ -8.0f, 0.0f, 0.0f }, { -8.0f, 6.0f, 0.0f }, kLineColor);
            ShaderDebugDraw::DrawCircle({ -5.0f, 2.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 1.6f, kCircleColor);
            ShaderDebugDraw::DrawRectangle({ -1.5f, 2.0f, 0.0f }, { 1.4f, 0.0f, 0.0f }, { 0.0f, 1.4f, 0.0f },
                                           kRectangleColor);
            ShaderDebugDraw::DrawAABB({ 1.0f, 0.5f, -1.5f }, { 3.4f, 3.0f, 1.5f }, kAABBColor);

            // The Box primitive with corners that are NOT an AABB — a sheared box,
            // which is the case the 8-explicit-corner form exists for.
            std::array<glm::vec3, 8> corners{};
            for (u32 i = 0; i < 8; ++i)
            {
                const f32 x = (i & 1u) ? 1.2f : -1.2f;
                const f32 y = (i & 2u) ? 1.2f : -1.2f;
                const f32 z = (i & 4u) ? 1.2f : -1.2f;
                corners[i] = glm::vec3(5.6f + x + (0.5f * y), 2.0f + y, z);
            }
            ShaderDebugDraw::DrawBox(corners, kBoxColor);

            ShaderDebugDraw::DrawCone({ 9.0f, 4.0f, 0.0f }, { 0.0f, -3.0f, 0.0f }, 1.4f, kConeColor);
            ShaderDebugDraw::DrawSphere({ 12.5f, 2.0f, 0.0f }, 1.6f, kSphereColor);
        }

        // Render one frame with a fresh set of every primitive.
        void CaptureWithPushes(const EditorCamera& camera, std::vector<u8>& outPixels)
        {
            PushEveryPrimitive();
            CaptureFrame(camera, outPixels);
        }

        // Render one frame (consuming whatever was already pushed) and read back
        // the composited image in top-down row order, ready for stbi_write_png.
        void CaptureFrame(const EditorCamera& camera, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 1);

            auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!framebuffer)
                framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!framebuffer)
                framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(framebuffer) << "No composited framebuffer";

            ReadbackRgba8(framebuffer->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            // glGetTextureImage returns rows bottom-up; flip so the PNG is the
            // right way up.
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bottom = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }
        }

        static void WritePng(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("ShaderDebugDraw_" + poseName + ".png")).string();
            // The rendering rules make the PNG the evidence, so a failed write is
            // a failed test — discarding the result would leave the suite green
            // with nothing to look at, which is the opposite of the point.
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               pixels.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed to write the evidence PNG '" << path << "'";
        }
    };

    TEST_F(ShaderDebugDrawVisualTest, EveryPrimitiveReachesTheViewport)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        const bool restoreEnabled = settings.ShaderDebugDrawEnabled;
        const f32 restoreLineWidth = settings.ShaderDebugDrawLineWidth;
        settings.ShaderDebugDrawEnabled = true;
        // Wide enough that a 800x600 readback definitely resolves each line — a
        // 1px line at this resolution can alias down to a handful of pixels and
        // make a working feature look broken.
        settings.ShaderDebugDrawLineWidth = 4.0f;

        struct Restore
        {
            RendererSettings& Settings;
            bool Enabled;
            f32 LineWidth;
            ~Restore()
            {
                Settings.ShaderDebugDrawEnabled = Enabled;
                Settings.ShaderDebugDrawLineWidth = LineWidth;
            }
        } restore{ settings, restoreEnabled, restoreLineWidth };

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 2.0f, 3.0f, 22.0f }, 0.0f, 0.05f);

        // One warm-up frame, for two independent reasons — both of which would
        // otherwise make a WORKING feature fail this test on frame 1:
        //   * the settings flag only reaches ShaderDebugDraw at
        //     ConfigurePassesForFrame, which runs inside EndScene — i.e. AFTER
        //     the pushes above, so the first frame's pushes are refused;
        //   * the debug shader compiles (possibly asynchronously) and the pass
        //     declares its SceneColor RMW only once it reports ready.
        std::vector<u8> pixels;
        CaptureWithPushes(camera, pixels);
        if (::testing::Test::HasFatalFailure())
            return;

        CaptureWithPushes(camera, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        WritePng("Front", pixels);

        const std::array<std::pair<const char*, glm::vec3>, 7> expected{ {
            { "Line", kLineColor },
            { "Circle", kCircleColor },
            { "Rectangle", kRectangleColor },
            { "AABB", kAABBColor },
            { "Box", kBoxColor },
            { "Cone", kConeColor },
            { "Sphere", kSphereColor },
        } };

        for (const auto& [name, hue] : expected)
        {
            EXPECT_GT(CountHue(pixels, hue), 50u)
                << "No pixels of the " << name
                << " channel's colour reached the composited frame. The primitive was pushed and the "
                   "contract tests pass, so the failure is on the DRAW side: check the indirect args "
                   "(instanceCount), the SHADER_STORAGE|COMMAND barrier, or the screen-space quad "
                   "expansion. See assets/tests/visual/ShaderDebugDraw_Front.png.";
        }
    }

    TEST_F(ShaderDebugDrawVisualTest, WorldSpacePrimitivesAreDepthTestedAgainstSceneGeometry)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        const bool restoreEnabled = settings.ShaderDebugDrawEnabled;
        settings.ShaderDebugDrawEnabled = true;
        struct Restore
        {
            RendererSettings& Settings;
            bool Enabled;
            ~Restore()
            {
                Settings.ShaderDebugDrawEnabled = Enabled;
            }
        } restore{ settings, restoreEnabled };

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 4.0f, 18.0f }, 0.0f, 0.12f);

        // Warm-up (see the enable-propagation note in the test above).
        std::vector<u8> pixels;
        PushEveryPrimitive();
        RunEditorFrames(camera, 1);

        // TWO spheres, pushed together into the same channel:
        //   * `kSphereColor` (orange) buried well BELOW the opaque ground plane,
        //     which sits at y = -2 and spans 30 units. It must NOT appear.
        //   * `kCircleColor` (green) floating in clear air. It MUST appear.
        //
        // The second one is what makes this test mean anything: a
        // "the buried sphere is invisible" assertion on its own passes just as
        // happily when the whole feature is broken and nothing draws at all. The
        // pair distinguishes "depth test works" from "nothing rendered".
        ShaderDebugDraw::DrawSphere({ 0.0f, -7.0f, 0.0f }, 1.5f, kSphereColor);
        ShaderDebugDraw::DrawSphere({ 0.0f, 3.0f, 6.0f }, 1.5f, kCircleColor);
        CaptureFrame(camera, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        WritePng("OccludedSphere", pixels);

        EXPECT_GT(CountHue(pixels, kCircleColor), 50u)
            << "The control sphere in open air did not render either — this frame drew no debug geometry at "
               "all, so the occlusion result below proves nothing. Fix that first.";
        EXPECT_LT(CountHue(pixels, kSphereColor), 50u)
            << "A debug sphere buried under the opaque ground plane is visible — the debug pass is not "
               "depth-testing against the scene, so every world-space bound will read as floating over "
               "the geometry it describes. See assets/tests/visual/ShaderDebugDraw_OccludedSphere.png.";
    }

    // -------------------------------------------------------------------------
    // Cache-invalidation regression (the #530 class,
    // docs/agent-rules/render-pipeline-caches.md).
    //
    // `ShaderDebugDrawPass::Setup` declares NOTHING while disabled, so the enable
    // gates a render-graph declaration — and a fingerprint that does not hash it
    // leaves `m_HasValidBlackboardCache` valid, short-circuits PopulateBlackboard
    // AND BuildFrameGraph, and reuses a cached build with the pass still
    // undeclared. The feature then stays invisible until some unrelated
    // fingerprint input happens to move, which is worse than never working: it
    // starts working the moment you also load a scene or switch rendering path,
    // so it reads as intermittent.
    //
    // The shape of this test is the whole point. Several frames run with the
    // feature OFF first, specifically so the graph cache is warm and every other
    // fingerprint input has settled; then the ONLY thing that changes is the
    // enable. Turning it on from a cold start would pass either way.
    // -------------------------------------------------------------------------
    TEST_F(ShaderDebugDrawVisualTest, EnablingAfterTheGraphCacheIsWarmStillDeclaresThePass)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        const bool restoreEnabled = settings.ShaderDebugDrawEnabled;
        const f32 restoreLineWidth = settings.ShaderDebugDrawLineWidth;
        struct Restore
        {
            RendererSettings& Settings;
            bool Enabled;
            f32 LineWidth;
            ~Restore()
            {
                Settings.ShaderDebugDrawEnabled = Enabled;
                Settings.ShaderDebugDrawLineWidth = LineWidth;
            }
        } restore{ settings, restoreEnabled, restoreLineWidth };

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 2.0f, 3.0f, 22.0f }, 0.0f, 0.05f);

        settings.ShaderDebugDrawEnabled = false;
        settings.ShaderDebugDrawLineWidth = 4.0f;
        RunEditorFrames(camera, 3); // warm the blackboard + frame-graph caches
        ASSERT_FALSE(ShaderDebugDraw::IsEnabled());

        // The only change from here on.
        settings.ShaderDebugDrawEnabled = true;

        // One frame for ConfigurePassesForFrame to propagate the flag (and, with
        // the fingerprint hashed, rebuild the graph), then push and draw.
        RunEditorFrames(camera, 1);
        ASSERT_TRUE(ShaderDebugDraw::IsEnabled());

        std::vector<u8> pixels;
        CaptureWithPushes(camera, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        WritePng("EnabledAfterWarmCache", pixels);

        EXPECT_GT(CountHue(pixels, kAABBColor), 50u)
            << "Turning the feature on after the graph cache was warm drew nothing. The enable gates a "
               "graph declaration, so it MUST be hashed into ComputeBlackboardFingerprint -- HashPassState "
               "covers only the pass pointer and IsReadyForExecution(), never IsEnabled().";
    }

    TEST_F(ShaderDebugDrawVisualTest, DisabledDrawsNothing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        const bool restoreEnabled = settings.ShaderDebugDrawEnabled;
        settings.ShaderDebugDrawEnabled = false;
        struct Restore
        {
            RendererSettings& Settings;
            bool Enabled;
            ~Restore()
            {
                Settings.ShaderDebugDrawEnabled = Enabled;
            }
        } restore{ settings, restoreEnabled };

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 2.0f, 3.0f, 22.0f }, 0.0f, 0.05f);

        // Warm-up FIRST, before any push. The settings flag only reaches
        // ShaderDebugDraw at ConfigurePassesForFrame (inside EndScene), so
        // pushing before a frame has run would push while ShaderDebugDraw still
        // holds whatever enable state the PREVIOUS test in this process left
        // behind — making the result depend on test order. One frame propagates
        // the flag, and only then is "the appenders refuse" the thing under test.
        RunEditorFrames(camera, 1);
        ASSERT_FALSE(ShaderDebugDraw::IsEnabled())
            << "The disabled state did not propagate; the rest of this test would be vacuous.";

        // Push while disabled — the appenders must refuse, so nothing is staged
        // and nothing is drawn. This is the "zero cost when disabled" acceptance
        // criterion's observable half.
        PushEveryPrimitive();
        RunEditorFrames(camera, 2);

        auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
        if (!framebuffer)
            framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
        if (!framebuffer)
            framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
        ASSERT_TRUE(framebuffer);

        std::vector<u8> pixels;
        ReadbackRgba8(framebuffer->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);

        EXPECT_LT(CountHue(pixels, kAABBColor), 50u) << "Debug geometry drew while the feature was disabled";
        EXPECT_LT(CountHue(pixels, kBoxColor), 50u) << "Debug geometry drew while the feature was disabled";
    }
} // namespace OloEngine::Tests
