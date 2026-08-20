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
//   5. ForwardPlusPathSmoke — the same rig on RenderingPath::ForwardPlus:
//      the pass must run, capture must cover the grid, and the lit/dark band
//      asymmetry must hold (no goldens). This pins the Forward+ wiring
//      directly — the shared LightProbeSampling.glsl sampler carries the
//      FORMULAS across paths, but the forward path's enable flags, atlas
//      bindings and graph registration are their own seams and get their own
//      GPU coverage here.
//
//   6-9. DDGICascadeEvidenceTest — issue #707's camera-centred cascades on a
//      corridor rig with NO authored probe volume: coverage in every cascade,
//      the measured live-vs-total probe counts sparsity is claimed to cut,
//      temporal stability of the converged field, and multi-angle evidence
//      PNGs (evidence, not goldens — the cascade windows follow the camera).
//
// Rendering path: DEFERRED for tests 1-4 (the primary GI consumer —
// DeferredLightingShared.glsl samples the DDGI atlases in its ambient
// ladder); test 5 switches to ForwardPlus for the frame-level smoke.
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
//   (RMSE <= 6.0) and writes nothing; --olo-golden-rebase (re)writes.
//   Run from OloEditor/ so the PNGs land under OloEditor/assets/tests/visual/.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"
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

#include <algorithm>
#include <array>
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
        // Calibration run (enclosed rig, RTX 4090): lit=68.44 dark=36.85
        // display => 1.86x display == 3.90x linear. (Pre-#751, with the
        // infinite-bounce term dead: lit=60.27 dark=32.82 => 3.81x linear.
        // Both halves gained ~13% display when the bounce came alive and the
        // CONTRAST barely moved, which is the reassuring result: this room's
        // 0.9-albedo walls interreflect hard, so a fix that leaked light past
        // the divider would have shown up here as the ratio collapsing.)
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
        // display grey levels (measured +2.9 neutral noise on the calibration
        // run; +2.2 pre-#751), while the lit side must show a strong red
        // excess (> 15; measured +43.5, was +33.7) as the positive control
        // that the instrument can actually see redness.
        //
        // These two are the numbers to watch when the GI gets brighter:
        // turning the infinite-bounce term on (#751) raised the dark side's
        // red excess by 0.7 grey levels while nearly tripling the margin on
        // the lit side, so the leak contract got RELATIVELY stronger, not
        // weaker. Neither threshold was touched.
        constexpr f64 kDarkRedExcessCeiling = 5.0;
        constexpr f64 kLitRedExcessFloor = 15.0;

        // --- Test 3 thresholds ----------------------------------------------
        // After the light moves to (+5,3,0), the formerly dark half is
        // directly lit (same intensity-8 light, mirrored geometry) — at least
        // 3x its old LINEAR mean (measured 3.68x: the pre-move mean already
        // contains the neutral GI pedestal, which survives the swap). The
        // formerly lit half keeps only the sun + pedestal: at most 0.5x its
        // old LINEAR mean (measured 0.25x). Stale (un-relit) probe irradiance
        // would keep its red glow alive and fail the drop. Both figures are
        // post-#751 and moved by under 2% from the dead-bounce measurements
        // (3.67x / 0.27x) — relight latency is a property of the pass, not of
        // how much light is in the room.
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
            return OloEngine::Tests::Options().GoldenRebase;
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

        // Mean LINEAR RGB of one probe's 6x6 irradiance-atlas INTERIOR
        // (border excluded), read straight from the pass's live FP16 atlas.
        // This measures probe irradiance ONLY — no direct lighting, no
        // camera, no tone mapping — the bounce-isolated instrument the
        // relight contract needs (the composited band means also carry the
        // direct term, which responds per frame regardless of DDGI).
        [[nodiscard]] glm::dvec3 ReadIrradianceTileMeanLinear(const glm::ivec3& probeCoord)
        {
            auto* pass = Renderer3D::GetDDGIPass();
            EXPECT_NE(pass, nullptr) << "DDGI pass missing for atlas readback";
            if (!pass)
                return glm::dvec3(0.0);
            const RHI::ResourceHandle atlas = pass->GetIrradianceAtlasID();
            EXPECT_TRUE(atlas.IsValid()) << "Irradiance atlas not created yet — pass never ran?";
            if (!atlas.IsValid())
                return glm::dvec3(0.0);
            // Raw readback below, so the test needs the driver name. This is a
            // TEST, not the sweep bucket — Debug::NativeTextureIdForDiagnostics
            // is the sanctioned way to ask.
            const u32 atlasID = Debug::NativeTextureIdForDiagnostics(atlas);

            constexpr glm::ivec3 kDims{ 4, 3, 4 }; // the rig's grid (kTotalProbes)
            const i32 probeIdx = DDGI::ProbeLinearIndex(probeCoord, kDims);
            const glm::ivec2 tileOrigin = DDGI::ProbeTileCoord(probeIdx, kDims) * DDGI::kIrradianceTileTexels;
            constexpr i32 kInner = DDGI::kIrradianceInteriorTexels;

            std::vector<f32> texels(static_cast<sizet>(kInner) * kInner * 4u);
            glGetTextureSubImage(atlasID, 0, tileOrigin.x + 1, tileOrigin.y + 1, 0, kInner, kInner, 1,
                                 GL_RGBA, GL_FLOAT,
                                 static_cast<GLsizei>(texels.size() * sizeof(f32)), texels.data());

            glm::dvec3 sum(0.0);
            for (i32 i = 0; i < kInner * kInner; ++i)
            {
                sum += glm::dvec3(texels[static_cast<sizet>(i) * 4u + 0u],
                                  texels[static_cast<sizet>(i) * 4u + 1u],
                                  texels[static_cast<sizet>(i) * 4u + 2u]);
            }
            return sum / static_cast<f64>(kInner * kInner);
        }

        // Golden-image model (WaterVisualEvidenceTest's): rebase mode
        // (--olo-golden-rebase) (re)writes the PNG; a normal run
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
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight
                                     << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "'" << fileName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
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

        // Issue #707: relocation offsets and classification are written by the
        // GPU relocation compute, so the CPU-side records need one explicit
        // sync before they carry anything. The frame never does this — that is
        // the point of the upgrade — so a reader has to ask.
        pass->ReadbackProbeDiagnostics();

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

        // Bounce-isolated PRIMARY instrument: the irradiance ATLAS itself.
        // Probe (0,1,1) sits mid-height in the lit (-X) half, (3,1,1) in the
        // dark (+X) half. Note on recapture: the continuous refresh DOES
        // re-capture ~1 probe/frame throughout this test, and that is
        // harmless BY CONSTRUCTION — capture stores geometry only
        // (albedo / normal / distance); lighting enters the cache exclusively
        // through the per-frame relight, so recapture cannot confound the
        // cached-hit-relight contract being pinned here.
        const glm::dvec3 litProbeBefore = ReadIrradianceTileMeanLinear({ 0, 1, 1 });
        const glm::dvec3 darkProbeBefore = ReadIrradianceTileMeanLinear({ 3, 1, 1 });
        ASSERT_GT(litProbeBefore.r, 1e-4)
            << "Lit-half probe carries no red irradiance before the move — the atlas "
               "instrument is not seeing the light at all";

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

        // --- Bounce-isolated PRIMARY contract: the atlas red irradiance must
        // SWAP sides. Direct lighting never enters the atlas, so unlike the
        // composited band means above these cannot pass on direct-light
        // response alone.
        const glm::dvec3 litProbeAfter = ReadIrradianceTileMeanLinear({ 0, 1, 1 });
        const glm::dvec3 darkProbeAfter = ReadIrradianceTileMeanLinear({ 3, 1, 1 });
        std::cout << "[DDGI] atlas red irradiance — lit-half probe: " << litProbeBefore.r << " -> "
                  << litProbeAfter.r << "; dark-half probe: " << darkProbeBefore.r << " -> "
                  << darkProbeAfter.r << "\n";

        EXPECT_GT(litProbeBefore.r, 3.0 * darkProbeBefore.r)
            << "Before the move, the lit-half probe should dominate in red irradiance";
        EXPECT_GT(darkProbeAfter.r, 3.0 * litProbeAfter.r)
            << "After the move, atlas red irradiance did not swap to the (+X) half — the "
               "cached hit points are not being relit";
        EXPECT_LT(litProbeAfter.r, 0.5 * litProbeBefore.r)
            << "The formerly-lit probe's red irradiance did not decay — stale atlas content";
        EXPECT_GT(darkProbeAfter.r, 2.0 * darkProbeBefore.r)
            << "The formerly-dark probe's red irradiance did not rise — the relight is not "
               "propagating the moved light through the cached hits";
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

    // -------------------------------------------------------------------------
    // 5. Forward+ smoke: the same rig on RenderingPath::ForwardPlus. The
    //    shared LightProbeSampling.glsl sampler carries the FORMULAS across
    //    paths, but the forward path has its own seams — the per-material
    //    EnableLightProbes flag (a dead hard-coded 0 before #632), the global
    //    atlas bindings surviving into ScenePass's forward shaders, and the
    //    DDGI pass registration on a non-deferred graph — so it gets direct
    //    GPU coverage. Band asymmetry only, no goldens (LightLeakRegression
    //    owns the pixel-exact evidence on the primary path).
    // -------------------------------------------------------------------------
    TEST_F(DDGIVisualEvidenceTest, ForwardPlusPathSmokeAsymmetry)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        // Switch the path for this test only (the fixture snapshots/restores
        // RendererSettings per test). ApplyRendererSettings rebuilds the
        // render graph on a path change.
        Renderer3D::GetRendererSettings().Path = RenderingPath::ForwardPlus;
        Renderer3D::ApplyRendererSettings();

        Converge(kDarkEye, kDarkTarget, kConvergenceFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr) << "DDGI pass not reachable on the Forward+ graph";
        EXPECT_TRUE(pass->RanThisFrame())
            << "DDGI pass did not execute on the Forward+ path — registration is "
               "deferred-only or the volume submission is gated on the path";
        EXPECT_FLOAT_EQ(pass->GetCapturedFraction(), 1.0f)
            << "Capture schedule did not cover the grid on the Forward+ path";

        std::vector<u8> darkPixels;
        CaptureView("fplus_dark", kDarkEye, kDarkTarget, darkPixels);
        if (::testing::Test::HasFatalFailure())
            return;
        std::vector<u8> litPixels;
        CaptureView("fplus_lit", kLitEye, kLitTarget, litPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        const BandStats dark = SampleBand(darkPixels, kBandX0, kBandX1, kBandY0, kBandY1);
        const BandStats lit = SampleBand(litPixels, kBandX0, kBandX1, kBandY0, kBandY1);
        const f64 darkMean = MeanChannel(dark);
        const f64 litMean = MeanChannel(lit);
        std::cout << "[DDGI] Forward+ bands — lit mean=" << litMean << " dark mean=" << darkMean
                  << "\n";

        EXPECT_GT(litMean, kNearBlackFloor)
            << "Forward+ lit side is near-black — the forward probe/lighting path produced "
               "nothing (check PBRMaterialUBO.EnableLightProbes wiring)";
        EXPECT_GE(DisplayMeanToLinear(litMean), kLitOverDarkMinLinearRatio * DisplayMeanToLinear(darkMean))
            << "Forward+ lit/dark linear contrast collapsed (lit=" << litMean
            << " dark=" << darkMean << " display) — leak or dead GI on the forward path";
        const f64 litRedExcess = lit.R - std::max(lit.G, lit.B);
        EXPECT_GT(litRedExcess, kLitRedExcessFloor)
            << "Forward+ lit side shows no red excess (" << litRedExcess
            << ") — the red light is not reaching the forward lit pass";
    }

    // =========================================================================
    // Issue #707 — camera-centred probe cascades, request-driven sparsity and
    // the variable update rate.
    //
    // A separate fixture from the authored-volume rig above, because the whole
    // point is that this scene has NO LightProbeVolumeComponent: the cascades
    // are what places the probe field, and an authored volume would take
    // precedence and stop the tests testing anything.
    //
    // The rig is a corridor 60 m long on Z with a coloured bounce panel inside
    // each cascade window. Cascade N covers 2^N times cascade 0's extent, so a
    // camera at the near end sees a panel in every cascade — which is what
    // makes "GI is continuous across a large scene with no authored probe
    // volume" (acceptance criterion 1) something a test can look at rather than
    // a claim.
    //
    // Classification: L8 / integration (full GL pipeline + readback + PNG).
    // =========================================================================
    class DDGICascadeEvidenceTest : public RendererAttachedTest
    {
      protected:
        // 4 x 8^3 = 2048 probes at 3 m base spacing, so the cascade windows
        // reach +-10.5 m, +-21 m, +-42 m and +-84 m from the camera.
        //
        // THE FIELD MUST OUT-REACH THE CORRIDOR, and getting that wrong is how
        // this rig first read as a bug: at 3 cascades x 2 m the outermost
        // window stopped at +-28 m of a 68 m corridor, so GI correctly faded
        // out two thirds of the way down and the evidence PNG looked exactly
        // like a cascade that had failed to converge. A rig that cannot tell
        // "the field ends here" from "the field is broken here" is not evidence
        // for "GI is continuous across a large scene" — it is a picture of the
        // field's own boundary.
        static constexpr i32 kCascadeCount = 4;
        static constexpr i32 kCascadeRes = 8;
        static constexpr f32 kBaseSpacing = 3.0f;
        static constexpr i32 kProbesPerCascade = kCascadeRes * kCascadeRes * kCascadeRes;
        // Deliberately NOT `kTotalProbes`: the anonymous namespace already has one
        // (the authored rig's 48), and a fixture member that shadows it makes every
        // reference here ambiguous to a reader even though the compiler is happy.
        static constexpr i32 kCascadeTotalProbes = kCascadeCount * kProbesPerCascade;

        // Long enough for the capture schedule to reach the far cascades AND
        // for the relocation spring's follow-up captures to drain, since a
        // probe only counts as Active once its relocation compute has run.
        static constexpr u32 kConvergeFrames = 140;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Deferred;
            settings.EnableDDGI = true;
            settings.Deferred.EnableLightProbes = true;
            // The feature under test. The fixture snapshots and restores
            // RendererSettings per test, so this cannot leak into the authored
            // rig above (which would silently stop testing the authored path).
            settings.DDGICascadesEnabled = true;
            settings.DDGICascadeCount = kCascadeCount;
            settings.DDGICascadeResolution = kCascadeRes;
            settings.DDGICascadeBaseSpacing = kBaseSpacing;
            settings.DDGICascadeBlendBand = 0.2f;
            settings.DDGISparsityEnabled = true;
            // 1-in-1 for the tests: the update rate is pinned as an exact
            // partition by DDGIMath.UpdateRatePartitionsProbesExactlyOncePerPeriod,
            // and throttling here would only make convergence slower to
            // observe without testing anything the L1 test does not.
            settings.DDGIUpdateRateDivisor = 1;
            // 6 m, not the 10 m this rig started with: the seed is a floor, and
            // an over-generous one silently becomes the dominant request source
            // — it marked so much of a deliberately small field that the
            // measured live fraction was really measuring the seed rather than
            // the screen.
            settings.DDGICameraSeedRadius = 6.0f;
            // Scene derives the cascaded capture budget as 16 * DDGIBudgetScale.
            settings.DDGIBudgetScale = 2.0f;
            Renderer3D::ApplyRendererSettings();

            // Gizmos are drawn into the composited frame these tests read back,
            // so the light and world-axis helpers sit on top of the GI evidence.
            //
            // The RendererSettings flags are NOT enough: Scene keeps its own
            // m_ShowLightGizmos / m_ShowWorldAxisHelper (both default true) and
            // only EditorLayer pushes the settings into them — which this
            // fixture never runs. Setting the renderer flags alone leaves the
            // gizmos on and looks like the toggle did not work.
            scene.SetLightGizmosVisible(false);
            scene.SetWorldAxisHelperVisible(false);

            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.TAAEnabled = false;
            pp.AutoExposureEnabled = false;

            {
                Entity camera = scene.CreateEntity("Camera");
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 3.0f, 6.0f };
                auto& cc = camera.AddComponent<CameraComponent>();
                cc.Primary = true;
                cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            }

            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 0.05f;
                dl.m_CastShadows = true;
            }

            // Two key lights, one near and one far, so every cascade has real
            // light to bounce rather than inheriting the near cascade's.
            const auto addPointLight = [&scene](const char* name, const glm::vec3& pos, bool shadows)
            {
                Entity e = scene.CreateEntity(name);
                e.GetComponent<TransformComponent>().Translation = pos;
                auto& pl = e.AddComponent<PointLightComponent>();
                pl.m_Color = { 1.0f, 0.95f, 0.85f };
                pl.m_Intensity = 25.0f;
                pl.m_Range = 30.0f;
                pl.m_Attenuation = 1.0f;
                pl.m_CastShadows = shadows;
            };
            addPointLight("Near Key", { 0.0f, 5.0f, 2.0f }, true);
            addPointLight("Far Key", { 0.0f, 5.0f, -35.0f }, false);

            // CUBES only: cube MeshComponents are the submission path verified
            // to reach the DDGI caster sites (see the authored rig's note).
            const auto addBox = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale,
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
                mat.m_Material.SetRoughnessFactor(0.9f);
            };

            // A CLOSED corridor: as in the authored rig, an open roof floods
            // every probe with a sky pedestal and washes out exactly the
            // contrast these tests measure.
            addBox("Floor", { 0.0f, 0.0f, -25.0f }, { 12.0f, 0.2f, 70.0f }, { 0.6f, 0.6f, 0.6f });
            addBox("Ceiling", { 0.0f, 7.0f, -25.0f }, { 12.0f, 0.2f, 70.0f }, { 0.75f, 0.75f, 0.75f });
            addBox("Left Wall", { -6.0f, 3.5f, -25.0f }, { 0.3f, 7.0f, 70.0f }, { 0.8f, 0.8f, 0.8f });
            addBox("Right Wall", { 6.0f, 3.5f, -25.0f }, { 0.3f, 7.0f, 70.0f }, { 0.8f, 0.8f, 0.8f });
            addBox("Near Wall", { 0.0f, 3.5f, 9.0f }, { 12.0f, 7.0f, 0.3f }, { 0.8f, 0.8f, 0.8f });
            addBox("Far Wall", { 0.0f, 3.5f, -59.0f }, { 12.0f, 7.0f, 0.3f }, { 0.8f, 0.8f, 0.8f });

            // One saturated bounce panel per cascade window.
            addBox("Panel Cascade0", { -5.5f, 3.0f, 0.0f }, { 0.3f, 5.0f, 6.0f }, { 0.9f, 0.12f, 0.08f });
            addBox("Panel Cascade1", { 5.5f, 3.0f, -12.0f }, { 0.3f, 5.0f, 6.0f }, { 0.12f, 0.85f, 0.15f });
            addBox("Panel Cascade2", { -5.5f, 3.0f, -30.0f }, { 0.3f, 5.0f, 6.0f }, { 0.08f, 0.2f, 0.95f });
        }

        [[nodiscard]] EditorCamera MakePosedCamera(const glm::vec3& eye, const glm::vec3& target) const
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            const YawPitch yp = LookAtYawPitch(eye, target);
            camera.SetPose(eye, yp.Yaw, yp.Pitch);
            return camera;
        }

        void Converge(const glm::vec3& eye, const glm::vec3& target, u32 frames)
        {
            RunEditorFrames(MakePosedCamera(eye, target), frames);
        }

        void CaptureView(const char* tag, const glm::vec3& eye, const glm::vec3& target,
                         std::vector<u8>& outPixels)
        {
            RunEditorFrames(MakePosedCamera(eye, target), 2);

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

        // Per-cascade probe-state histogram, from the pass's explicit
        // diagnostics sync (issue #707: the frame itself never reads back).
        struct CascadeHistogram
        {
            std::array<i32, 8> Active{};
            std::array<i32, 8> Inactive{};
            std::array<i32, 8> Uncaptured{};
        };

        [[nodiscard]] static CascadeHistogram ReadCascadeHistogram(const DDGIProbeUpdatePass& pass)
        {
            pass.ReadbackProbeDiagnostics();
            CascadeHistogram histogram{};
            const auto& records = pass.GetProbeRecords();
            const glm::ivec3 dims(kCascadeRes);
            for (std::size_t i = 0; i < records.size(); ++i)
            {
                const i32 level = std::clamp(DDGI::CascadeOfProbeIndex(static_cast<i32>(i), dims), 0, 7);
                switch (records[i].State)
                {
                    case DDGI::ProbeState::Active:
                        ++histogram.Active[static_cast<std::size_t>(level)];
                        break;
                    case DDGI::ProbeState::Inactive:
                        ++histogram.Inactive[static_cast<std::size_t>(level)];
                        break;
                    case DDGI::ProbeState::Uncaptured:
                        ++histogram.Uncaptured[static_cast<std::size_t>(level)];
                        break;
                }
            }
            return histogram;
        }

        // Unconditional PNG write — evidence, not a golden. The cascade windows
        // follow the camera, so a golden here would pin the pose as hard as it
        // pinned the result, and any camera-constant retune would read as a
        // regression. The images exist for a human (and the PR) to look at;
        // the assertions live in the tests above.
        static void WriteEvidencePng(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            EXPECT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, pixels.data(),
                                               static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            std::cout << "[ddgi-cascades] wrote " << path << "\n";
        }
    };

    // Acceptance criterion 1: GI is continuous across a large scene with NO
    // authored probe volume. "Continuous" is measured as live, captured, Active
    // probes in EVERY cascade — a field that stopped at cascade 0 would still
    // render a perfectly plausible frame, just with the far corridor lit only
    // by the ambient ladder.
    TEST_F(DDGICascadeEvidenceTest, CascadesCoverTheCorridorWithNoAuthoredVolume)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Converge({ 0.0f, 3.0f, 6.0f }, { 0.0f, 3.0f, -40.0f }, kConvergeFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr) << "the DDGI pass never ran";
        EXPECT_TRUE(pass->RanThisFrame())
            << "no cascaded field was submitted — check RendererSettings::DDGICascadesEnabled and that "
               "the scene really has no LightProbeVolumeComponent (an authored volume takes precedence)";

        EXPECT_EQ(pass->GetCascadeCount(), kCascadeCount);
        EXPECT_EQ(pass->GetTotalProbeCount(), kCascadeTotalProbes);

        // Cascade N must be twice cascade N-1's spacing, and every window must
        // be centred near the camera. Checked against the LIVE cascade table so
        // a regression in the pass's own layout is caught, not just the math
        // header's (which DDGIMathTest already pins).
        const auto& cascades = pass->GetCascades();
        for (i32 level = 1; level < kCascadeCount; ++level)
        {
            EXPECT_NEAR(cascades[static_cast<std::size_t>(level)].Spacing.x,
                        cascades[static_cast<std::size_t>(level - 1)].Spacing.x * 2.0f, 1e-3f)
                << "cascade " << level << " must be twice the previous one's spacing";
        }

        const CascadeHistogram histogram = ReadCascadeHistogram(*pass);
        for (i32 level = 0; level < kCascadeCount; ++level)
        {
            const auto l = static_cast<std::size_t>(level);
            std::cout << "[ddgi-cascades] cascade " << level
                      << ": active=" << histogram.Active[l]
                      << " inactive=" << histogram.Inactive[l]
                      << " uncaptured=" << histogram.Uncaptured[l]
                      << " (of " << kProbesPerCascade << ")\n";
            EXPECT_GT(histogram.Active[l], 0)
                << "cascade " << level << " has no Active probe — GI stops before it, which renders as a "
                                          "plausible frame lit only by the ambient ladder";
        }
    }

    // Acceptance criterion 2, the measurable half: the ACTIVE probe count in a
    // typical view is a small fraction of the dense grid. Measured, printed,
    // and asserted loosely — the exact ratio depends on the rig, but "sparsity
    // is doing nothing" (live == total) must fail.
    TEST_F(DDGICascadeEvidenceTest, SparsityKeepsTheLiveSetASmallFractionOfTheField)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Converge({ 0.0f, 3.0f, 6.0f }, { 0.0f, 3.0f, -40.0f }, kConvergeFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr);
        const DDGIProbeUpdatePass::ProbeStats stats = pass->GetProbeStats();

        std::cout << "[ddgi-cascades] probes: total=" << kCascadeTotalProbes
                  << " live=" << stats.LiveProbes
                  << " active=" << stats.ActiveProbes
                  << " relit/frame=" << stats.RelitProbes
                  << " blended/frame=" << stats.BlendedProbes
                  << " captured=" << stats.CapturedProbes
                  << " live-but-uncaptured=" << stats.UncapturedLive << "\n";
        std::cout << "[ddgi-cascades] live fraction = "
                  << (100.0 * static_cast<f64>(stats.LiveProbes) / static_cast<f64>(kCascadeTotalProbes)) << "%\n";

        EXPECT_GT(stats.LiveProbes, 0u) << "nothing requested a probe — sparsity has starved the field";
        EXPECT_LT(stats.LiveProbes, static_cast<u32>(kCascadeTotalProbes))
            << "every probe is live, so sparsity is not actually gating anything";
        // The relight set can never exceed the live set — if it does, the two
        // gates (DDGI_Relight.glsl and DDGI_ProbeMaintain.comp) disagree about
        // liveness, which is the drift the shared ddgiProbeUpdatesNow exists to
        // prevent.
        EXPECT_LE(stats.RelitProbes, stats.LiveProbes);
    }

    // The OTHER half of acceptance criterion 2, and the half that is easy to
    // quote wrongly. Sparsity alone answers "how many probes are live"; what
    // the frame actually PAYS FOR is `relit`, which is live x the update rate.
    // The tests above deliberately run at 1-in-1 so convergence is observable,
    // which excludes the second factor entirely — so it is measured here at the
    // shipping 1-in-8 instead of multiplied out on paper.
    TEST_F(DDGICascadeEvidenceTest, ShippingUpdateRateRelightsAFractionOfTheLiveSet)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        settings.DDGIUpdateRateDivisor = 8; // PGI's default, and ours
        Renderer3D::ApplyRendererSettings();

        // Longer than kConvergeFrames: at 1-in-8 a probe's EMA advances one
        // frame in eight, so the field needs proportionally longer to settle.
        const glm::vec3 eye{ 0.0f, 3.0f, 6.0f };
        const glm::vec3 target{ 0.0f, 3.0f, -40.0f };
        Converge(eye, target, kConvergeFrames * 2u);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr);

        // AVERAGE OVER A FULL PERIOD, not a single frame.
        //
        // `RelitProbes` is one frame's count, and the round-robin selects probes
        // by INDEX — so each of the 8 phases picks a different slice of a live
        // set whose indices are clustered (they are cascade-major, and the
        // cascades are not equally live). Sampling one frame therefore returns
        // anything from well under to well over the mean: measured 63 on one
        // run and 119 on another with an identical live set of 515. Averaging
        // one whole period is what actually measures "1-in-8", and it removes a
        // margin that was passing on luck.
        constexpr u32 kPeriod = 8u;
        u64 relitTotal = 0;
        u64 blendedTotal = 0;
        u32 liveSample = 0;
        for (u32 frame = 0; frame < kPeriod; ++frame)
        {
            Converge(eye, target, 1);
            const DDGIProbeUpdatePass::ProbeStats s = pass->GetProbeStats();
            relitTotal += s.RelitProbes;
            blendedTotal += s.BlendedProbes;
            liveSample = s.LiveProbes;
            EXPECT_EQ(s.BlendedProbes, s.RelitProbes)
                << "frame " << frame
                << ": the blend gate and the relight gate must select the SAME probes (ddgiProbeUpdatesNow); a "
                   "probe blended without being relit EMAs toward a radiance cache nobody is refreshing";
        }

        const f64 relitPerFrame = static_cast<f64>(relitTotal) / static_cast<f64>(kPeriod);
        const f64 livePct = 100.0 * static_cast<f64>(liveSample) / static_cast<f64>(kCascadeTotalProbes);
        const f64 relitPct = 100.0 * relitPerFrame / static_cast<f64>(kCascadeTotalProbes);
        std::cout << "[ddgi-cascades] at 1-in-8 (mean over " << kPeriod << " frames): total=" << kCascadeTotalProbes
                  << " live=" << liveSample << " (" << livePct << "%)  relit/frame=" << relitPerFrame << " ("
                  << relitPct << "% of the field)\n";

        ASSERT_GT(liveSample, 0u);
        EXPECT_EQ(relitTotal, blendedTotal);
        // Over a full period every live probe relights exactly once, so the mean
        // is live/8 by construction — assert that rather than a hand-picked
        // slack, and let the tolerance cover probes entering or leaving the live
        // set during the 8 frames.
        const f64 expected = static_cast<f64>(liveSample) / static_cast<f64>(kPeriod);
        EXPECT_NEAR(relitPerFrame, expected, 0.25 * expected)
            << "1-in-8 must relight a mean of live/8 probes per frame over one full period; measured "
            << relitPerFrame << " against an expected " << expected;
    }

    // Sparsity and the variable update rate are about WHEN probes update, so
    // their failure mode is temporal: a field that flickers or creeps looks
    // fine in any single frame. Converge, capture, run more frames from the
    // same pose, capture again, and compare.
    TEST_F(DDGICascadeEvidenceTest, CascadeFieldIsTemporallyStableFromAFixedPose)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 0.0f, 3.0f, 6.0f };
        const glm::vec3 target{ 0.0f, 3.0f, -40.0f };

        // CONVERGE UNTIL THE CAPTURE SCHEDULE HAS CAUGHT UP, not for a fixed
        // frame count. A fixed count cannot tell "still converging" from "not
        // stable", and that is not a hypothetical: at 2048 probes and a budget
        // of 32 the field needs ~200 frames, so the original fixed 140 measured
        // an RMSE of 2.96 against a 3.0 threshold — a test that would have gone
        // red on any rig change while reporting nothing about stability.
        //
        // `UncapturedLive == 0` is the honest "caught up" signal: no probe that
        // something is shading from is still waiting for its first capture.
        auto* warmupPass = Renderer3D::GetDDGIPass();
        ASSERT_NE(warmupPass, nullptr);
        constexpr u32 kMaxWarmupIntervals = 12u;
        u32 warmupFrames = 0;
        for (; warmupFrames < kMaxWarmupIntervals; ++warmupFrames)
        {
            Converge(eye, target, kConvergeFrames / 2u);
            if (warmupPass->GetProbeStats().UncapturedLive == 0u)
            {
                break;
            }
        }
        const DDGIProbeUpdatePass::ProbeStats warm = warmupPass->GetProbeStats();
        // `warmupFrames` is the index of the iteration that broke, but on the
        // exhausted path it is the loop LIMIT — so the number that actually ran
        // is min(warmupFrames + 1, limit). Adding one unconditionally
        // over-reports by a whole interval exactly when the warm-up failed,
        // i.e. in the diagnostic that matters most.
        const u32 warmupIterations = std::min(warmupFrames + 1u, kMaxWarmupIntervals);
        std::cout << "[ddgi-cascades] warmed up after " << (warmupIterations * (kConvergeFrames / 2u))
                  << " frames; live=" << warm.LiveProbes << " live-but-uncaptured=" << warm.UncapturedLive << "\n";
        EXPECT_EQ(warm.UncapturedLive, 0u)
            << "the capture schedule never caught up, so the RMSE below would measure convergence rather than "
               "stability — raise the capture budget or the frame count rather than relaxing the threshold";

        // THREE captures, not two, and the assertion is about DRIFT rather than
        // an absolute threshold.
        //
        // A single "RMSE(A,B) < k" cannot tell bounded churn from creep, and it
        // is the creep that matters: a field that keeps moving in one direction
        // under a still camera has a feedback term that does not settle, while
        // one that jitters within a bound is the capture-refresh cycle doing its
        // job (it re-captures the oldest probes forever, and each recapture
        // re-enters the visibility EMA). The first version of this test asserted
        // RMSE < 3.0 against a measured 3.5 — and 3.0 was a number I picked, not
        // one I measured, which is the mistake live-verification-noise-floor.md
        // is about. Comparing two intervals makes the pipeline supply its own
        // noise floor instead.
        // SUCCESSIVE INCREMENTS, not cumulative differences.
        //
        // Two earlier versions of this assertion measured the wrong thing, and
        // both were wrong in the same direction — they could not tell a field
        // that is still CONVERGING from one that is DRIFTING:
        //
        //   * "RMSE(t, t+45) < k" needs a k, and 3.0 was a number I picked, not
        //     one I measured. It also made the test ORDER-DEPENDENT: 3.5 inside
        //     the suite (earlier tests had warmed the process-global atlas)
        //     versus 18.6 alone. A number that depends on what ran before it is
        //     a flake with a plausible value.
        //   * "RMSE(t, t+90) < 1.6 x RMSE(t, t+45)" looks like a drift test but
        //     is not: while converging toward a fixed point the frame moves
        //     monotonically, so the cumulative difference grows linearly and the
        //     ratio sits at ~2 — identical to genuine drift.
        //
        // The discriminator is the SHAPE of the successive increments.
        // Converging shrinks them; drifting holds them constant; flickering
        // makes them noisy but non-shrinking. So the series is what gets
        // asserted, and it is printed so a reader can see the shape rather than
        // trust a single scalar.
        constexpr u32 kIntervalFrames = 45u;
        constexpr std::size_t kIntervals = 5;

        std::vector<u8> previous;
        CaptureView("cascade-stability", eye, target, previous);
        if (::testing::Test::HasFatalFailure())
            return;

        std::vector<f64> increments;
        increments.reserve(kIntervals);
        for (std::size_t i = 0; i < kIntervals; ++i)
        {
            Converge(eye, target, kIntervalFrames);
            std::vector<u8> current;
            CaptureView("cascade-stability", eye, target, current);
            if (::testing::Test::HasFatalFailure())
                return;
            ASSERT_EQ(previous.size(), current.size());
            increments.push_back(Rgba8Rmse(previous, current));
            previous.swap(current);
        }

        std::cout << "[ddgi-cascades] successive " << kIntervalFrames << "-frame RMSE increments:";
        for (const f64 v : increments)
        {
            std::cout << " " << v;
        }
        std::cout << "\n";

        const f64 largest = *std::max_element(increments.begin(), increments.end());

        // BOUNDED, not monotonically shrinking — and the difference matters.
        //
        // A cascaded field does not converge to a fixed point, and it should not
        // be asserted to: the capture scheduler re-captures the oldest probes
        // forever (the budget/8 refresh tier), so with ~515 live probes at ~4
        // refreshes per frame there is a sweep of period ~130 frames permanently
        // moving through the field. The increment series therefore oscillates
        // with roughly that period rather than decaying to zero, and demanding
        // a strictly shrinking series would be demanding the refresh cycle stop
        // existing.
        //
        // What IS required is that the oscillation stays inside the noise budget
        // the authored path already meets, and does not grow. Both together rule
        // out the failure this test is for: a request or update-rate gate that
        // disagrees with itself produces churn that neither bounds nor repeats.
        EXPECT_LT(largest, kGoldenRmseThreshold)
            << "converged-field churn exceeds the golden tolerance the authored DDGI path holds to — the field "
               "is not settling into the pipeline's own noise budget";
        EXPECT_LT(increments.back(), 1.5 * increments.front())
            << "the churn is GROWING across the series, which under a still camera means the field is diverging "
               "rather than oscillating around a settled state";
    }

    // Multi-angle evidence. NOT goldens: the cascade layout depends on the
    // camera position, so these images are for a human to read (and for the PR)
    // rather than for an RMSE compare that would pin a pose as much as a result.
    TEST_F(DDGICascadeEvidenceTest, MultiAngleCascadeEvidence)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct View
        {
            const char* Name;
            glm::vec3 Eye;
            glm::vec3 Target;
        };
        const std::array<View, 4> views = { {
            { "ddgi_cascade_near", { 0.0f, 3.0f, 6.0f }, { 0.0f, 3.0f, -40.0f } },
            { "ddgi_cascade_mid", { 0.0f, 3.0f, -12.0f }, { 0.0f, 3.0f, -50.0f } },
            { "ddgi_cascade_far", { 0.0f, 3.0f, -34.0f }, { 0.0f, 3.0f, -58.0f } },
            { "ddgi_cascade_back", { 0.0f, 3.0f, -34.0f }, { 0.0f, 3.0f, 6.0f } },
        } };

        for (const View& view : views)
        {
            // Re-converge per pose: the cascade windows are centred on the
            // camera, so moving the camera invalidates a slab in every cascade
            // and the field genuinely needs frames to settle. Capturing without
            // this would photograph the transient and read as a cascade bug.
            Converge(view.Eye, view.Target, kConvergeFrames);

            std::vector<u8> pixels;
            CaptureView(view.Name, view.Eye, view.Target, pixels);
            if (::testing::Test::HasFatalFailure())
                return;
            WriteEvidencePng(view.Name, pixels);

            auto* pass = Renderer3D::GetDDGIPass();
            ASSERT_NE(pass, nullptr);
            const DDGIProbeUpdatePass::ProbeStats stats = pass->GetProbeStats();
            std::cout << "[ddgi-cascades] " << view.Name << ": live=" << stats.LiveProbes
                      << " active=" << stats.ActiveProbes << " of " << kCascadeTotalProbes << "\n";
        }
    }

} // namespace OloEngine::Tests
