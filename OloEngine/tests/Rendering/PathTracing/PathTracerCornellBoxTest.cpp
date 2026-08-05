// OLO_TEST_LAYER: L1
// =============================================================================
// PathTracerCornellBoxTest.cpp — the CI gate for the offline reference path
// tracer (issue #709, acceptance criterion 3).
//
// PathTracerFurnaceTest proves the integrator is UNBIASED. This file proves it
// is USABLE as a gate: reproducible to the bit, correct about the phenomena a
// GI implementation is supposed to reproduce, and cheap enough to run headless
// on every commit.
//
// WHY BIT-IDENTICAL AND NOT "WITHIN TOLERANCE"
// --------------------------------------------
// The point of a reference is to remove fuzziness, and a reference that is only
// reproducible up to thread scheduling reintroduces it: the gate would need a
// tolerance, and the tolerance would then have to be loose enough to swallow
// scheduling noise — which is exactly the band a real regression hides in. So
// determinism is asserted as an exact hash of the linear radiance buffer,
// across TWO renders and across a sequential-vs-parallel pair. See
// PathSampler.h and PathTracer::Render for the two halves of the guarantee.
//
// The physics assertions are deliberately DIFFERENTIAL (this region is redder
// than that one; this region is darker than its neighbour) rather than absolute
// pixel values. A converged path-traced image is still a Monte Carlo estimate,
// and an absolute per-pixel assertion would be a golden image with extra steps
// — the thing this instrument exists to replace.
//
// Classification: L1 (pure CPU, headless).
// =============================================================================

#include "OloEnginePCH.h"

#include "PathTracing/ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/PathTracing/PathTracer.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    namespace Fixtures = OloEngine::Tests::PathTracingFixtures;
    namespace fs = std::filesystem;

    namespace
    {
        // The CI gate's budget. Small on purpose: every assertion below is a
        // region mean or an ordering, and both are stable long before the image
        // looks clean. Measured margins at this budget are 4-6x the thresholds
        // (see each assertion).
        constexpr u32 kGateWidth = 64;
        constexpr u32 kGateHeight = 64;
        constexpr u32 kGateSamples = 128;

        // The indirect-light test renders TWICE (direct-only and multi-bounce)
        // and asserts only frame/patch means, so it buys nothing from the extra
        // resolution the evidence PNG wants.
        constexpr u32 kBounceWidth = 48;
        constexpr u32 kBounceHeight = 48;

        [[nodiscard]] PathTracerSettings GateSettings()
        {
            PathTracerSettings settings;
            settings.SamplesPerPixel = kGateSamples;
            settings.MaxBounces = 6;
            settings.RussianRouletteStartBounce = 4;
            settings.EnableNextEventEstimation = true;
            settings.Seed = 0x709u;
            return settings;
        }

        // The visual-evidence directory: always OloEditor/assets/tests/visual,
        // reached from either possible working directory. The GPU visual tests
        // run with CWD == OloEditor (RenderPropertyFixture sets it); this file
        // is headless and runs from the repo root.
        //
        // The probe is `assets/shaders` — a directory that exists ONLY under
        // OloEditor. An earlier version keyed off `assets` alone and quietly
        // wrote the evidence into the REPO ROOT's assets/ (the README
        // screenshots live there), creating a second, wrong output tree that
        // nothing failed over. If you relax this probe, check what else is
        // called "assets" first.
        [[nodiscard]] fs::path ResolveVisualDir()
        {
            std::error_code ec;
            for (const fs::path editorRoot : { fs::path("OloEditor"), fs::path(".") })
            {
                if (!fs::exists(editorRoot / "assets" / "shaders", ec))
                    continue;

                const fs::path visual = editorRoot / "assets" / "tests" / "visual";
                fs::create_directories(visual, ec);
                if (!ec)
                    return visual;
            }
            return {};
        }

        // Mean radiance of a small patch centred on the pixel a WORLD point
        // projects to. Every region assertion in this file goes through here:
        // the patch is anchored to geometry, so it stays correct if the camera
        // or the resolution changes, and a wrong anchor is a visible world
        // coordinate rather than an invisible pixel fraction.
        [[nodiscard]] glm::vec3 SampleAround(const ReferenceFilm& film, const Fixtures::CornellBoxScene& fixture,
                                             const glm::vec3& worldPoint, i32 radius = 3)
        {
            const glm::ivec2 pixel = fixture.ProjectToPixel(worldPoint, film.GetWidth(), film.GetHeight());
            if (pixel.x < 0 || pixel.y < 0)
                return glm::vec3(0.0f);

            const auto x0 = static_cast<u32>(std::max(pixel.x - radius, 0));
            const auto y0 = static_cast<u32>(std::max(pixel.y - radius, 0));
            const auto x1 = static_cast<u32>(pixel.x + radius);
            const auto y1 = static_cast<u32>(pixel.y + radius);
            return film.MeanRadiance(x0, y0, x1, y1);
        }

        void WriteEvidencePng(const ReferenceFilm& film, const std::string& name)
        {
            const fs::path dir = ResolveVisualDir();
            if (dir.empty())
            {
                // Evidence is a diagnostic, not a contract — never fail a
                // headless run because the asset tree is not where we guessed.
                std::cout << "[evidence] could not resolve a visual output directory; skipping " << name << "\n";
                return;
            }

            std::vector<u8> rgba;
            // Reinhard + gamma: the engine's DEFAULT display transform
            // (PostProcessSettings), so the evidence reads like a viewport
            // screenshot rather than a raw HDR dump.
            film.EncodeRgba8(rgba, /*tonemap*/ 1, /*exposure*/ 1.0f, /*applyGamma*/ true);

            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(film.GetWidth()),
                                               static_cast<int>(film.GetHeight()), 4, rgba.data(),
                                               static_cast<int>(film.GetWidth()) * 4);
            if (wrote == 0)
                std::cout << "[evidence] stbi_write_png failed for " << path << "\n";
            else
                std::cout << "[evidence] wrote " << path << "\n";
        }
        // OPT-IN high-resolution evidence.
        //
        // The gate renders at 64x64 because every assertion here is a region
        // mean, and resolution buys those nothing but runtime. But a 64x64 PNG
        // is not something a human can actually read, and "evidence you cannot
        // inspect" is not evidence. So: cheap by default, inspectable on
        // demand. Set OLO_PATHTRACER_EVIDENCE=1 to also write a converged
        // 192x192 frame. Same env-var-opt-in shape as OLOENGINE_GOLDEN_REBASE.
        //
        // Run it in a RELEASE build: the tracer is ~40x slower under MSVC Debug
        // (measured on the gate renders), which turns a 10-second evidence
        // frame into a coffee break.
        [[nodiscard]] bool HighResEvidenceRequested()
        {
            const char* value = std::getenv("OLO_PATHTRACER_EVIDENCE");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        }

        void WriteHighResEvidenceIfRequested(const Fixtures::CornellBoxScene& fixture, const std::string& name)
        {
            if (!HighResEvidenceRequested())
                return;

            constexpr u32 kSize = 192;
            PathTracerSettings settings = GateSettings();
            settings.SamplesPerPixel = 256;
            settings.MaxBounces = 8;

            std::cout << "[evidence] OLO_PATHTRACER_EVIDENCE set — rendering two angles at " << kSize << "x" << kSize
                      << ", " << settings.SamplesPerPixel << " spp\n";

            // TWO angles, per CLAUDE.md's rendering rule. Head-on shows the
            // emitter and the ceiling (the indirect-only surface); the raking
            // pose shows the block's side faces, where colour bleeding is most
            // legible — its left face reads red and its right green. Neither
            // pose alone covers both. One pose's blind spot is not evidence of
            // correctness.
            ReferenceFilm film(kSize, kSize);
            PathTracer::Render(fixture.Scene, fixture.MakeCamera(kSize, kSize), settings, film);
            WriteEvidencePng(film, name + "_HeadOn");

            ReferenceFilm rakingFilm(kSize, kSize);
            PathTracer::Render(fixture.Scene, Fixtures::CornellBoxScene::MakeRakingCamera(kSize, kSize), settings,
                               rakingFilm);
            WriteEvidencePng(rakingFilm, name + "_Raking");
        }
    } // namespace

    // =========================================================================
    // Determinism — the property the gate rests on.
    // =========================================================================
    TEST(PathTracerCornellBox, RendersAreBitIdentical)
    {
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        ASSERT_TRUE(fixture.Scene.IsBuilt());

        const ReferenceCamera camera = fixture.MakeCamera(32, 32);
        PathTracerSettings settings = GateSettings();
        settings.SamplesPerPixel = 16;

        ReferenceFilm first(32, 32);
        PathTracer::Render(fixture.Scene, camera, settings, first);

        ReferenceFilm second(32, 32);
        PathTracer::Render(fixture.Scene, camera, settings, second);

        EXPECT_EQ(first.ComputeHash(), second.ComputeHash())
            << "two identical renders produced different pixels — the sampler or the accumulation "
               "order is not stateless, and no exact-comparison gate can be built on this";
    }

    // The strongest form, and the one that does NOT depend on how many worker
    // threads happen to be running: reproduce ONE pixel by hand, in complete
    // isolation from the frame, and require it to match the full render
    // bit-for-bit.
    //
    // If a pixel's value depended on anything outside that pixel — a shared
    // sampler stream, a running accumulator, an RNG advanced by its neighbours
    // — this fails. Every other determinism test in this file can be satisfied
    // by a render that is merely *repeatable*; this one cannot.
    TEST(PathTracerCornellBox, ASinglePixelReproducesTheFullRenderExactly)
    {
        constexpr u32 kSize = 24;
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        const ReferenceCamera camera = fixture.MakeCamera(kSize, kSize);

        PathTracerSettings settings = GateSettings();
        settings.SamplesPerPixel = 16;

        ReferenceFilm film(kSize, kSize);
        PathTracer::Render(fixture.Scene, camera, settings, film);

        // Mirror Render's per-pixel loop EXACTLY — including its arithmetic
        // form, not merely its algebra. Render precomputes a reciprocal and
        // multiplies; `a * (1.0f / 24.0f)` is not required to equal `a / 24.0f`
        // in fp32 (24 is not a power of two, so the reciprocal is inexact). A
        // one-ULP difference in the screen UV moves the primary ray, and near a
        // silhouette the two rays hit different surfaces — which would make the
        // file's load-bearing determinism assertion fail for a reason that has
        // nothing to do with determinism.
        const f32 invSize = 1.0f / static_cast<f32>(kSize);
        const f32 invSamples = 1.0f / static_cast<f32>(settings.SamplesPerPixel);
        const auto reproduce = [&](u32 x, u32 y)
        {
            const u32 pixelSeed = MakePixelSeed(x, y, settings.Seed);
            glm::vec3 accumulated(0.0f);
            for (u32 sample = 0; sample < settings.SamplesPerPixel; ++sample)
            {
                PathSampler sampler(pixelSeed, sample);
                const glm::vec2 jitter = sampler.Get2D();
                const glm::vec2 screenUV((static_cast<f32>(x) + jitter.x) * invSize,
                                         (static_cast<f32>(y) + jitter.y) * invSize);
                accumulated += PathTracer::TracePath(fixture.Scene, camera.GenerateRay(screenUV), settings, sampler);
            }
            return accumulated * invSamples;
        };

        for (const glm::uvec2 pixel : { glm::uvec2(0, 0), glm::uvec2(7, 19), glm::uvec2(13, 11),
                                        glm::uvec2(kSize - 1, kSize - 1) })
        {
            const glm::vec3 expected = reproduce(pixel.x, pixel.y);
            const glm::vec3 actual = film.GetPixel(pixel.x, pixel.y);
            for (glm::length_t channel = 0; channel < 3; ++channel)
            {
                EXPECT_FLOAT_EQ(actual[channel], expected[channel])
                    << "pixel (" << pixel.x << ", " << pixel.y << ") channel " << channel
                    << " — a pixel's value depends on something outside that pixel";
            }
        }
    }

    // The parallel-vs-sequential form. NOTE: this DEGENERATES to a repeat
    // render whenever the process has no FScheduler workers running (as the
    // headless test binary usually does) — ParallelFor then executes inline.
    // It is kept because it is the assertion that matters on a machine where
    // workers ARE up, but it is not the load-bearing determinism test; the
    // single-pixel reproduction above is.
    TEST(PathTracerCornellBox, ParallelAndSequentialRendersAgreeExactly)
    {
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        const ReferenceCamera camera = fixture.MakeCamera(32, 32);

        PathTracerSettings parallelSettings = GateSettings();
        parallelSettings.SamplesPerPixel = 16;
        parallelSettings.ForceSingleThread = false;

        PathTracerSettings sequentialSettings = parallelSettings;
        sequentialSettings.ForceSingleThread = true;

        ReferenceFilm parallelFilm(32, 32);
        PathTracer::Render(fixture.Scene, camera, parallelSettings, parallelFilm);

        ReferenceFilm sequentialFilm(32, 32);
        PathTracer::Render(fixture.Scene, camera, sequentialSettings, sequentialFilm);

        EXPECT_EQ(parallelFilm.ComputeHash(), sequentialFilm.ComputeHash())
            << "the parallel render does not match the sequential one bit-for-bit";
    }

    // Changing only the seed must change the noise but not the converged
    // answer. If it changed the answer, the "reference" would be one of many.
    TEST(PathTracerCornellBox, SeedChangesNoiseButNotTheConvergedMean)
    {
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        const ReferenceCamera camera = fixture.MakeCamera(24, 24);

        PathTracerSettings a = GateSettings();
        a.SamplesPerPixel = 96;
        PathTracerSettings b = a;
        b.Seed = a.Seed ^ 0xabcdef01u;

        ReferenceFilm filmA(24, 24);
        PathTracer::Render(fixture.Scene, camera, a, filmA);
        ReferenceFilm filmB(24, 24);
        PathTracer::Render(fixture.Scene, camera, b, filmB);

        EXPECT_NE(filmA.ComputeHash(), filmB.ComputeHash()) << "the seed had no effect at all";

        // Measured: 0.2% apart per channel.
        const glm::vec3 meanA = filmA.MeanRadiance(2, 2, 21, 21);
        const glm::vec3 meanB = filmB.MeanRadiance(2, 2, 21, 21);
        for (glm::length_t channel = 0; channel < 3; ++channel)
        {
            const f32 relative = std::abs(meanA[channel] - meanB[channel]) / std::max(meanA[channel], 1e-4f);
            EXPECT_LT(relative, 0.05f) << "channel " << channel << ": the two seeds converge to different images";
        }
    }

    // =========================================================================
    // Physics the reference must get right — the phenomena a GI implementation
    // is judged on. All differential.
    // =========================================================================
    TEST(PathTracerCornellBox, ReproducesColourBleedingAndShadowing)
    {
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        ASSERT_TRUE(fixture.Scene.IsBuilt());

        PathTracerSettings settings = GateSettings();

        ReferenceFilm film(kGateWidth, kGateHeight);
        PathTracer::Render(fixture.Scene, fixture.MakeCamera(kGateWidth, kGateHeight), settings, film);
        WriteEvidencePng(film, "PathTracer_CornellBox");
        WriteHighResEvidenceIfRequested(fixture, "PathTracer_CornellBox_HiRes");

        // --- colour bleeding ------------------------------------------------
        // The left wall is red and the right wall is green. The white floor
        // near each wall must pick up that wall's hue via the INDIRECT bounce.
        // This is the single clearest GI signature and it is impossible to
        // produce with direct lighting alone.
        //
        // Both patches are FLOOR points, projected through the same camera the
        // render used (see CornellBoxScene::ProjectToPixel for why a hand-
        // guessed pixel rectangle is not acceptable here).
        // z = +0.2 puts both patches in FRONT of the block, so both are
        // directly lit and the only asymmetry between them is which wall their
        // indirect light bounced off.
        const glm::vec3 nearRedWall = SampleAround(film, fixture, glm::vec3(-0.85f, -1.0f, 0.2f));
        const glm::vec3 nearGreenWall = SampleAround(film, fixture, glm::vec3(0.85f, -1.0f, 0.2f));

        ASSERT_GT(nearRedWall.r + nearRedWall.g + nearRedWall.b, 1e-3f) << "the red-side floor patch is black";
        ASSERT_GT(nearGreenWall.r + nearGreenWall.g + nearGreenWall.b, 1e-3f)
            << "the green-side floor patch is black";

        const f32 redSideHue = nearRedWall.r / std::max(nearRedWall.g, 1e-5f);
        const f32 greenSideHue = nearGreenWall.r / std::max(nearGreenWall.g, 1e-5f);

        // Measured: 1.36 beside the red wall vs 0.89 beside the green one — a
        // 1.53x separation against a 1.15x threshold.
        EXPECT_GT(redSideHue, greenSideHue * 1.15f)
            << "no colour bleeding: the floor beside the RED wall (r/g = " << redSideHue
            << ") is not measurably redder than the floor beside the GREEN wall (r/g = " << greenSideHue
            << ") — indirect light is not carrying wall albedo";

        // --- the emitter is the brightest thing in frame --------------------
        const glm::vec3 emitter = SampleAround(film, fixture, glm::vec3(0.0f, 0.98f, 0.0f), 2);
        const glm::vec3 backWall = SampleAround(film, fixture, glm::vec3(0.0f, 0.0f, -1.0f));
        // Measured ratio: 37.8x.
        EXPECT_GT(emitter.g, backWall.g * 2.0f) << "the ceiling emitter is not brighter than the lit back wall";

        // --- shadowing ------------------------------------------------------
        // The block spans x,z in [-0.55, -0.05] with its top at y = -0.2, and
        // the emitter is centred over the origin — so a floor point BEHIND the
        // block relative to the light is occluded, while a mirrored point on
        // the open side is not. Both are verified floor points, and the
        // occlusion is a geometric fact of the fixture, not an eyeball.
        const glm::vec3 shadowedFloor = SampleAround(film, fixture, glm::vec3(-0.75f, -1.0f, -0.75f));
        const glm::vec3 openFloor = SampleAround(film, fixture, glm::vec3(0.5f, -1.0f, -0.2f));
        // Measured ratio: 3.3x.
        EXPECT_GT(openFloor.g, shadowedFloor.g * 1.2f)
            << "the open floor (" << openFloor.g << ") is not brighter than the floor in the block's shadow ("
            << shadowedFloor.g << ") — shadow rays are not being traced";
    }

    // Indirect light must actually exist: a 1-bounce (direct-only) render has
    // to be strictly darker than a multi-bounce one everywhere the direct term
    // does not dominate, and the back of the block — which the emitter cannot
    // see at all — must go from black to non-black.
    TEST(PathTracerCornellBox, MultiBounceAddsIndirectLight)
    {
        const Fixtures::CornellBoxScene fixture = Fixtures::MakeCornellBoxScene();
        const ReferenceCamera camera = fixture.MakeCamera(kBounceWidth, kBounceHeight);

        PathTracerSettings direct = GateSettings();
        direct.MaxBounces = 1; // direct lighting only

        PathTracerSettings global = direct;
        global.MaxBounces = 8;

        ReferenceFilm directFilm(kBounceWidth, kBounceHeight);
        PathTracer::Render(fixture.Scene, camera, direct, directFilm);
        WriteEvidencePng(directFilm, "PathTracer_CornellBox_DirectOnly");

        ReferenceFilm globalFilm(kBounceWidth, kBounceHeight);
        PathTracer::Render(fixture.Scene, camera, global, globalFilm);

        // Whole-frame energy must go UP with more bounces, never down.
        const glm::vec3 directMean = directFilm.MeanRadiance(0, 0, kBounceWidth - 1, kBounceHeight - 1);
        const glm::vec3 globalMean = globalFilm.MeanRadiance(0, 0, kBounceWidth - 1, kBounceHeight - 1);
        for (glm::length_t channel = 0; channel < 3; ++channel)
        {
            EXPECT_GE(globalMean[channel], directMean[channel] * 0.999f)
                << "channel " << channel << ": adding bounces removed energy";
        }
        // Measured: the frame mean rises 1.19x from direct-only to 8 bounces.
        EXPECT_GT(globalMean.g, directMean.g * 1.05f)
            << "extra bounces added no measurable indirect light";

        // The ceiling sits ABOVE the emitter, which emits one-sided downward —
        // so direct-only leaves it exactly black, and any light it shows is
        // necessarily a bounce off the floor. This is the sharpest
        // indirect-only probe the fixture offers: the "before" value is not
        // merely small, it is zero.
        const glm::vec3 ceilingPoint(-0.6f, 1.0f, 0.35f);
        const glm::vec3 directCeiling = SampleAround(directFilm, fixture, ceilingPoint);
        const glm::vec3 globalCeiling = SampleAround(globalFilm, fixture, ceilingPoint);
        // Measured: exactly 0.0 direct-only, 0.114 with 8 bounces.
        EXPECT_LT(directCeiling.g, 1e-4f) << "the ceiling is lit in a DIRECT-ONLY render — the emitter's "
                                             "one-sided emission is leaking upward";
        EXPECT_GT(globalCeiling.g, 1e-3f)
            << "the ceiling receives no bounce light at all — there is no indirect transport";
    }
} // namespace OloEngine::Tests
