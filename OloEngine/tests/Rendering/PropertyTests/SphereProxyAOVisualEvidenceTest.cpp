// =============================================================================
// SphereProxyAOVisualEvidenceTest.cpp
//
// OLO_TEST_LAYER: L8
//
// Visual evidence (PNG) + a driver-independent contract for analytic
// sphere-proxy ambient occlusion (SphereProxyAO.comp driven by
// SphereProxyAORenderPass), issue #710.
//
// THE ACCEPTANCE CRITERION IS A DIFFERENTIAL ACROSS A CAMERA PAN, and that is
// what this file measures. A still frame cannot show a pop; only two frames of
// the same receiver with the occluder in and then out of view can.
//
// The scene is a grey floor with one large cube occluder placed near the camera
// and off to one side, and a receiver patch of floor a little further away and
// nearer the view axis. Two poses:
//
//   * "InFrame"  — yaw 0. The occluder is on screen, so the SCREEN-SPACE AO
//                  term can see it and darkens the receiver.
//   * "OffFrame" — yawed until the occluder is entirely outside the frame while
//                  the receiver is still comfortably inside it. The occluder is
//                  much closer to the camera than the receiver, so a modest yaw
//                  swings it out while the receiver barely moves.
//
// The contract, asserted on the AO BUFFER (white = unoccluded) so AO
// correctness is unambiguous, and golden-free so it survives a GPU change:
//
//   1. The proxy pass darkens the receiver in BOTH poses, relative to the same
//      frame with the pass off. Its CONTRIBUTION is what is measured - a
//      within-pose difference, so the camera move, the tone curve and GTAO's
//      own contribution all cancel.
//   2. That contribution SURVIVES the pan: it is essentially the same with the
//      occluder off screen as with it on. This is the acceptance criterion -
//      the darkening does not pop away when the occluder leaves the frame.
//   3. The sky stays unoccluded in every frame (the pass skips far-plane texels
//      rather than tinting the background).
//   4. The term is TEMPORALLY STABLE: two consecutive captures of the same
//      proxy-on frame are pixel-identical. The integral is analytic, so any
//      flicker would be a binning or reprojection bug, and that is the issue's
//      second acceptance bullet.
//
// WHAT THIS TEST DELIBERATELY DOES NOT ASSERT, and why. The issue frames the
// criterion as a screen-space contact shadow POPPING away, and an earlier
// version of this file tried to measure exactly that: the screen-space term's
// receiver darkening in "InFrame" against "OffFrame". It does not reproduce on
// this engine, and the reason is not this pass. GTAO here produces almost no
// localised darkening at a cube/floor junction to begin with - measured 88% of
// its (already small) receiver darkening retained across the pan, i.e. what it
// loses is noise - and GTAOVisualEvidenceTest already records the same gap in
// its own words ("GTAO's horizon search here does not yet produce a strongly
// visually distinct darker band at this cube/floor junction"). There is no pop
// because there is barely a contact shadow. Asserting on that difference would
// have been asserting on GTAO's noise floor, so this test measures the proxy
// term's own contribution and its invariance across the pan instead - which is
// the property the feature actually promises. GTAORadius is still raised to 8
// so the screen-space term has every chance to reach the occluder.
//
// The measurement bands are PROJECTED from world space through the capture
// camera rather than hard-coded in UV, because the receiver deliberately moves
// on screen between the two poses. Runs in the normal suite and SKIPs (not
// fails) without a GL 4.6 context, matching the other AO evidence tests.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
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

        const glm::vec3 kCameraPosition{ 0.0f, 4.0f, 8.0f };
        constexpr f32 kCameraPitch = 0.25f;

        // The occluder, on the LEFT of the view axis at yaw 0, resting on the
        // floor (half-extent 4, centre at y = 4). Sized so its CONTAINED proxy
        // (radius 4, the inscribed sphere) subtends enough of the receiver's
        // hemisphere for the contribution to be several luma rather than one —
        // a contract asserted at the buffer's noise floor is not a contract.
        const glm::vec3 kOccluderCentre{ -3.0f, 4.0f, -12.0f };
        constexpr f32 kOccluderSize = 8.0f;

        // The receiver, on the RIGHT — so a yaw that sweeps the occluder off one
        // edge moves the receiver further INTO frame rather than off the other.
        // Their angular separation from the camera (~17 degrees) is what makes a
        // yaw exist that has the occluder fully out and the receiver comfortably
        // in; a receiver tucked right beside the occluder would leave with it.
        // Still close enough in 3D (7.2 units to the proxy centre, 1.8 radii)
        // that the integral has real magnitude there.
        const glm::vec3 kReceiverPoint{ 3.0f, 0.0f, -12.0f };

        [[nodiscard]] f64 BandLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const auto clampU = [](f32 v)
            { return std::clamp(v, 0.0f, 1.0f); };
            const u32 ix0 = static_cast<u32>(clampU(x0) * static_cast<f32>(kWidth));
            const u32 ix1 = static_cast<u32>(clampU(x1) * static_cast<f32>(kWidth));
            const u32 iy0 = static_cast<u32>(clampU(y0) * static_cast<f32>(kHeight));
            const u32 iy1 = static_cast<u32>(clampU(y1) * static_cast<f32>(kHeight));
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

        // The brightest cell of the open floor, used as an in-frame reference for
        // "unoccluded". Self-normalising on purpose: the AO buffer is captured
        // through the tone mapper and the camera moves between poses, so a fixed
        // absolute value would be measuring the tone curve and the pose rather
        // than the AO. The band excludes the sky.
        [[nodiscard]] f64 BrightestFloorLuma(const std::vector<u8>& px)
        {
            constexpr f32 x0 = 0.05f, x1 = 0.95f, y0 = 0.55f, y1 = 0.95f;
            constexpr u32 cellsX = 12, cellsY = 6;
            f64 brightest = 0.0;
            const f32 dx = (x1 - x0) / static_cast<f32>(cellsX);
            const f32 dy = (y1 - y0) / static_cast<f32>(cellsY);
            for (u32 cy = 0; cy < cellsY; ++cy)
            {
                for (u32 cx = 0; cx < cellsX; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    brightest = std::max(brightest, BandLuma(px, cellX0, cellX0 + dx, cellY0, cellY0 + dy));
                }
            }
            return brightest;
        }

        // Where a world point lands in the captured image, as a UV with row 0 at
        // the TOP (the capture flips the GL readback, so the Y is flipped here
        // too). Returns false when the point is behind the camera or off frame.
        [[nodiscard]] bool ProjectToImageUV(const glm::mat4& viewProjection, const glm::vec3& world,
                                            glm::vec2& outUV)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
            if (!(clip.w > 0.0f))
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
                return false;
            outUV = glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            return true;
        }
    } // namespace

    class SphereProxyAOVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Deferred: the proxy pass reads the same G-Buffer normals + depth
            // GTAO does, and the AO buffer it writes back into is GTAO's.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Overhead white sun, shadows OFF so the only floor variation between
            // the captures is the AO contribution.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.1f, -0.95f, 0.1f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 1.5f;
                // Shadows OFF, and the proxy pass must still see the occluder:
                // its proxy bounds come from the DDGI caster funnel, which is
                // deliberately not gated on shadow casting. A frame that lost its
                // proxies here would show up as criterion 3 failing.
                dl.m_CastShadows = false;
            }

            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = false;
            }

            const auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
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

            {
                Entity floor = addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 80.0f, 1.0f, 80.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            {
                Entity occluder = addMesh("Occluder", MeshPrimitive::Cube, kOccluderCentre,
                                          glm::vec3(kOccluderSize));
                auto& mat = occluder.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        // The AO settings both captures share. `proxyEnabled` is the one variable
        // under test.
        void ApplyAOSettings(bool proxyEnabled, bool debugProxyTermOnly)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.GTAOEnabled = true;
            // See the header: the production 0.5 gives the screen-space term no
            // reach to the occluder, so there would be no pop to observe.
            pp.GTAORadius = 8.0f;
            pp.GTAOPower = 2.2f;
            pp.GTAODenoiseEnabled = true;
            pp.GTAODenoisePasses = 4;
            pp.GTAODebugView = true; // capture the AO buffer, not the lit composite

            pp.SphereProxyAOEnabled = proxyEnabled;
            pp.SphereProxyAOStrength = 1.0f;
            pp.SphereProxyAOMaxProxies = 64;
            pp.SphereProxyAOMaxRadius = 25.0f;
            // Production defaults. The scene has one occluder, so neither the
            // ceiling nor the influence window binds at the receiver, and the
            // assertions below measure the integral itself.
            pp.SphereProxyAOInfluenceScale = 4.0f;
            pp.SphereProxyAOMaxOcclusion = 1.0f;
            pp.SphereProxyAODebugView = debugProxyTermOnly;

            Renderer3D::ApplyRendererSettings();
        }

        void Capture(const std::string& tag, EditorCamera& camera, std::vector<u8>& outPixels)
        {
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for sphere-proxy AO capture '" << tag << "'";

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

            const std::string path = (dir / ("SphereProxyAO_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        [[nodiscard]] static EditorCamera MakeCamera(f32 yaw)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(kCameraPosition, yaw, kCameraPitch);
            return camera;
        }

        // True when NO corner of the occluder's world AABB projects inside the
        // frame. The centre alone is not enough: a cube whose centre has left
        // the frame can still cover a third of it.
        [[nodiscard]] static bool OccluderFullyOffScreen(const EditorCamera& camera)
        {
            const glm::vec3 half(kOccluderSize * 0.5f);
            const glm::mat4 vp = camera.GetViewProjection();
            glm::vec2 uv{};
            for (i32 corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 point = kOccluderCentre +
                                        glm::vec3((corner & 1) ? half.x : -half.x,
                                                  (corner & 2) ? half.y : -half.y,
                                                  (corner & 4) ? half.z : -half.z);
                if (ProjectToImageUV(vp, point, uv))
                    return false;
            }
            return true;
        }

        // Search for the smallest yaw that puts the occluder entirely out of
        // frame while the receiver is still well inside it. Derived rather than
        // hard-coded: the answer depends on the camera convention and on the
        // scene's proportions, and a hard-coded yaw that silently stops
        // satisfying either half turns this whole test into a tautology.
        [[nodiscard]] static bool FindOffFrameYaw(f32& outYaw)
        {
            constexpr f32 kReceiverMargin = 0.04f; // keep it clear of the frame edge
            for (i32 step = 1; step <= 200; ++step)
            {
                for (const f32 sign : { 1.0f, -1.0f })
                {
                    const f32 yaw = sign * static_cast<f32>(step) * 0.01f;
                    EditorCamera camera = MakeCamera(yaw);
                    if (!OccluderFullyOffScreen(camera))
                        continue;
                    glm::vec2 uv{};
                    if (!ProjectToImageUV(camera.GetViewProjection(), kReceiverPoint, uv))
                        continue;
                    if (uv.x < kReceiverMargin || uv.x > 1.0f - kReceiverMargin ||
                        uv.y < kReceiverMargin || uv.y > 1.0f - kReceiverMargin)
                        continue;
                    outYaw = yaw;
                    return true;
                }
            }
            return false;
        }

        // Mean AO over a small band centred on the receiver's projected position.
        // Projected rather than hard-coded because the receiver MOVES on screen
        // between the two poses — that motion is the point of the pan.
        [[nodiscard]] static bool ReceiverAO(const std::vector<u8>& px, const EditorCamera& camera, f64& out)
        {
            glm::vec2 uv{};
            if (!ProjectToImageUV(camera.GetViewProjection(), kReceiverPoint, uv))
                return false;
            constexpr f32 kHalfBand = 0.03f;
            out = BandLuma(px, uv.x - kHalfBand, uv.x + kHalfBand, uv.y - kHalfBand, uv.y + kHalfBand);
            return true;
        }

        // The occluder is fully on screen at yaw 0; the off-frame yaw is derived
        // by FindOffFrameYaw.
        static constexpr f32 kYawInFrame = 0.0f;
    };

    // The headline acceptance criterion: the receiver's contact darkening must
    // NOT pop away when the occluder leaves the frame.
    TEST_F(SphereProxyAOVisualEvidenceTest, ContactDarkeningSurvivesTheOccluderLeavingTheFrame)
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

        // The pan must actually do what the test claims, or every measurement
        // below is meaningless. Establish the setup before asserting the result.
        f32 offFrameYaw = 0.0f;
        ASSERT_TRUE(FindOffFrameYaw(offFrameYaw))
            << "no yaw puts the occluder fully off screen while keeping the receiver in it — "
               "the scene proportions no longer support the pan this test is built on";

        EditorCamera inFrameCamera = MakeCamera(kYawInFrame);
        EditorCamera offFrameCamera = MakeCamera(offFrameYaw);
        SCOPED_TRACE(::testing::Message() << "off-frame yaw = " << offFrameYaw);

        {
            glm::vec2 uv{};
            ASSERT_TRUE(ProjectToImageUV(inFrameCamera.GetViewProjection(), kOccluderCentre, uv))
                << "the occluder is not on screen in the InFrame pose — the pan proves nothing";
            ASSERT_TRUE(ProjectToImageUV(inFrameCamera.GetViewProjection(), kReceiverPoint, uv))
                << "the receiver left the frame in the InFrame pose";
        }

        std::vector<u8> proxyOffIn;
        std::vector<u8> proxyOffOut;
        std::vector<u8> proxyOnIn;
        std::vector<u8> proxyOnOut;

        ApplyAOSettings(/*proxyEnabled*/ false, /*debugProxyTermOnly*/ false);
        Capture("ProxyOff_InFrame", inFrameCamera, proxyOffIn);
        if (::testing::Test::HasFatalFailure())
            return;
        ApplyAOSettings(false, false);
        Capture("ProxyOff_OffFrame", offFrameCamera, proxyOffOut);
        if (::testing::Test::HasFatalFailure())
            return;

        ApplyAOSettings(/*proxyEnabled*/ true, /*debugProxyTermOnly*/ false);
        Capture("ProxyOn_InFrame", inFrameCamera, proxyOnIn);
        if (::testing::Test::HasFatalFailure())
            return;
        ApplyAOSettings(true, false);
        Capture("ProxyOn_OffFrame", offFrameCamera, proxyOnOut);
        if (::testing::Test::HasFatalFailure())
            return;

        // The proxy term alone, for the eye: this is what the pass contributes.
        std::vector<u8> proxyTermOnly;
        ApplyAOSettings(true, /*debugProxyTermOnly*/ true);
        Capture("ProxyTerm_OffFrame", offFrameCamera, proxyTermOnly);
        if (::testing::Test::HasFatalFailure())
            return;

        f64 offIn = 0.0, offOut = 0.0, onIn = 0.0, onOut = 0.0;
        ASSERT_TRUE(ReceiverAO(proxyOffIn, inFrameCamera, offIn));
        ASSERT_TRUE(ReceiverAO(proxyOffOut, offFrameCamera, offOut));
        ASSERT_TRUE(ReceiverAO(proxyOnIn, inFrameCamera, onIn));
        ASSERT_TRUE(ReceiverAO(proxyOnOut, offFrameCamera, onOut));

        // The proxy term's CONTRIBUTION at the receiver: how much darker the same
        // pose gets with the pass on. A within-pose difference, so the camera
        // move, the tone curve and GTAO's own contribution all cancel.
        const f64 contributionInFrame = offIn - onIn;
        const f64 contributionOffFrame = offOut - onOut;

        // Reported alongside as context: the receiver's darkening against the
        // brightest open floor in the same image. Not asserted on (see the
        // header) - GTAO's contact term at this junction is at its noise floor.
        SCOPED_TRACE(::testing::Message()
                     << "receiver darkening vs open floor - proxyOff in/out: "
                     << (BrightestFloorLuma(proxyOffIn) - offIn) << "/"
                     << (BrightestFloorLuma(proxyOffOut) - offOut) << ", proxyOn in/out: "
                     << (BrightestFloorLuma(proxyOnIn) - onIn) << "/"
                     << (BrightestFloorLuma(proxyOnOut) - onOut));

        // 1) The pass does real work in both poses - including the one where the
        //    occluder is not in the depth buffer at all, which is the capability
        //    the issue asks for and which no screen-space term can have.
        // ASSERT, not EXPECT: `retained` divides by this below, so a zero or
        // negative contribution here would turn the acceptance-criterion check
        // into a NaN comparison and bury the failure that actually happened.
        ASSERT_GT(contributionInFrame, 6.0)
            << "the proxy pass barely darkened the receiver with the occluder on screen (contribution="
            << contributionInFrame << "). See SphereProxyAO_ProxyOn_InFrame.png";
        EXPECT_GT(contributionOffFrame, 6.0)
            << "the proxy pass did not darken the receiver with the occluder OFF screen (contribution="
            << contributionOffFrame << ") - the whole feature. See SphereProxyAO_ProxyTerm_OffFrame.png";

        // 2) THE ACCEPTANCE CRITERION. The contribution does not pop away when
        //    the occluder leaves the frame: it is essentially the same on both
        //    sides of the pan. It cannot be identical - the receiver sits at a
        //    different screen position and depth, so it is a different texel of
        //    an 8-bit buffer - but it must not fall off a cliff.
        const f64 retained = contributionOffFrame / contributionInFrame;
        EXPECT_GT(retained, 0.75)
            << "the proxy darkening popped when the occluder left the frame: kept "
            << (retained * 100.0) << "% (in frame=" << contributionInFrame
            << ", off frame=" << contributionOffFrame << "). See SphereProxyAO_ProxyOn_*.png";
        EXPECT_LT(retained, 1.5)
            << "the proxy darkening more than doubled when the occluder left the frame (kept "
            << (retained * 100.0) << "%) - that is as wrong as losing it";

        // 4) The sky is never occluded — the pass must skip far-plane texels, not
        //    tint the background. Asserted as a DIFFERENCE against the proxy-off
        //    frame rather than against an absolute white: the AO buffer is
        //    captured through the tone mapper, so "fully unoccluded" lands around
        //    185, not 255, and an absolute threshold would be measuring the tone
        //    curve rather than this pass.
        constexpr f32 kSkyX0 = 0.10f, kSkyX1 = 0.90f, kSkyY0 = 0.02f, kSkyY1 = 0.10f;
        const f64 skyProxyOff = BandLuma(proxyOffOut, kSkyX0, kSkyX1, kSkyY0, kSkyY1);
        const f64 skyProxyOn = BandLuma(proxyOnOut, kSkyX0, kSkyX1, kSkyY0, kSkyY1);
        const f64 skyTermOnly = BandLuma(proxyTermOnly, kSkyX0, kSkyX1, kSkyY0, kSkyY1);
        EXPECT_GT(skyProxyOff, 100.0) << "the sky band is not bright even with the proxy pass off — "
                                         "the band is probably not on the sky";
        EXPECT_NEAR(skyProxyOn, skyProxyOff, 1.0)
            << "the proxy pass changed the sky (off=" << skyProxyOff << ", on=" << skyProxyOn
            << ") — it must skip far-plane texels";
        EXPECT_NEAR(skyTermOnly, skyProxyOff, 1.0)
            << "the proxy term alone is not neutral over the sky (term=" << skyTermOnly
            << ", reference=" << skyProxyOff << ")";
    }

    // The issue's second acceptance bullet: no added temporal noise. The term is
    // analytic, so two captures of an identical frame must be byte-identical —
    // anything else is a binning or reprojection bug, not a sampling trade-off.
    TEST_F(SphereProxyAOVisualEvidenceTest, ProxyTermIsTemporallyStable)
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

        // The proxy term ALONE, so GTAO's own temporal dither cannot mask or
        // manufacture a difference.
        ApplyAOSettings(/*proxyEnabled*/ true, /*debugProxyTermOnly*/ true);

        f32 offFrameYaw = 0.0f;
        ASSERT_TRUE(FindOffFrameYaw(offFrameYaw));

        EditorCamera camera = MakeCamera(offFrameYaw);
        std::vector<u8> first;
        Capture("Stability_A", camera, first);
        if (::testing::Test::HasFatalFailure())
            return;

        EditorCamera sameCamera = MakeCamera(offFrameYaw);
        std::vector<u8> second;
        Capture("Stability_B", sameCamera, second);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_EQ(first.size(), second.size());
        EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size()), 0)
            << "the sphere-proxy AO term is not frame-to-frame stable — it is analytic, so a "
               "difference here is a binning bug, not noise. Compare SphereProxyAO_Stability_A.png "
               "and SphereProxyAO_Stability_B.png";
    }
} // namespace OloEngine::Tests
