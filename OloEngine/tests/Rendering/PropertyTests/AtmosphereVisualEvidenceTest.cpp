// OLO_TEST_LAYER: L8
// =============================================================================
// AtmosphereVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the atmosphere & sky epic (issue #633): the
// time-of-day driven sun/moon + AtmosphereSky bake, the weather director's
// blended fog/precipitation/sun-dimming, and the volumetric cloudscape —
// rendered through the FULL Renderer3D pipeline across the acceptance
// matrix
//     {dawn, noon, dusk, night} x {Clear, Overcast, Storm}
// from a fixed ground-level pose, written to
//     OloEditor/assets/tests/visual/Atmosphere_<Time><Weather>.png
//
// Scene follows docs/agent-rules/single-mesh-visual-test-lighting.md: a
// static ground plane + props so the frame never reads as "subject on
// black", and the PNGs are meant to be OPENED AND LOOKED AT (CLAUDE.md:
// rendering changes MUST be visually verified), not just pass the numbers.
//
// Driver-independent contracts asserted on the captured bands (goldens are
// the regression net; these pin the physics):
//   1. The noon clear sky is bright and blue (B >= R in the sky band).
//   2. The clear night sky is far darker than the clear noon sky.
//   3. Noon storm ground is darker than noon clear ground (sun dimming +
//      cloud shadows attenuate the directional light).
//   4. The dawn horizon is warmer (higher R/B) than the noon horizon.
//
// Golden model mirrors WaterVisualEvidenceTest: deterministic frame (mock
// time pinned; the time-of-day clock is paused — each capture sets the hour
// explicitly), normal runs COMPARE (RMSE) against the committed PNGs; set
// --olo-golden-rebase to (re)write them after a deliberate change.
// Runs in the normal suite; SKIPs cleanly without a GL 4.6 context.
//
// NIGHT-CELL COUPLING (issue #754). The three Night* cells are uniquely
// sensitive to two procedural night-sky paths, and a rebake of THIS golden
// must accompany any change to either — in the SAME PR:
//   • the star-field hash in AtmosphereSky.glsl / AtmosphereSky.cpp — moves
//     every star (sky band; strongest in NightClear, occluded in NightStorm);
//   • the InfiniteGrid.glsl coplanar depth bias — the grid is the dominant
//     bright feature on the near-black night ground (ground band; present in
//     all three night cells, invisible by day where the lit ground swamps it).
// #754 is exactly what happens when that coupling is ignored: f4fef24b (star
// hash) and dfd100ef (grid bias) both landed without rebaking these goldens,
// so NightClear/NightOvercast sat red for 10 days and the failure normalised
// as "the expected 1 failure" — masking any genuine new regression. An
// ADDITIONAL CPU contract test (one that only re-detects the star relocation)
// was considered (per the issue) and deliberately NOT added — the existing
// AtmosphereSkyMathTest CPU mirror and the band contracts below stay:
// "these goldens must be rebaked" ≡ "the night frame changed visibly", which
// is precisely what this golden already measures; the star POSITIONS are
// bit-exact but their brightness carries cross-vendor/compiler ULP variance
// (AtmosphereSky.h), so a tight star-value pin would itself become a flaky red
// — reintroducing the very problem #754 fixes. The durable guard is the
// same-PR rebake discipline, pinned by the GOLDEN COUPLING notes at both
// change sites, not another normalisable red.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Atmosphere/WeatherSystem.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall clock: pins the cloud wind advection + any Time-driven
        // animation so every capture is deterministic (the golden
        // prerequisite). The time-of-day HOUR is a component field set per
        // capture — the clock itself is paused.
        constexpr f32 kCaptureTime = 100.0f;

        // Per-channel RMSE threshold (0..255). Slightly looser than the water
        // test's 6.0: the cloud raymarch keeps a jittered start whose temporal
        // accumulation depends on the global frame counter, contributing a
        // few grey levels of run-order variance in cloudy captures.
        constexpr f64 kGoldenRmseThreshold = 8.0;

        struct BandStats
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean absolute difference between horizontally ADJACENT luma samples in
        // a band. Zero on any horizontally uniform image, including one with a
        // strong vertical gradient -- which is the point. A plain standard
        // deviation is inflated by the sky's own top-to-bottom gradient, so it
        // cannot tell "a lit sky with cloud shapes in it" from "a flat fill
        // with a gradient", and the flat fill is the failure being guarded
        // against (issue #903 follow-up). Clouds are horizontal structure.
        [[nodiscard]] f64 HorizontalDetail(const std::vector<u8>& pixels, u32 rowBegin, u32 rowEnd)
        {
            const auto luma = [](const u8* px)
            { return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]; };

            f64 total = 0.0;
            u64 count = 0;
            for (u32 y = rowBegin; y < rowEnd; ++y)
            {
                const u8* row = pixels.data() + static_cast<std::size_t>(y) * kWidth * 4u;
                for (u32 x = 1; x < kWidth; ++x)
                {
                    total += std::abs(luma(row + x * 4) - luma(row + (x - 1) * 4));
                    ++count;
                }
            }
            return count > 0 ? total / static_cast<f64>(count) : 0.0;
        }

        // Mean RGB over a horizontal band [rowBegin, rowEnd) of a TOP-DOWN
        // RGBA8 buffer.
        [[nodiscard]] BandStats MeanBand(const std::vector<u8>& pixels, u32 rowBegin, u32 rowEnd)
        {
            BandStats s;
            u64 count = 0;
            for (u32 y = rowBegin; y < rowEnd; ++y)
            {
                const u8* row = pixels.data() + static_cast<std::size_t>(y) * kWidth * 4u;
                for (u32 x = 0; x < kWidth; ++x)
                {
                    s.R += row[x * 4 + 0];
                    s.G += row[x * 4 + 1];
                    s.B += row[x * 4 + 2];
                    ++count;
                }
            }
            if (count > 0)
            {
                s.R /= static_cast<f64>(count);
                s.G /= static_cast<f64>(count);
                s.B /= static_cast<f64>(count);
            }
            return s;
        }

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

        // Goldens are baselined per GPU vendor.
        //
        // These captures are compared with an RMSE threshold of 8, and a real
        // AMD-vs-NVIDIA difference blows straight through that: on radeonsi the
        // three NIGHT captures drift 19-24 RMSE against NVIDIA-baselined images
        // while every day capture passes, because the night sky is where the
        // star-field/procedural-sky float precision diverges most. That is a
        // genuine vendor difference, not a regression, so each vendor needs its
        // own baseline set.
        //
        // Mirrors GoldenImageTests::GoldenBaselineDir so both golden mechanisms
        // scope identically; previously only that one honoured the variable, so
        // an AMD run silently compared against — and on a rebase would have
        // OVERWRITTEN — the NVIDIA baselines.
        [[nodiscard]] fs::path GoldenBaselineDir()
        {
            fs::path base = fs::path("assets") / "tests" / "visual";
            if (const std::string& vendor = OloEngine::Tests::Options().GoldenVendor; !vendor.empty())
            {
                // Belt-and-braces since #735: the value is now ALSO whitelisted
                // at parse time (TestOptions.cpp, RequirePlainPathComponent), so
                // an unsafe vendor never reaches any consumer. That gate is the
                // one that matters — this check lived only here while the
                // GoldenImageTests::GoldenBaselineDir it claims to mirror
                // concatenated the value unchecked, which is the drift the
                // parse-time whitelist exists to stop recurring.
                //
                // The vendor must name ONE directory below the baseline root,
                // nothing else. `base /= vendor` is not safe on its own: an
                // absolute value REPLACES base outright (fs::path semantics),
                // and "../.." walks out of the tree — so under
                // --olo-golden-rebase a stray value would write goldens
                // anywhere the process can reach. Accept only a plain name.
                const std::string_view name(vendor);
                // A separator/dot check alone is not enough on Windows: a
                // DRIVE-RELATIVE value like "C:vendor" contains neither, yet
                // fs::path treats it as rooted, so `base /= name` would discard
                // base and resolve against C:'s current directory. Reject
                // anything fs::path considers rooted at all.
                const fs::path vendorPath(name);
                const bool safe = name.find('/') == std::string_view::npos &&
                                  name.find('\\') == std::string_view::npos &&
                                  name != "." && name != ".." &&
                                  !vendorPath.has_root_name() && !vendorPath.has_root_directory();
                if (!safe)
                {
                    ADD_FAILURE() << "--olo-golden-vendor must be a single directory name "
                                     "(no separators, '.', '..', drive letter or root) — got '"
                                  << name << "'";
                    return base;
                }
                base /= name;
            }
            return base;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class AtmosphereVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // The weather director writes the PROCESS-GLOBAL Renderer3D
        // fog/wind/precipitation/snow settings (that is its job), and the
        // RendererAttachedTest snapshot does NOT cover those structs — the
        // same trap VolumetricFogVisualEvidenceTest documents for FogSettings.
        // Without this restore, the matrix's final Storm state leaked into
        // every later visual test in the suite (storm fog over the water /
        // bloom / planar-reflection goldens — 8 cross-test failures).
        void SetUp() override
        {
            m_SavedFog = Renderer3D::GetFogSettings();
            m_SavedWind = Renderer3D::GetWindSettings();
            m_SavedPrecipitation = Renderer3D::GetPrecipitationSettings();
            m_SavedSnowAccumulation = Renderer3D::GetSnowAccumulationSettings();
            // The #1008 probe below reads CloudsColor AFTER the frame. CloudsColor is
            // a transient that dies at the fog pass, so with aliasing on the planner
            // is free to hand its GL texture to a later pass, and the "pre-fog" read
            // then returns that pass's output instead. Measured: with aliasing on,
            // every fogged cell read back the fog composite under the CloudsColor
            // name; with it off, the same read returned the cloudscape output. The
            // texture-id check against the FINAL framebuffer cannot catch this
            // (that one is a different texture either way). Process-wide state:
            // restored in TearDown.
            m_SavedDisableAliasing = Levers::DisableTransientAliasing();
            Levers::SetDisableTransientAliasing(true);
            RendererAttachedTest::SetUp();
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            Renderer3D::GetFogSettings() = m_SavedFog;
            Renderer3D::GetWindSettings() = m_SavedWind;
            Renderer3D::GetPrecipitationSettings() = m_SavedPrecipitation;
            Renderer3D::GetSnowAccumulationSettings() = m_SavedSnowAccumulation;
            Renderer3D::SetCloudscapeState(CloudscapeRenderState{});
            Levers::SetDisableTransientAliasing(m_SavedDisableAliasing);
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Derive the live renderer flags (depth prepass, Forward+ mode,
            // culling toggles) from the settings, exactly as EditorLayer does
            // at boot/scene-load since issue #534. The pristine test process
            // runs with those flags UN-derived, but any earlier test that
            // calls ApplyRendererSettings (e.g. RendererSettingsBootstrapTest's
            // TearDown) flips them process-wide — and the derived state shifts
            // the textured ground enough to break the night goldens (RMSE ~22
            // on the grid lines; the applied state is also the one that
            // renders them stably instead of z-precision-dashed). Applying
            // here makes the captures order-independent and pins the goldens
            // to the editor-equivalent state.
            Renderer3D::ApplyRendererSettings();

            // The sun/moon light — DRIVEN by the time-of-day system each
            // frame; authored values here are placeholders.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_CastShadows = true;
            }

            // The atmosphere director: procedural sky (AtmosphereSky bake
            // inputs), the clock, the weather state machine and the cloud
            // layer, all on one entity for clarity.
            {
                Entity atmosphere = scene.CreateEntity("Atmosphere");
                auto& sky = atmosphere.AddComponent<ProceduralSkyComponent>();
                sky.m_CubemapResolution = 128; // 12 rebakes in this test — keep each cheap

                auto& tod = atmosphere.AddComponent<TimeOfDayComponent>();
                tod.m_Paused = true; // captures set the hour explicitly
                tod.m_DayOfYear = 80;
                tod.m_LatitudeDegrees = 48.0f;
                tod.m_MoonPhase = 0.5f; // full moon for the night captures
                tod.m_RebakeQuantumGameMinutes = 0.25f;

                auto& weather = atmosphere.AddComponent<WeatherStateComponent>();
                weather.m_TransitionDuration = 0.0f; // captures snap states

                // Fog must not be allowed to erase the sky it is supposed to sit
                // in front of. The engine's Overcast/Storm presets pair a cloud
                // deck with a genuinely thick fog (density 0.003 / 0.009), and at
                // this 4000 m far plane that saturates the distance term for
                // every sky pixel -- so the deck those same presets author at
                // 0.75-0.97 coverage was completely invisible in eight of the
                // twelve matrix cells. Capping the opacity keeps the authored
                // densities (the matrix still depicts the engine's real weather
                // states, and fog still thickens the horizon and dims the ground)
                // while guaranteeing the scene behind it survives.
                //
                // Note this is the SECOND scene to work around these defaults:
                // DriftScenePresets.h lowers the same two densities outright,
                // with a comment about Storm's 0.0034 "not the engine preset's
                // 0.009". Two independent scenes overriding the same default is
                // worth a look at the default itself -- out of scope here.
                weather.m_PresetOvercast.FogMaxOpacity = 0.7f;
                weather.m_PresetStorm.FogMaxOpacity = 0.7f;

                auto& clouds = atmosphere.AddComponent<CloudscapeComponent>();
                // Scales tuned live in the AtmosphereTest editor scene: a low,
                // thin layer with ~2 km weather features keeps visible cloud
                // STRUCTURE (shapes + blue gaps) inside the capture frustum.
                clouds.m_LayerBottom = 700.0f;
                clouds.m_LayerTop = 1600.0f;
                clouds.m_WeatherMapScaleKm = 8.0f;
                clouds.m_MaxSteps = 48;
                clouds.m_TemporalBlend = 0.5f; // few warm-up frames per capture

                m_Atmosphere = atmosphere;
            }

            // Ground + props (single-mesh-visual-test-lighting rule: never a
            // lone subject on nothing).
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

            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                         { 120.0f, 1.0f, 120.0f }, { 0.36f, 0.4f, 0.3f });
            addPrimitive("Pillar", MeshPrimitive::Cube, { -6.0f, 6.0f, -14.0f },
                         { 2.0f, 12.0f, 2.0f }, { 0.75f, 0.72f, 0.68f });
            addPrimitive("Cube", MeshPrimitive::Cube, { 6.0f, 1.5f, -10.0f },
                         { 3.0f, 3.0f, 3.0f }, { 0.7f, 0.35f, 0.25f });
        }

        // Set the matrix cell, render, read back (top-down), save/compare the
        // PNG, and record the band stats for the cross-capture contracts. The
        // default pose is the ground-level horizon view every matrix cell
        // uses; the aerial capture below passes its own (positive pitch tilts
        // the view down, per EditorCamera).
        void Capture(const std::string& name, f32 hours, WeatherStateId weatherState,
                     const glm::vec3& cameraEye = { 0.0f, 3.0f, 14.0f }, f32 cameraPitch = -0.08f)
        {
            auto& tod = m_Atmosphere.GetComponent<TimeOfDayComponent>();
            tod.m_TimeOfDayHours = hours;

            auto& weather = m_Atmosphere.GetComponent<WeatherStateComponent>();
            weather.m_CurrentState = weatherState;
            weather.m_TargetState = weatherState;
            weather.m_TransitionProgress = 1.0f;
            weather.m_PrevTargetSeen = weatherState;
            weather.m_BlendedValid = false; // re-settle on the new state
            WeatherSystem::ApplyImmediate(GetScene());

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Ground-level pose looking over the props toward the horizon —
            // sky (with clouds) fills the top half, lit ground the bottom.
            camera.SetPose(cameraEye, 0.0f, cameraPitch);

            // Several ticks: the sky rebake happens on the first, the cloud
            // temporal accumulation settles over the rest.
            RunEditorFrames(camera, 4);

            auto resolveComposite = []
            {
                auto resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
                if (!resolved)
                    resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
                if (!resolved)
                    resolved = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                return resolved;
            };
            auto fb = resolveComposite();
            if (!fb)
            {
                // A preceding GPU test can leave the pipeline mid-reconfigure
                // (path switch / topology rebuild); the graph then needs an
                // extra frame or two before the composite chain resolves.
                // Settle more and retry once before declaring failure.
                RunEditorFrames(camera, 6);
                fb = resolveComposite();
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << name << "'";

            std::vector<u8> pixels;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // Flip to top-down for both the PNG and the band sampling.
            {
                const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                    u8* bot = pixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            // Bands: sky = top 18%, horizon = 38-46%, ground = bottom 25%.
            m_Sky[name] = MeanBand(pixels, 0, kHeight * 18u / 100u);
            m_SkyDetail[name] = HorizontalDetail(pixels, 0, kHeight * 18u / 100u);
            m_Horizon[name] = MeanBand(pixels, kHeight * 38u / 100u, kHeight * 46u / 100u);
            m_Ground[name] = MeanBand(pixels, kHeight * 75u / 100u, kHeight);

            // ---- issue #1008: what is BEHIND the fog in the horizon band? ----
            //
            // The AMD baselines read the uncapped fog colour across that band while
            // NVIDIA shows sky and cloud through a 0.7-capped fog. With the cap
            // proven to arrive and hold on AMD hardware (a standalone EGL probe reads
            // u_FogRayleighColorAndMaxOpacity.a == 0.700000 and clamp() honouring it),
            // the composite `0.3*background + 0.7*fogColor` can only read as pure fog
            // if the BACKGROUND already equals the fog colour there.
            //
            // FogRenderPass reads the first valid of PrecipitationColor / CloudsColor /
            // TAAColor / ... as its input, so with a cloud deck present CloudsColor IS
            // the pre-fog image. Reporting its horizon band next to the composite's is
            // the measurement that separates the two candidates in #1008:
            //
            //   pre-fog band ~= fog colour   -> the sky/atmosphere path is the bug,
            //                                   the fog pass is innocent
            //   pre-fog band ~= NVIDIA's     -> the fog composite is the bug
            //
            // Diagnostic only: it prints, it never fails. The point is to get one
            // number off the AMD box, and an assertion here would just be a second
            // way for #1008 to turn a job red.
            // Mirror FogRenderPass's OWN input selection order, which is
            // PrecipitationColor first and CloudsColor second (see its
            // ReadFirstValidVersionedInputForPass list). The Storm preset enables
            // rain, so for the four Storm cells the buffer the fog pass actually
            // reads is PrecipitationColor -- the cloud composite plus the
            // precipitation overlay. Resolving only CloudsColor labelled those
            // cells' numbers "pre-fog" while reading a buffer one pass upstream of
            // the real fog input. Clear and Overcast author no precipitation, so
            // for them the two are the same buffer and nothing changes.
            //
            // Resolving PrecipitationColor is NOT enough to prefer it: the resource
            // exists in the graph from the first cell that enables rain onward, and
            // resolves happily for every later cell -- but PrecipitationPass returns
            // early when precipitation is off, so the buffer is then BLACK. Taking it
            // unconditionally reported horizon luma 0 for the nine cells that author
            // no precipitation. Gate on the live renderer setting that actually drives
            // the pass, which is the same thing FogRenderPass's "first VALID input"
            // resolves to at graph-build time.
            const bool precipitationRan = Renderer3D::GetPrecipitationSettings().Enabled;
            const char* preFogName = precipitationRan ? "PrecipitationColor" : "CloudsColor";
            auto preFog = precipitationRan
                              ? Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PrecipitationColor)
                              : Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::CloudsColor);
            if (!preFog)
            {
                preFogName = "CloudsColor";
                preFog = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::CloudsColor);
            }
            if (preFog)
            {
                // FALSIFY THE INSTRUMENT FIRST. Four Overcast cells reported a
                // pre-fog horizon band identical to the composited one to three
                // decimals, which is the signature of reading ONE buffer twice.
                // This check catches the direct case (CloudsColor resolving to the
                // final image). It does NOT catch the case that actually happened:
                // CloudsColor's transient reused by an intermediate pass after the
                // fog pass consumed it -- a different texture from the final
                // framebuffer, holding a post-fog image all the same. That is what
                // the SetUp lever closes; this check stays as the cheap guard.
                const u32 preTex = preFog->GetColorAttachmentRendererID(0);
                const u32 postTex = fb->GetColorAttachmentRendererID(0);
                std::cout << "[#1008-alias] " << name << "  " << preFogName << " tex=" << preTex
                          << "  composite tex=" << postTex
                          << (preTex == postTex ? "  *** SAME TEXTURE — probe is invalid ***" : "  (distinct)")
                          << std::endl;

                std::vector<u8> prePixels;
                ReadbackRgba8(preTex, kWidth, kHeight, prePixels);
                if (prePixels.size() == static_cast<std::size_t>(kWidth) * kHeight * 4u)
                {
                    const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
                    std::vector<u8> tmp(rowBytes);
                    for (u32 y = 0; y < kHeight / 2u; ++y)
                    {
                        u8* top = prePixels.data() + static_cast<std::size_t>(y) * rowBytes;
                        u8* bot = prePixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                        std::memcpy(tmp.data(), top, rowBytes);
                        std::memcpy(top, bot, rowBytes);
                        std::memcpy(bot, tmp.data(), rowBytes);
                    }
                    const auto preHorizon = MeanBand(prePixels, kHeight * 38u / 100u, kHeight * 46u / 100u);
                    const auto preSky = MeanBand(prePixels, 0, kHeight * 18u / 100u);
                    // Clamped to 8 bits by the readback, which is fine: this is a
                    // cross-VENDOR comparison of the same buffer, and the question is
                    // binary. Both sides get identical treatment.
                    // The band mean answers "how much light", never "a picture of
                    // what". CLAUDE.md's rule for renderer work is to look at the
                    // pixels, and this is the one buffer nobody has ever seen on AMD.
                    // gpu-conformance-amd uploads visual/**/*.png in its
                    // `if: failure()` artifact, and that job fails on #1008 anyway,
                    // so writing here is how the image gets off the box.
                    {
                        const fs::path preDir = fs::path("assets") / "tests" / "visual" / "fog1008";
                        std::error_code ec;
                        fs::create_directories(preDir, ec);
                        if (!ec)
                        {
                            const std::string prePath = (preDir / ("PreFog_" + name + ".png")).string();
                            ::stbi_write_png(prePath.c_str(), static_cast<int>(kWidth),
                                             static_cast<int>(kHeight), 4, prePixels.data(),
                                             static_cast<int>(kWidth) * 4);
                        }
                    }

                    std::cout << "[#1008] " << name
                              << "  pre-fog(" << preFogName << ") horizon luma=" << preHorizon.Luma()
                              << " rgb=" << preHorizon.R << "," << preHorizon.G << "," << preHorizon.B
                              << "  |  pre-fog sky luma=" << preSky.Luma()
                              << "  |  composited horizon luma=" << m_Horizon[name].Luma()
                              << std::endl;
                }
            }
            else
            {
                std::cout << "[#1008] " << name
                          << "  pre-fog unavailable -- neither PrecipitationColor nor CloudsColor this frame"
                          << std::endl;
            }

            const fs::path dir = GoldenBaselineDir();
            const std::string path = (dir / ("Atmosphere_" + name + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': "
                                 << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr) << "Missing golden '" << path
                                       << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh
                                     << " — rerun with --olo-golden-rebase.";
            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Capture '" << name << "' drifted from its golden (RMSE " << rmse << ")";
        }

        Entity m_Atmosphere;
        std::map<std::string, BandStats> m_Sky;
        std::map<std::string, f64> m_SkyDetail;
        std::map<std::string, BandStats> m_Horizon;
        std::map<std::string, BandStats> m_Ground;

        bool m_SavedDisableAliasing = false;
        FogSettings m_SavedFog;
        WindSettings m_SavedWind;
        PrecipitationSettings m_SavedPrecipitation;
        SnowAccumulationSettings m_SavedSnowAccumulation;
    };

    TEST_F(AtmosphereVisualEvidenceTest, DayNightWeatherMatrixRendersAndHoldsContracts)
    {
        // Pin the wall clock so wind advection / animated noise are identical
        // every run (mirrors WaterVisualEvidenceTest's mock-time scope).
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

        const std::pair<const char*, f32> times[] = {
            { "Dawn", 7.0f }, { "Noon", 12.0f }, { "Dusk", 17.5f }, { "Night", 0.0f }
        };
        const std::pair<const char*, WeatherStateId> weathers[] = {
            { "Clear", WeatherStateId::Clear },
            { "Overcast", WeatherStateId::Overcast },
            { "Storm", WeatherStateId::Storm },
        };

        for (const auto& [timeName, hours] : times)
        {
            for (const auto& [weatherName, state] : weathers)
            {
                Capture(std::string(timeName) + weatherName, hours, state);
                if (::testing::Test::HasFatalFailure())
                    return;
            }
        }

        // 13th capture — a substantially different angle, per the repo's
        // multi-angle visual-verification rule: an elevated vantage over the
        // props pitched ~22° down (the 120 m ground plane fills the lower
        // half; the top-of-frame ray still clears the horizon, keeping the
        // 700 m cloud deck in the top band). Exposes what the horizon pose
        // can't: the props/ground depth-compositing seen from above, the
        // scattered noon cumulus casting ground shadows, and the cloud
        // field's structure from a second vantage. First attempt used
        // { 0, 380, 60 } pitch 0.85 — at 60° FOV that puts the WHOLE frustum
        // below the horizon and shrinks the plane to a sliver; the frame was
        // a featureless haze. No named contract reads this cell; the golden
        // pins it.
        Capture("NoonClearAerial", 12.0f, WeatherStateId::Clear,
                { 0.0f, 45.0f, 55.0f }, 0.38f);
        if (::testing::Test::HasFatalFailure())
            return;

        // ── Cross-capture physical contracts ──
        // 1. Noon clear sky: bright and blue.
        const BandStats& noonClearSky = m_Sky["NoonClear"];
        EXPECT_GT(noonClearSky.Luma(), 60.0) << "noon clear sky must be bright";
        EXPECT_GE(noonClearSky.B, noonClearSky.R) << "noon clear sky must read blue";

        // 2. Clear night sky is far darker than clear noon.
        EXPECT_LT(m_Sky["NightClear"].Luma(), noonClearSky.Luma() * 0.4)
            << "night sky must be much darker than day";

        // 3. Storm dims the noon ground (sun dimming + cloud shadows).
        EXPECT_LT(m_Ground["NoonStorm"].Luma(), m_Ground["NoonClear"].Luma())
            << "storm must darken the ground at noon";

        // 4. Dawn horizon is warmer than noon horizon.
        const f64 dawnWarmth = m_Horizon["DawnClear"].R / std::max(m_Horizon["DawnClear"].B, 1.0);
        const f64 noonWarmth = m_Horizon["NoonClear"].R / std::max(m_Horizon["NoonClear"].B, 1.0);
        EXPECT_GT(dawnWarmth, noonWarmth) << "dawn horizon must be warmer than noon";

        // 5. THE OVERCAST AND STORM SKIES MUST ACTUALLY CONTAIN CLOUD.
        //
        // This file's header promises evidence for "the volumetric cloudscape"
        // across the whole {time} x {weather} matrix, and every cell had a
        // committed golden, so the matrix read as covered. It was not. Measured
        // on the pre-fix goldens, the sky band of all EIGHT Overcast/Storm cells
        // was a flat fill: standard deviation 0.000, range 0, and a mean of
        // *exactly* 165.00 (Overcast) / 150.00 (Storm) at dawn, noon, dusk AND
        // night alike. A lit cloud deck cannot be byte-identical at midnight and
        // midday; that constant was the weather preset's fog colour, which had
        // replaced the sky outright at these densities and far plane. Disabling
        // the cloudscape entirely moved those captures' sky band by 0.00% and
        // max channel 0 -- the goldens could not see the cloud system at all up
        // there, only its ground lighting (99% of the ground band).
        //
        // A golden cannot catch this on its own: it happily pins a flat fill
        // forever. So the contract is on horizontal DETAIL, which is what a
        // cloud deck has and a fog wall does not. Post-fix the eight cells
        // measure 0.21-0.35; the threshold sits an order of magnitude below the
        // signal and above the 0.000 the fogged frames produced.
        for (const char* cell : { "DawnOvercast", "NoonOvercast", "DuskOvercast", "NightOvercast",
                                  "DawnStorm", "NoonStorm", "DuskStorm", "NightStorm" })
        {
            EXPECT_GT(m_SkyDetail[cell], 0.02)
                << "'" << cell << "' sky band has no horizontal structure -- the cloud deck is being "
                                  "replaced by a flat fill (fog), so this capture is evidence of nothing";
        }

        // ...and the Clear cells, whose thinner 0.15-coverage deck is the one
        // regime where a low reading would be legitimate, still clear the bar.
        for (const char* cell : { "DawnClear", "NoonClear", "DuskClear", "NightClear" })
        {
            EXPECT_GT(m_SkyDetail[cell], 0.02)
                << "'" << cell << "' sky band is featureless";
        }
    }
} // namespace OloEngine::Tests
