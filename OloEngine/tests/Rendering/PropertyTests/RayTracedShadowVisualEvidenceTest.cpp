// OLO_TEST_LAYER: integration
//
// =============================================================================
// RayTracedShadowVisualEvidenceTest.cpp — #1056.
//
// WHAT THIS TENANT CAN AND CANNOT PROVE, said first because it is the whole
// design of the file.
//
// The fixture renders through an OPENGL 4.6 context, and GL_EXT_ray_query has
// no OpenGL representation — so a ray-traced shadow can never be produced here.
// That is not a gap this test is working around; it is the single most
// important arm of the feature, because it is the arm every CI runner and
// every non-RT GPU takes. So this file proves the FALLBACK, and proves it the
// only way that is worth anything:
//
//   1. Arming the technique on a device that cannot deliver it moves NO MORE
//      pixels than the renderer moves on its own between two identical
//      captures. That is the issue's "existing-golden ratchet, not a claim" —
//      a stray uniform, a re-bound texture unit, a cleared target or a
//      re-declared graph resource would each move a pixel, and any of them
//      moving one means the fallback is not free.
//
//      It is phrased against a MEASURED noise floor rather than against zero
//      because the two captures are not independent: they share one renderer,
//      so the second runs with more temporal history behind it than the first.
//      Asserting a flat zero would be asserting that every temporal accumulator
//      in the engine has settled by frame N — a claim about somebody else's
//      code that would fail as a flake and be read as this feature's bug. So
//      the test captures the SAME setting twice first, calls whatever moved
//      the noise floor, and then requires the technique flip to stay inside it
//      (live-verification-noise-floor.md). On a settled renderer the floor is
//      zero and this IS the byte-identity check; when it is not, the test still
//      states something true instead of something convenient.
//
//   2. The frame it is identical TO actually contains a shadow. Two identical
//      black frames would satisfy (1) perfectly, so the contrast between the
//      lit floor and the cube's shadow is asserted on the same capture.
//
//   3. The mask texture slot is not silently sampled. The lighting shader's
//      routing lane must be off, which the identical-bytes result already
//      implies — but the PNGs are written so a human can look, per CLAUDE.md.
//
// THE RAY-TRACED IMAGES THEMSELVES ARE NOT EVIDENCE FROM HERE. They come from
// a live editor run on the Vulkan backend, captured through the MCP
// diagnostics server, and are reported in the PR. Claiming otherwise from a GL
// test would be exactly the "green unit tests are not evidence" failure
// CLAUDE.md warns about.
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
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowTechnique.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
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

        // Frozen wall clock, for the same reason WaterVisualEvidenceTest freezes
        // it: the byte-identical comparison below is only meaningful if both
        // captures render the same instant.
        constexpr f32 kCaptureTime = 4.0f;

        // The scene, as CONSTANTS rather than literals buried in BuildScene, so
        // the assertions below can DERIVE where the shadow lands instead of
        // guessing a pixel band. A guessed band is the classic way a visual test
        // ends up measuring the floor twice and passing on a frame with no
        // shadow in it at all.
        const glm::vec3 kSunTravelDirection = glm::normalize(glm::vec3(-0.45f, -0.8f, -0.4f));
        // TWO casters, and only one of them is asserted on.
        //
        // The distant one is small (4 m) and 24 m from the camera, so the CSM
        // cascade covering it resolves its shadow as a thin sliver — visible in
        // RayTracedShadow_ShadowMapTechnique.png at roughly (434, 274) and far
        // too faint to be a reliable "is there a shadow here" probe. That
        // faintness is not a bug to work around: it is the resolution ceiling
        // #1056 exists to lift, and it is why the ray-traced captures in the PR
        // are worth looking at. It stays in the scene, unasserted.
        //
        // The PROBE caster is larger and nearer, so it lands in the finest
        // cascade and casts a solid shadow the assertion below can stand on.
        const glm::vec3 kDistantCasterCentre{ 0.0f, 4.0f, 0.0f };
        const glm::vec3 kShadowProbeCasterCentre{ 14.0f, 6.0f, 16.0f };
        const glm::vec3 kCameraPosition{ 0.0f, 9.0f, 22.0f };
        constexpr f32 kCameraYaw = 0.0f;
        constexpr f32 kCameraPitch = 0.35f;
        // A floor point far from the caster's shadow, on the opposite side of
        // the sun's travel direction, so it is lit in any plausible framing.
        const glm::vec3 kLitReferencePoint{ 7.0f, 0.0f, 6.0f };
    } // namespace

    class RayTracedShadowVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // A sun that opts INTO ray tracing. On this GL context it cannot be
            // served, which is the point: the request must travel all the way
            // through the light setup, the technique selection and the pass, and
            // come out the other side having changed nothing.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = kSunTravelDirection;
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = true;
                dl.m_RayTracedShadows = true;
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

            // A bright, plain floor so the shadow is a large unambiguous region
            // rather than a detail: the contrast assertion below reads two bands
            // of it, and a textured floor would make that a texture measurement.
            addPrimitive("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f },
                         { 0.85f, 0.85f, 0.86f });
            // The caster, floating clear of the floor so the shadow separates
            // from the object — which is also the configuration where the
            // ray-traced penumbra widens with occluder distance, so the SAME
            // scene is the one the live Vulkan captures use.
            addPrimitive("DistantCaster", MeshPrimitive::Cube, kDistantCasterCentre, { 2.0f, 2.0f, 2.0f },
                         { 0.2f, 0.45f, 0.8f });
            // The probe caster: near enough for the finest cascade, big enough
            // that its shadow is a solid region rather than a sliver. The
            // contrast assertion reads its shadow, so it is what stops the
            // ratchet below from comparing two frames that prove nothing.
            addPrimitive("ShadowProbeCaster", MeshPrimitive::Cube, kShadowProbeCasterCentre,
                         { 3.0f, 3.0f, 3.0f }, { 0.8f, 0.35f, 0.2f });
        }

        void Capture(const std::string& poseName, std::vector<u8>& outPixels)
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kCameraPosition, kCameraYaw, kCameraPitch);
            // Kept so the assertions can project world points through the SAME
            // matrices the capture rendered with. Deriving the sample windows
            // beats guessing a pixel band: a guess that misses reads the lit
            // floor twice and passes on a frame with no shadow in it.
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

            // GL returns rows bottom-up; the PNG and the band sampling below both
            // treat row 0 as the top.
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

            // These PNGs are EVIDENCE, not goldens: they are written on every run
            // rather than compared against a committed reference, because the
            // contract this file asserts is the equality of the two captures with
            // each other, not either one's equality with a stored image. A golden
            // would add a tracked binary that moves with every unrelated lighting
            // change and would assert nothing this file does not already assert
            // more tightly. (See task-loop.md on not committing PNG churn — these
            // are written under the same directory but are regenerated, so stage
            // them deliberately or not at all.)
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec)
                return;
            const std::string path = (dir / ("RayTracedShadow_" + poseName + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             outPixels.data(), static_cast<int>(kWidth) * 4);
        }

        // Project a world point to a pixel with row 0 at the TOP, matching the
        // vertical flip Capture applies. Returns false when the point is behind
        // the camera or off screen, which the caller must treat as a failed
        // premise rather than a failed assertion.
        [[nodiscard]] bool ProjectWorldToPixel(const glm::vec3& world, u32& outX, u32& outY) const
        {
            const glm::vec4 clip = m_CaptureViewProjection * glm::vec4(world, 1.0f);
            if (!(clip.w > 1.0e-4f))
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
                return false;
            const f32 u = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth);
            // NDC +y is up; row 0 is the top after the flip.
            const f32 v = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight);
            outX = static_cast<u32>(std::clamp(u, 0.0f, static_cast<f32>(kWidth - 1u)));
            outY = static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(kHeight - 1u)));
            return true;
        }

        // Mean luminance of a square window centred on a pixel, clamped to the
        // frame. Small (a few percent of the frame) so it stays inside the
        // shadow rather than averaging across its penumbra.
        [[nodiscard]] static f64 MeanLumaAround(const std::vector<u8>& pixels, u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = cx > halfExtent ? cx - halfExtent : 0u;
            const u32 y0 = cy > halfExtent ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent + 1u, kWidth);
            const u32 y1 = std::min(cy + halfExtent + 1u, kHeight);
            return MeanLuma(pixels, x0, y0, x1, y1);
        }

        // Mean luminance of an axis-aligned pixel rectangle, 0..255.
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

        // Pixels whose RGB differs between two equal-size RGBA8 buffers. Alpha
        // is ignored: the composited frame's alpha is not displayed, and a
        // difference there would fail the ratchet for something invisible.
        [[nodiscard]] static std::size_t CountDifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b,
                                                              std::size_t* outFirstDifference)
        {
            std::size_t differing = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                if (a[i + 0] != b[i + 0] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
                {
                    if (differing == 0 && outFirstDifference != nullptr)
                        *outFirstDifference = i / 4u;
                    ++differing;
                }
            }
            return differing;
        }

        glm::mat4 m_CaptureViewProjection{ 1.0f };
    };

    TEST_F(RayTracedShadowVisualEvidenceTest, RequestingRayTracingOnANonRTDeviceStaysInsideTheRendererNoiseFloor)
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

        auto& shadowMap = Renderer3D::GetShadowMap();
        const ShadowSettings originalSettings = shadowMap.GetSettings();
        struct ScopedSettings
        {
            ShadowMap& Map;
            ShadowSettings Original;
            ~ScopedSettings()
            {
                Map.SetSettings(Original);
            }
        } scopedSettings{ shadowMap, originalSettings };

        // Arm A — the raster tier, explicitly. This is the reference frame, and
        // it is the one the contrast assertions run on.
        {
            ShadowSettings settings = originalSettings;
            settings.Enabled = true;
            settings.Technique = ShadowTechnique::ShadowMap;
            shadowMap.SetSettings(settings);
        }
        std::vector<u8> shadowMapPixels;
        Capture("ShadowMapTechnique", shadowMapPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // The SAME setting again. Whatever differs between these two is the
        // renderer's own frame-to-frame movement, and it is the yardstick the
        // technique flip is measured against below.
        std::vector<u8> shadowMapPixelsRepeat;
        Capture("ShadowMapTechniqueRepeat", shadowMapPixelsRepeat);
        if (::testing::Test::HasFatalFailure())
            return;

        // Arm B — the same scene asking for ray tracing it cannot have.
        {
            ShadowSettings settings = originalSettings;
            settings.Enabled = true;
            settings.Technique = ShadowTechnique::RayTraced;
            shadowMap.SetSettings(settings);
        }
        std::vector<u8> rayTracedPixels;
        Capture("RayTracedTechniqueFallback", rayTracedPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // ------------------------------------------------------------------
        // (2) first: the reference frame must actually contain a shadow.
        // ------------------------------------------------------------------
        // Where the probe caster's shadow lands is ARITHMETIC, not a guess:
        // follow the sun's travel direction from the cube centre until it meets
        // the floor plane at y = 0, then project that world point through the
        // very matrices the capture rendered with. A hand-picked pixel band that
        // missed would sample the lit floor twice and pass on a frame with no
        // shadow in it at all — which is exactly what happened to the first
        // version of this test.
        ASSERT_LT(kSunTravelDirection.y, -1.0e-3f) << "the sun must point downward for this derivation";
        const f32 distanceToFloor = kShadowProbeCasterCentre.y / -kSunTravelDirection.y;
        const glm::vec3 shadowCentre = kShadowProbeCasterCentre + kSunTravelDirection * distanceToFloor;

        u32 shadowX = 0, shadowY = 0, litX = 0, litY = 0;
        ASSERT_TRUE(ProjectWorldToPixel(shadowCentre, shadowX, shadowY))
            << "the derived shadow centre " << shadowCentre.x << "," << shadowCentre.y << "," << shadowCentre.z
            << " is off screen — the scene or the camera moved and this test's premise is stale";
        ASSERT_TRUE(ProjectWorldToPixel(kLitReferencePoint, litX, litY))
            << "the lit reference point is off screen — same staleness";

        // A window a few percent of the frame across: wide enough to average out
        // shadow-map filtering noise, narrow enough to stay inside the shadow
        // rather than straddling its edge.
        constexpr u32 kSampleHalfExtent = 12u;
        const f64 shadowedLuma = MeanLumaAround(shadowMapPixels, shadowX, shadowY, kSampleHalfExtent);
        const f64 litLuma = MeanLumaAround(shadowMapPixels, litX, litY, kSampleHalfExtent);

        EXPECT_GT(litLuma, 20.0) << "The lit floor sample at (" << litX << "," << litY
                                 << ") rendered near-black — the capture is not a lit scene. "
                                    "See RayTracedShadow_ShadowMapTechnique.png";
        EXPECT_LT(shadowedLuma, litLuma * 0.85)
            << "No shadow contrast in the reference frame: the derived shadow centre (" << shadowX << ","
            << shadowY << ") reads " << shadowedLuma << " against a lit floor at (" << litX << "," << litY
            << ") reading " << litLuma << ". The byte-identity check below would then be comparing two frames "
                                          "that prove nothing. See RayTracedShadow_ShadowMapTechnique.png";

        // ------------------------------------------------------------------
        // (1) the ratchet, against the measured noise floor.
        // ------------------------------------------------------------------
        ASSERT_EQ(shadowMapPixels.size(), shadowMapPixelsRepeat.size());
        ASSERT_EQ(shadowMapPixels.size(), rayTracedPixels.size());

        std::size_t firstTechniqueDifference = 0;
        const std::size_t noiseFloorPixels = CountDifferingPixels(shadowMapPixels, shadowMapPixelsRepeat, nullptr);
        const std::size_t techniquePixels =
            CountDifferingPixels(shadowMapPixelsRepeat, rayTracedPixels, &firstTechniqueDifference);

        if (noiseFloorPixels > 0)
        {
            // Not silent: a non-zero floor is a real fact about this run, and a
            // reader who only sees "passed" would wrongly believe the frames
            // were identical.
            OLO_CORE_WARN("RayTracedShadowVisualEvidenceTest: renderer noise floor is {} pixel(s) between two "
                          "identical captures; the technique ratchet is evaluated against that, not against zero.",
                          noiseFloorPixels);
        }

        EXPECT_LE(techniquePixels, noiseFloorPixels)
            << "Arming the ray-traced technique moved " << techniquePixels
            << " pixel(s) on a device that cannot serve it, against a renderer noise floor of "
            << noiseFloorPixels << " (first difference at " << (firstTechniqueDifference % kWidth) << ","
            << (firstTechniqueDifference / kWidth) << "). The fallback is supposed to be free: compare "
                                                      "RayTracedShadow_ShadowMapTechniqueRepeat.png and "
                                                      "RayTracedShadow_RayTracedTechniqueFallback.png. A moved pixel means the pass left state "
                                                      "behind, the mask was sampled, or a graph resource was declared that should not have been.";
    }
} // namespace OloEngine::Tests
