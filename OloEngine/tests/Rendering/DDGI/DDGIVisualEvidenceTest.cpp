// OLO_TEST_LAYER: L8
// =============================================================================
// DDGIVisualEvidenceTest.cpp
//
// Visual + pipeline evidence for the realtime DDGI probe relighting path
// (issue #632, docs/adr/0007-ddgi-hit-point-cache-gather.md).
//
// The scene is the programmatic twin of the manual bring-up rig
// (OloEditor/SandboxProject/Assets/Scenes/DDGITest.olo): a grey-floored room
// with a red and a blue side wall, a white back wall, and — the point of the
// rig — a DIVIDER wall at x=0 splitting the room into a directly-lit half
// (red point light at (-5,3,0)) and a dark half that only a light LEAK could
// turn red. A 4x3x4 Realtime-mode LightProbeVolumeComponent covers the
// interior. Four contracts:
//
//   1. ProbePipelineCapturesAndClassifies — the amortized capture schedule
//      covers every probe and classification leaves the interior probes
//      Active (pipeline contract, no pixels).
//   2. LightLeakRegression — the dark half must NOT glow red through the
//      divider (the Chebyshev-visibility leak fix), pinned by band means and
//      two golden PNGs.
//   3. MovingLightRelight — the ADR's headline property: moving the point
//      light to the other half re-lights the STATIC hit-point cache within
//      frames (no recapture needed — geometry is unchanged); the formerly
//      dark side brightens and the formerly lit side goes dark.
//   4. MultiAngleGoldens — three golden PNGs (lit / dark / top-down).
//
// Rendering path: DEFERRED, explicitly (RendererSettings::Path). Deferred is
// the primary GI consumer (DeferredLightingShared.glsl samples the DDGI
// atlases in its ambient ladder); the forward PBR_MultiLight parity is
// covered by the photometric integration the shared LightProbeSampling.glsl
// sampler carries — same include, same formulas — so this file does not
// duplicate the captures on the forward path.
//
// Determinism / goldens
// ---------------------
//   The DDGI pipeline is deterministic by construction: the capture schedule
//   is a linear cursor (no randomness), gather directions are fixed
//   octahedral texel centers (no stochastic rays), and the irradiance EMA
//   converges to a fixed point. On top of that the tests freeze the wall
//   clock (Time::SetMockTime) and pin TAA + auto-exposure OFF (their
//   defaults, but pinned because both are history/feedback effects that
//   would make frame N depend on the whole pose history). Each test runs
//   kConvergenceFrames of the full pipeline before capturing, which
//   re-converges the pass's process-global atlases from ANY starting state
//   (the DDGIProbeUpdatePass singleton persists across tests in the suite,
//   so this is also what makes the goldens independent of test order).
//   Golden compare follows WaterVisualEvidenceTest: a normal run COMPARES
//   (RMSE <= 6.0) and writes nothing; OLOENGINE_GOLDEN_REBASE=1 (re)writes.
//   Run from OloEditor/ so the PNGs land under OloEditor/assets/tests/visual/.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RendererAttachedTest.h"
#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
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
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Capture resolution. Matches SSGIVisualEvidenceTest — big enough that
        // the central band averages thousands of pixels, small enough that
        // five goldens stay cheap to store and diff.
        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;

        // Frozen wall-clock value for every capture. Nothing in this rig is
        // time-animated, but pinning the clock guarantees no time-driven
        // shader input (post stages, future scene additions) can drift a
        // golden — same policy as WaterVisualEvidenceTest.
        constexpr f32 kCaptureTime = 20.0f;

        // Golden RMSE pass threshold (0..255 per channel). Determinism makes
        // same-machine re-runs ~0; the slack absorbs cross-GPU float variance.
        // Same value as WaterVisualEvidenceTest — this is an integration smoke
        // check, not a tight pixel test.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // Probe grid: LightProbeVolumeComponent resolution 4x3x4 (the manual
        // rig's) => 48 probes.
        constexpr i32 kTotalProbes = 4 * 3 * 4;

        // Frames for the pipeline contract test. Derivation: capture budget 8
        // covers the 48 probes in ceil(48/8) = 6 frames; a relocated probe
        // schedules a recapture (a few relocation iterations per probe near
        // the walls/floor), and the steady-state refresh re-captures
        // budget/8 = 1 probe/frame — 40 frames is 6x the minimum, generous
        // headroom for all of that.
        constexpr u32 kContractFrames = 40;

        // Frames run before any capture. Derivation: capture coverage needs 6
        // frames (above); relight touches EVERY probe each frame
        // (RelightBudget 0 = all); the irradiance EMA at hysteresis 0.5
        // halves the distance to its fixed point every frame, so after 60
        // frames the residual from ANY starting state is <= 2^-60 of the
        // initial delta — unmeasurable in 8-bit output. 60 also re-converges
        // the process-global atlases regardless of which test ran before.
        constexpr u32 kConvergenceFrames = 60;

        // Central analysis band (UV fractions of the frame, rows top-down).
        // Samples the room interior — walls/floor/divider face — while
        // avoiding the frame edges where the room exterior and background
        // show. Wide enough (40% x 30% of the frame ≈ 94k pixels) that the
        // mean is insensitive to per-pixel noise.
        constexpr f32 kBandX0 = 0.30f;
        constexpr f32 kBandX1 = 0.70f;
        constexpr f32 kBandY0 = 0.35f;
        constexpr f32 kBandY1 = 0.65f;

        // Near-black floor guard (mean 0..255 channel value) for poses that
        // are SUPPOSED to be lit — catches a dead/black render that would
        // otherwise pass a ratio assertion trivially. Same value as
        // WaterVisualEvidenceTest's guard.
        constexpr f64 kNearBlackFloor = 5.0;

        // --- Linear-space comparison ------------------------------------------
        // Band means are measured on the tone-mapped, gamma-encoded composite
        // (scene PostProcess Gamma = 2.2), so RATIOS of display means wildly
        // understate physical contrast: a 4x linear contrast reads as only
        // 4^(1/2.2) ≈ 1.9x in display units. Every ratio contract below is
        // therefore evaluated in approximate linear luminance, display^2.2.
        // Calibration run (enclosed rig, RTX 4090): lit=60.27 dark=32.82
        // display => 1.84x display == 3.81x linear.
        [[nodiscard]] inline f64 DisplayMeanToLinear(f64 displayMean0To255)
        {
            return std::pow(std::max(displayMean0To255, 0.0) / 255.0, 2.2);
        }

        // --- Test 2 thresholds ----------------------------------------------
        // Lit/dark LINEAR contrast floor. The lit half receives the red point
        // light directly (intensity 8, range 15) plus its red first-bounce
        // GI; the dark half legitimately receives multi-bounce GI around the
        // divider's end slits plus the 0.05 sun — GI fills shadows BY DESIGN,
        // so the dark side is dim, not black. Measured 3.81x linear on the
        // calibration run; 3.0x is the floor with ~27% margin. The LEAK
        // contract proper is the red-excess check below, not this ratio.
        constexpr f64 kLitOverDarkMinLinearRatio = 3.0;

        // Leak contract: the divider blocks the RED light, so any dark-side
        // illumination must be color-neutral (white sun / white-wall bounce).
        // A Chebyshev-less leak tints the dark half red — red channel well
        // above green/blue. Contract: dark-side (R - max(G,B)) stays under 5
        // display grey levels (measured +2.2 neutral noise on the calibration
        // run), while the lit side must show a strong red excess (> 15;
        // measured +33.7) as the positive control that the instrument can
        // actually see redness.
        constexpr f64 kDarkRedExcessCeiling = 5.0;
        constexpr f64 kLitRedExcessFloor = 15.0;

        // --- Test 3 thresholds ----------------------------------------------
        // After the light moves to (+5,3,0), the formerly dark half is
        // directly lit (same intensity-8 light, mirrored geometry) — at least
        // 3x its old LINEAR mean (measured 3.67x: the pre-move mean already
        // contains the neutral GI pedestal, which survives the swap). The
        // formerly lit half keeps only the sun + pedestal: at most 0.5x its
        // old LINEAR mean (measured 0.27x). Stale (un-relit) probe irradiance
        // would keep its red glow alive and fail the drop.
        constexpr f64 kRelightRiseFactorLinear = 3.0;
        constexpr f64 kRelightDropFactorLinear = 0.5;

        // --- Camera poses -----------------------------------------------------
        // The room is a fully-enclosed box (ceiling + z-end walls close it so
        // no sky pedestal washes out the leak contract), so every pose stands
        // INSIDE it: eyes tucked into the front corners of each half (x short
        // of the +-8 side walls, z short of the +6 front wall, y under the
        // ceiling), looking diagonally into that half's floor, side wall,
        // back wall, and divider face in the central band.
        constexpr glm::vec3 kDarkEye{ 6.0f, 3.5f, 4.8f };
        constexpr glm::vec3 kDarkTarget{ 2.0f, 1.5f, -2.0f };
        constexpr glm::vec3 kLitEye{ -6.0f, 3.5f, 4.8f };
        constexpr glm::vec3 kLitTarget{ -2.0f, 1.5f, -2.0f };
        // "Top": a high oblique from under the ceiling (a true top-down would
        // sit above the now-closed roof and see only its outside); the 0.1 z
        // offset in the look direction also dodges the straight-down yaw
        // singularity of the old pose.
        constexpr glm::vec3 kTopEye{ 0.0f, 5.4f, 4.6f };
        constexpr glm::vec3 kTopTarget{ 0.0f, 0.5f, -1.5f };

        // Mean RMSE over RGB (alpha ignored) between two equal-size RGBA8
        // buffers, 0..255 units. Copied from WaterVisualEvidenceTest.
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

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
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

        // Plain mean of the three channel means. Deliberately NOT Rec.709
        // luma: the moving light is red-dominant, and 709 weights would
        // discount exactly the channel the leak/relight contracts care about.
        [[nodiscard]] f64 MeanChannel(const BandStats& s)
        {
            return (s.R + s.G + s.B) / 3.0;
        }

        struct YawPitch
        {
            f32 Yaw;
            f32 Pitch;
        };

        // Derive EditorCamera (yaw, pitch) looking from `eye` toward `target`.
        // EditorCamera::GetOrientation builds quat(euler(-pitch, -yaw, 0)),
        // i.e. R = Ry(-yaw) * Rx(-pitch), and forward = R * (0,0,-1)
        //   = ( sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch) ).
        // Inverting for a desired unit direction d:
        //   pitch = asin(-d.y)        (positive pitch tilts the view DOWN)
        //   yaw   = atan2(d.x, -d.z)  (yaw 0 looks toward -Z)
        [[nodiscard]] YawPitch LookAtYawPitch(const glm::vec3& eye, const glm::vec3& target)
        {
            const glm::vec3 d = glm::normalize(target - eye);
            return { std::atan2(d.x, -d.z), std::asin(glm::clamp(-d.y, -1.0f, 1.0f)) };
        }

        // Freeze the wall clock; RAII restores the real clock on any exit
        // path (including ASSERT early-returns) so the mock can't leak into
        // later tests. Same pattern as WaterVisualEvidenceTest.
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
    } // namespace

    class DDGIVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // The red point light entity — MovingLightRelight repositions it
        // through the normal TransformComponent write, exactly like gameplay
        // code would.
        Entity m_RedLight;

        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Full 3D draw path at capture resolution (sizes cameras + the
            // Renderer3D render-graph targets and flips rendering on).
            EnableRendering(kWidth, kHeight);

            // DEFERRED explicitly — the primary DDGI consumer (see file
            // header for why the forward path is not duplicated here). The
            // fixture snapshots/restores RendererSettings + PostProcess per
            // test, so neither change leaks into later tests.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Determinism: TAA and auto-exposure are history/feedback effects
            // whose output depends on the whole preceding pose sequence.
            // Both default OFF — pinned here so a changed default can never
            // silently make the goldens pose-history-dependent.
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.TAAEnabled = false;
            pp.AutoExposureEnabled = false;

            // Runtime primary camera — RunFrames (used by the pipeline
            // contract test) renders through Scene::OnUpdateRuntime, which
            // needs a primary CameraComponent to drive the graph at all.
            // Pose mirrors the manual rig's camera. SceneCamera defaults to
            // orthographic — force perspective (SceneRenderEvidenceTest's
            // lesson: the ortho [-1,1] depth range clips the room away).
            {
                Entity camera = scene.CreateEntity("Camera");
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 5.0f, 15.0f };
                tc.SetRotationEuler({ -0.3f, 0.0f, 0.0f });
                auto& cc = camera.AddComponent<CameraComponent>();
                cc.Primary = true;
                cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            }

            // Dim near-white sun (intensity 0.05, the manual rig's value):
            // just enough baseline so the dark half is measurable but far too
            // little to mask a red leak. CastShadows=true so the sun cannot
            // shine THROUGH the room's walls either.
            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.95f, 0.9f);
                dl.m_Intensity = 0.05f;
                dl.m_CastShadows = true;
            }

            // The red point light on the LIT (-X) side of the divider.
            // Shadow-casting is the leak contract's whole point: without
            // probe-visibility (Chebyshev) AND light shadowing, its red would
            // reach dark-side probes straight through the divider.
            {
                m_RedLight = scene.CreateEntity("Red Point Light");
                m_RedLight.GetComponent<TransformComponent>().Translation = { -5.0f, 3.0f, 0.0f };
                auto& pl = m_RedLight.AddComponent<PointLightComponent>();
                pl.m_Color = { 1.0f, 0.2f, 0.1f };
                pl.m_Intensity = 8.0f;
                pl.m_Range = 15.0f;
                pl.m_Attenuation = 1.0f; // the manual rig's value (default is 2)
                pl.m_CastShadows = true;
            }

            // Box helper: cube primitive with an explicit MeshSource (the
            // MeshComponent draw loop bails without one — this mirrors what
            // the scene deserializer does for primitive meshes) plus a rough
            // diffuse material. All rig geometry is CUBES on purpose: cube
            // MeshComponents are verified to reach the DDGI caster sites
            // (the live bring-up counted exactly these 5 casters), while
            // sphere primitives go through a submission path that is not yet
            // wired for DDGI capture.
            auto addBox = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale,
                                   const glm::vec3& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f); // diffuse bounce surfaces
            };

            // Positions/scales/albedos are the manual rig's (DDGITest.olo) —
            // plus a ceiling and both z-end walls the manual rig lacks. The
            // enclosure is load-bearing for the leak thresholds: with an open
            // roof, sky-miss texels flood every probe with a neutral ambient
            // pedestal (~42/255 measured on the first calibration run) that
            // compresses the lit:dark ratio to ~1.5x even though the divider
            // blocks the red light perfectly. A sky-tight box makes the dark
            // side genuinely dark, which is the contract being pinned.
            addBox("Floor", { 0.0f, 0.0f, 0.0f }, { 20.0f, 0.1f, 20.0f }, { 0.5f, 0.5f, 0.5f });
            addBox("Left Wall", { -8.0f, 3.0f, 0.0f }, { 0.2f, 6.0f, 12.0f }, { 0.9f, 0.2f, 0.1f });
            addBox("Right Wall", { 8.0f, 3.0f, 0.0f }, { 0.2f, 6.0f, 12.0f }, { 0.1f, 0.2f, 0.9f });
            addBox("Back Wall", { 0.0f, 3.0f, -6.0f }, { 16.0f, 6.0f, 0.2f }, { 0.9f, 0.9f, 0.9f });
            addBox("Front Wall", { 0.0f, 3.0f, 6.0f }, { 16.0f, 6.0f, 0.2f }, { 0.9f, 0.9f, 0.9f });
            addBox("Ceiling", { 0.0f, 6.0f, 0.0f }, { 16.0f, 0.2f, 12.0f }, { 0.9f, 0.9f, 0.9f });
            // The divider: the leak test. It now meets the ceiling (y 0..6)
            // and both end walls (z -5..5 against the z=+-6 walls leaves only
            // thin slits at the ends, closed enough that the Chebyshev
            // visibility term must do the real work).
            addBox("Divider Wall", { 0.0f, 3.0f, 0.0f }, { 0.3f, 6.0f, 12.0f }, { 0.85f, 0.85f, 0.85f });

            // The Realtime DDGI probe volume covering the room interior.
            {
                Entity volume = scene.CreateEntity("Probe Volume");
                auto& lpv = volume.AddComponent<LightProbeVolumeComponent>();
                lpv.m_BoundsMin = { -7.0f, 0.5f, -5.0f };
                lpv.m_BoundsMax = { 7.0f, 5.5f, 5.0f };
                lpv.m_Resolution = { 4, 3, 4 }; // 48 probes (kTotalProbes)
                lpv.m_Active = true;
                lpv.m_Mode = LightProbeVolumeComponent::Mode::Realtime;
                // 256 rays snaps to the 16x16 octahedral hit cache
                // (DDGI::HitCacheResolutionForRayCount) — the rig's authored value.
                lpv.m_RaysPerProbe = 256;
                // 0.5 instead of the authored 0.9: the EMA residual halves
                // every frame, so kConvergenceFrames (60) converges to a
                // fixed point from any prior atlas state — fast, order-
                // independent test convergence. The input is noise-free
                // (fixed cached directions), so low hysteresis costs nothing.
                lpv.m_Hysteresis = 0.5f;
                // 8 probes/frame => full 48-probe coverage in 6 frames.
                lpv.m_ProbeCaptureBudget = 8;
                // 0 = relight EVERY probe each frame — lighting responds
                // within one frame, the MovingLightRelight prerequisite.
                lpv.m_RelightBudget = 0;
                lpv.m_SelfShadowBias = 0.3f; // component default, rig value
            }
        }

        // Editor camera posed at `eye` looking at `target` (fly-camera pose;
        // SetPose is the only setter that rebuilds the view immediately).
        [[nodiscard]] EditorCamera MakePosedCamera(const glm::vec3& eye, const glm::vec3& target) const
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f,
                                1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            const YawPitch yp = LookAtYawPitch(eye, target);
            camera.SetPose(eye, yp.Yaw, yp.Pitch);
            return camera;
        }

        // Run the full editor render path for `frames` ticks from the pose —
        // used to advance the DDGI capture/relight/EMA before a capture.
        void Converge(const glm::vec3& eye, const glm::vec3& target, u32 frames)
        {
            const EditorCamera camera = MakePosedCamera(eye, target);
            RunEditorFrames(camera, frames);
        }

        // Render 2 frames from the pose (RunEditorFrames wraps each tick in a
        // GLStateGuard so nothing leaks to later GPU tests), read back the
        // final composited frame (the same UIComposite image the editor
        // viewport shows — after tone-mapping), and flip it so row 0 is the
        // TOP (GL reads back bottom-up; the band helpers and stbi_write_png
        // are top-down).
        void CaptureView(const char* tag, const glm::vec3& eye, const glm::vec3& target,
                         std::vector<u8>& outPixels)
        {
            const EditorCamera camera = MakePosedCamera(eye, target);
            RunEditorFrames(camera, 2);

            // Mirror EditorLayer's resolve order: UIComposite, then
            // ToneMapColor, then SceneColor as a last resort.
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
        }

        // Golden-image model (WaterVisualEvidenceTest's): rebase mode
        // (OLOENGINE_GOLDEN_REBASE=1) (re)writes the PNG; a normal run
        // COMPARES against the committed golden (RMSE) and never writes.
        void CompareOrRebaseGolden(const std::string& fileName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / fileName).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.generic_string()
                                 << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with OLOENGINE_GOLDEN_REBASE=1 to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight
                                     << " — rerun with OLOENGINE_GOLDEN_REBASE=1.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "'" << fileName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "OLOENGINE_GOLDEN_REBASE=1 to update " << path;
        }
    };

    // -------------------------------------------------------------------------
    // 1. Pipeline contract: the amortized capture covers every probe and
    //    classification keeps the interior probes Active. No pixels.
    // -------------------------------------------------------------------------
    TEST_F(DDGIVisualEvidenceTest, ProbePipelineCapturesAndClassifies)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Runtime path (Scene::OnUpdateRuntime via the primary camera) —
        // ProcessScene3DSharedLogic submits the DDGI volume + cube casters
        // every frame, and the pass captures 8 probes/frame.
        RunFrames(kContractFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr) << "Renderer3D::GetDDGIPass() is null — DDGI pass not registered "
                                    "in the deferred render graph";

        EXPECT_TRUE(pass->RanThisFrame())
            << "DDGI pass did not execute on the last frame — the Realtime volume was not "
               "submitted (check RendererSettings::EnableDDGI / Deferred.EnableLightProbes)";

        // 48 probes / budget 8 = 6 frames to first-capture everything;
        // kContractFrames (40) is generous headroom (see constant derivation).
        EXPECT_FLOAT_EQ(pass->GetCapturedFraction(), 1.0f)
            << "Capture schedule did not cover all " << kTotalProbes << " probes in "
            << kContractFrames << " frames";

        const auto& records = pass->GetProbeRecords();
        ASSERT_EQ(records.size(), static_cast<std::size_t>(kTotalProbes))
            << "Probe record count does not match the submitted 4x3x4 grid";

        i32 uncaptured = 0, active = 0, inactive = 0;
        for (const auto& record : records)
        {
            switch (record.State)
            {
                case DDGI::ProbeState::Uncaptured:
                    ++uncaptured;
                    break;
                case DDGI::ProbeState::Active:
                    ++active;
                    break;
                case DDGI::ProbeState::Inactive:
                    ++inactive;
                    break;
            }
        }

        std::ostringstream histogram;
        histogram << "Probe state histogram: Uncaptured=" << uncaptured << " Active=" << active
                  << " Inactive=" << inactive << " (of " << kTotalProbes << ")";
        std::cout << "[DDGI] " << histogram.str() << "\n";

        EXPECT_EQ(uncaptured, 0) << histogram.str();

        // The volume's y=0.5 bottom layer sits just above the floor and its
        // walls-adjacent probes may relocate into/near geometry and classify
        // Inactive (backface fraction > 25%) — allow up to 8 of 48, but the
        // bulk of the grid is open interior air and must be Active.
        EXPECT_GE(active, 40) << histogram.str();
    }

    // -------------------------------------------------------------------------
    // 2. Leak regression: the dark half of the divided room must not glow red.
    // -------------------------------------------------------------------------
    TEST_F(DDGIVisualEvidenceTest, LightLeakRegression)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Converge capture + relight + EMA (see kConvergenceFrames derivation).
        Converge(kDarkEye, kDarkTarget, kConvergenceFrames);

        std::vector<u8> darkPixels;
        CaptureView("leak_dark", kDarkEye, kDarkTarget, darkPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        std::vector<u8> litPixels;
        CaptureView("leak_lit", kLitEye, kLitTarget, litPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        const BandStats dark = SampleBand(darkPixels, kBandX0, kBandX1, kBandY0, kBandY1);
        const BandStats lit = SampleBand(litPixels, kBandX0, kBandX1, kBandY0, kBandY1);
        const f64 darkMean = MeanChannel(dark);
        const f64 litMean = MeanChannel(lit);

        // Calibration printout (the photometric-parity rule: band tolerances
        // are calibrated from in-suite measurements, so always print them).
        std::cout << "[DDGI] leak bands — lit mean RGB=(" << lit.R << ", " << lit.G << ", " << lit.B
                  << ") mean=" << litMean << "; dark mean RGB=(" << dark.R << ", " << dark.G << ", "
                  << dark.B << ") mean=" << darkMean << "; ratio="
                  << (darkMean > 0.0 ? litMean / darkMean : std::numeric_limits<f64>::infinity())
                  << "\n";

        // A dead render would make the ratio assertion trivially true — the
        // lit side must actually be lit.
        EXPECT_GT(litMean, kNearBlackFloor)
            << "Lit-side band is near-black — the render produced nothing. See ddgi_leak_lit.png";

        // Contrast in LINEAR luminance (see DisplayMeanToLinear).
        EXPECT_GE(DisplayMeanToLinear(litMean), kLitOverDarkMinLinearRatio * DisplayMeanToLinear(darkMean))
            << "Lit/dark linear contrast collapsed (lit=" << litMean << " dark=" << darkMean
            << " display; linear ratio "
            << DisplayMeanToLinear(litMean) / std::max(DisplayMeanToLinear(darkMean), 1e-9)
            << ") — light is reaching the far side of the divider. See ddgi_leak_dark.png";

        // The red light must not leak: any dark-side light is color-neutral
        // (sun / white-wall bounce), so red must not exceed the other
        // channels. The lit side is the positive control.
        const f64 darkRedExcess = dark.R - std::max(dark.G, dark.B);
        const f64 litRedExcess = lit.R - std::max(lit.G, lit.B);
        EXPECT_LT(darkRedExcess, kDarkRedExcessCeiling)
            << "Dark-side red excess " << darkRedExcess << " >= " << kDarkRedExcessCeiling
            << " (0..255) — the red point light is leaking through the divider. "
               "See ddgi_leak_dark.png";
        EXPECT_GT(litRedExcess, kLitRedExcessFloor)
            << "Lit-side red excess " << litRedExcess << " <= " << kLitRedExcessFloor
            << " — the instrument cannot see the red light at all; the leak check above "
               "is not meaningful. See ddgi_leak_lit.png";

        CompareOrRebaseGolden("ddgi_leak_dark.png", darkPixels);
        if (::testing::Test::HasFatalFailure())
            return;
        CompareOrRebaseGolden("ddgi_leak_lit.png", litPixels);
    }

    // -------------------------------------------------------------------------
    // 3. The ADR's headline property: per-frame relighting of the STATIC
    //    hit-point cache responds to a moving light — no recapture involved
    //    (geometry is unchanged, so the cached hit points stay valid).
    // -------------------------------------------------------------------------
    TEST_F(DDGIVisualEvidenceTest, MovingLightRelight)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Converge with the light at its authored position (-5,3,0).
        Converge(kDarkEye, kDarkTarget, kConvergenceFrames);

        std::vector<u8> pixels;
        CaptureView("relight_dark_before", kDarkEye, kDarkTarget, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const f64 darkBefore = MeanChannel(SampleBand(pixels, kBandX0, kBandX1, kBandY0, kBandY1));

        CaptureView("relight_lit_before", kLitEye, kLitTarget, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const f64 litBefore = MeanChannel(SampleBand(pixels, kBandX0, kBandX1, kBandY0, kBandY1));

        // The "drops below half" assertion is only meaningful against a
        // genuinely lit baseline.
        ASSERT_GT(litBefore, kNearBlackFloor)
            << "Lit-side baseline is near-black (" << litBefore << ") — cannot measure a drop";

        // Move the red light to the mirrored position on the other side of
        // the divider — a plain transform write, no component dirtying.
        m_RedLight.GetComponent<TransformComponent>().Translation = { 5.0f, 3.0f, 0.0f };

        // Relight reads the light UBO fresh every frame and RelightBudget=0
        // relights every probe, so the radiance responds on the very next
        // frame; the EMA (hysteresis 0.5, further cut by the big-change
        // response in DDGI::AdjustHysteresis) converges in ~10 frames.
        // Captures are NOT needed — the geometry did not move. 60 frames is
        // the same any-state convergence bound as everywhere else.
        Converge(kDarkEye, kDarkTarget, kConvergenceFrames);

        CaptureView("relight_dark_after", kDarkEye, kDarkTarget, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const f64 darkAfter = MeanChannel(SampleBand(pixels, kBandX0, kBandX1, kBandY0, kBandY1));

        CaptureView("relight_lit_after", kLitEye, kLitTarget, pixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const f64 litAfter = MeanChannel(SampleBand(pixels, kBandX0, kBandX1, kBandY0, kBandY1));

        std::cout << "[DDGI] relight bands — formerly-dark: " << darkBefore << " -> " << darkAfter
                  << "; formerly-lit: " << litBefore << " -> " << litAfter << "\n";

        // The formerly dark side is now directly lit: at least 3x its old
        // LINEAR mean, plus an absolute floor so a black->black "rise" can't
        // pass. (Display-space ratios understate the change — see
        // DisplayMeanToLinear.)
        EXPECT_GT(darkAfter, kNearBlackFloor)
            << "Formerly-dark side stayed near-black after the light moved to (+5,3,0)";
        EXPECT_GE(DisplayMeanToLinear(darkAfter), kRelightRiseFactorLinear * DisplayMeanToLinear(darkBefore))
            << "Formerly-dark side did not brighten after the light moved (before=" << darkBefore
            << " after=" << darkAfter << " display) — per-frame relighting is not responding";

        // The formerly lit side keeps only the 0.05 sun + decayed history +
        // the neutral GI pedestal. Stale (un-relit) probe irradiance would
        // keep its red glow alive.
        EXPECT_LT(DisplayMeanToLinear(litAfter), kRelightDropFactorLinear * DisplayMeanToLinear(litBefore))
            << "Formerly-lit side did not go dark after the light moved (before=" << litBefore
            << " after=" << litAfter << " display) — stale probe irradiance is not decaying";
    }

    // -------------------------------------------------------------------------
    // 4. Multi-angle goldens after convergence (lit / dark / top-down).
    // -------------------------------------------------------------------------
    TEST_F(DDGIVisualEvidenceTest, MultiAngleGoldens)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Fresh scene per test — the light is back at (-5,3,0); 60 frames
        // re-converge the process-global atlases from whatever the previous
        // test left behind (goldens stay order-independent).
        Converge(kLitEye, kLitTarget, kConvergenceFrames);

        struct Angle
        {
            const char* GoldenName;
            glm::vec3 Eye;
            glm::vec3 Target;
            // Whether the pose frames directly-lit content (near-black guard).
            // The dark angle is dim BY DESIGN — a floor there would be a
            // tautology or a flake, so it relies on LightLeakRegression's
            // ratio assertions instead.
            bool ExpectLit;
        };

        const Angle angles[] = {
            { "ddgi_angle_lit.png", kLitEye, kLitTarget, true },
            { "ddgi_angle_dark.png", kDarkEye, kDarkTarget, false },
            { "ddgi_angle_top.png", kTopEye, kTopTarget, true },
        };

        for (const Angle& angle : angles)
        {
            SCOPED_TRACE(angle.GoldenName);

            std::vector<u8> pixels;
            CaptureView(angle.GoldenName, angle.Eye, angle.Target, pixels);
            if (::testing::Test::HasFatalFailure())
                return;

            if (angle.ExpectLit)
            {
                const f64 mean =
                    MeanChannel(SampleBand(pixels, kBandX0, kBandX1, kBandY0, kBandY1));
                EXPECT_GT(mean, kNearBlackFloor)
                    << "Pose rendered (near-)black — see " << angle.GoldenName;
            }

            CompareOrRebaseGolden(angle.GoldenName, pixels);
            if (::testing::Test::HasFatalFailure())
                return;
        }
    }
} // namespace OloEngine::Tests
