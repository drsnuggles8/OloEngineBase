// =============================================================================
// VRCSPerfProbe.cpp — what does VRCS actually cost, and what does it buy?
//
// OLO_TEST_LAYER: L6
//
// Issue #683's acceptance criterion is "measured GPU-time reduction on the
// adopted passes in a lighting-heavy scene at visually equal output". The
// "visually equal" half is asserted in VRCSVisualEvidenceTest. This is the
// other half, and it is an INSTRUMENT, not a gate.
//
// It drives the real deferred pipeline over the real GTAO pass and reports the
// GPU-ms that GPUPassTimerPool already brackets around each dispatch:
//
//     GTAOPass/GTAO           the horizon-integration dispatch — what VRCS saves
//     GTAOPass/VRCSClassify   the classification dispatch      — what VRCS costs
//     GTAOPass/GTAO_Denoise   unchanged by VRCS; a control that should not move
//
// THE HEADLINE IS THE WHOLE BRACKET, not the GTAO dispatch, for two reasons the
// probe learned the hard way:
//
//   1. Classification is not free. It reads three full-resolution buffers to
//      write one texel per 64 pixels, and a report quoting only the shading
//      dispatch would look like a win while the frame got slower. The first
//      measured build was exactly that: GTAO 0.947 -> 1.354 ms, a 43%
//      REGRESSION, because masking lanes does not retire waves.
//   2. A GPU timestamp is written when the command stream REACHES it, not when
//      the preceding dispatch has drained, so work migrates between adjacent
//      brackets. One run measured gtao at 0.10 vs 1.37 ms while the denoise
//      bracket — which VRCS never touches — moved 0.07 -> 1.20 ms. The split is
//      for spotting where something changed; the sum is what can be quoted.
//
// AND THE TWO CONFIGURATIONS ARE INTERLEAVED, in alternating blocks, because a
// straight A-then-B run is not a controlled comparison on this machine: the
// identical full-rate path measured 0.947 ms in one run and 1.368 ms in the
// next, purely because a sibling worktree's build was loading the box. Drift
// that slow moves a whole block, so alternating splits it between the two
// configurations instead of charging all of it to whichever ran second.
//
// DISABLED_ ON PURPOSE, for the same three reasons GPUPrefixSumPerfProbe is:
//   * it has no baseline and asserts no timing, so it cannot fail for
//     environmental reasons (docs/testing.md's anti-flake rule);
//   * GPU timings on this box are contended by other worktrees and the desktop;
//   * L6 perf baselines are dev-workstation-only in this repo anyway.
//
// It exists so the PR's saving claim is reproducible rather than asserted, and
// so the next pass to adopt VRCS can measure its own dispatch instead of
// inheriting this one's number. Run it with:
//
//   OloEngine-Tests.exe --gtest_also_run_disabled_tests \
//                       --gtest_filter='*VRCSPerfProbe*'
//
// WHAT IT DOES NOT ISOLATE. The saving depends entirely on how much of the
// frame classifies as coarse, which is a property of the SCENE, not of the
// feature — a frame of foliage will coarsen almost nothing and a frame of flat
// walls almost everything. The scene here is a row of cubes on a large flat
// floor, which is deliberately favourable, so treat the ratio as an upper-ish
// bound for this pass rather than as a frame-rate forecast.
//
// The probe does NOT report the coarse-tile fraction. Reading it back would
// mean a GPU->CPU copy of the rate image in the middle of the measurement, and
// that stall is exactly the kind of thing that corrupts the timings this exists
// to produce. Look at VRCS_Rate_*.png from VRCSVisualEvidenceTest for that half
// of the picture.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 1600;
        constexpr u32 kHeight = 900;
        // Sampling shape. The two configurations are measured in ALTERNATING
        // blocks rather than one after the other, because a straight A-then-B
        // run is not a controlled comparison on this box: the first version of
        // this probe measured the *identical* full-rate path at 0.947 ms in one
        // run and 1.368 ms in the next, purely because a sibling worktree's
        // build was loading the machine and the GPU clocked differently. Drift
        // that slow moves a whole block; alternating blocks split it evenly
        // between the two configurations instead of attributing all of it to
        // whichever ran second.
        constexpr u32 kRounds = 8;
        constexpr u32 kWarmFramesPerBlock = 3; // the timer ring resolves 1-3 frames behind
        constexpr u32 kSampledFramesPerBlock = 4;
        constexpr f32 kCaptureTime = 4.0f;

        [[nodiscard]] f64 Median(std::vector<f64> values)
        {
            if (values.empty())
                return 0.0;
            std::ranges::sort(values);
            return values[values.size() / 2u];
        }

        [[nodiscard]] f64 FindTiming(const std::vector<GPUPassTimerPool::PassTiming>& timings,
                                     std::string_view name)
        {
            for (const auto& t : timings)
            {
                if (t.Name == name)
                    return t.GpuMs;
            }
            return -1.0;
        }
    } // namespace

    class VRCSPerfProbe : public RendererAttachedTest
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
                dl.m_Direction = glm::normalize(glm::vec3(0.25f, -0.92f, 0.3f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 1.5f;
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
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
                return e;
            };

            addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f });
            // Several boxes, so the frame carries a realistic amount of
            // silhouette rather than one lonely cube on an otherwise empty
            // plane — the coarse fraction is the whole story here.
            for (int i = 0; i < 5; ++i)
            {
                const f32 x = static_cast<f32>(i - 2) * 7.0f;
                addMesh("Occluder", MeshPrimitive::Cube, { x, 2.5f, 0.0f }, { 5.0f, 5.0f, 5.0f });
            }
        }

        // Per-configuration samples. Everything is a LIST because the two
        // configurations are interleaved: samples accumulate across rounds and
        // are reduced once at the end.
        struct Samples
        {
            std::vector<f64> Gtao;
            std::vector<f64> Classify;
            std::vector<f64> Denoise;
            std::vector<f64> Whole; // classify + gtao + denoise, per frame

            [[nodiscard]] sizet Count() const
            {
                return Whole.size();
            }
        };

        void ApplyConfig(bool vrcsEnabled)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.GTAOEnabled = true;
            pp.GTAORadius = 1.5f;
            pp.GTAODenoiseEnabled = true;
            pp.GTAODenoisePasses = 4;
            pp.GTAODebugView = false;
            pp.VRCSEnabled = vrcsEnabled;
            pp.VRCSGTAO = true;
            pp.VRCSAllow4x4 = false;
            pp.VRCSDebugOverlay = false;
            Renderer3D::ApplyRendererSettings();
        }

        [[nodiscard]] static EditorCamera MakeCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 6.0f, 22.0f }, 0.0f, 0.30f);
            return camera;
        }

        // Render one block of frames in the CURRENT configuration and append its
        // sampled timings. The timer pool resolves 1-3 frames behind, so the
        // warm frames exist to let the ring refill after the configuration
        // change; a bracket that has not landed yet reports -1 and is skipped
        // rather than folded in as a zero, which would drag the median toward a
        // number no frame ever took.
        void SampleBlock(EditorCamera& camera, Samples& out)
        {
            RunEditorFrames(camera, kWarmFramesPerBlock);
            for (u32 i = 0; i < kSampledFramesPerBlock; ++i)
            {
                RunEditorFrames(camera, 1);
                const auto timings = GPUPassTimerPool::GetInstance().GetLastPassTimingsCopy();
                const f64 g = FindTiming(timings, "GTAOPass/GTAO");
                const f64 c = FindTiming(timings, "GTAOPass/VRCSClassify");
                const f64 d = FindTiming(timings, "GTAOPass/GTAO_Denoise");
                if (g >= 0.0)
                    out.Gtao.push_back(g);
                if (c >= 0.0)
                    out.Classify.push_back(c);
                if (d >= 0.0)
                    out.Denoise.push_back(d);
                // The whole-bracket sum is the number to believe. A GPU
                // timestamp is written when the command stream REACHES it, not
                // when the dispatch it follows has drained, so work can be
                // attributed to a neighbouring bracket: the interleaved probe
                // measured GTAO at 0.10 ms against 1.37 ms while the untouched
                // denoise bracket moved 0.07 -> 1.20 ms in the same run. The
                // per-dispatch splits below are for diagnosis; the sum is what
                // survives that attribution.
                if (g >= 0.0 && d >= 0.0)
                    out.Whole.push_back(g + d + std::max(c, 0.0));
            }
        }
    };

    TEST_F(VRCSPerfProbe, DISABLED_GtaoDispatchCostWithAndWithoutVariableRate)
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

        EditorCamera camera = MakeCamera();

        // One long warm-up before any sampling: shader compiles, transient-pool
        // allocation and the first graph build are one-off costs that belong in
        // neither configuration's numbers.
        ApplyConfig(false);
        RunEditorFrames(camera, 10);

        Samples full;
        Samples variable;
        for (u32 round = 0; round < kRounds; ++round)
        {
            ApplyConfig(false);
            SampleBlock(camera, full);
            ApplyConfig(true);
            SampleBlock(camera, variable);
        }

        const f64 fullWhole = Median(full.Whole);
        const f64 variableWhole = Median(variable.Whole);
        const f64 net = fullWhole - variableWhole;
        const f64 pct = fullWhole > 0.0 ? (net / fullWhole) * 100.0 : 0.0;

        const auto ms = [](f64 v)
        { return std::to_string(v) + " ms"; };

        std::string report;
        report += "\nVRCS GPU-time probe — " + std::to_string(kWidth) + "x" + std::to_string(kHeight) +
                  ", 2x2 only, " + std::to_string(kRounds) + " interleaved rounds of " +
                  std::to_string(kSampledFramesPerBlock) + " sampled frames (medians)\n";
        report += "\n  WHOLE GTAO BRACKET (classify + gtao + denoise) — the number to believe\n";
        report += "    full rate                : " + ms(fullWhole) + "\n";
        report += "    variable rate            : " + ms(variableWhole) + "\n";
        report += "    saved                    : " + ms(net) + "  (" + std::to_string(pct) + "%)\n";
        report += "\n  Component split — DIAGNOSIS ONLY, see the attribution note below\n";
        report += "    gtao      full / variable: " + ms(Median(full.Gtao)) + " / " + ms(Median(variable.Gtao)) + "\n";
        report += "    denoise   full / variable: " + ms(Median(full.Denoise)) + " / " +
                  ms(Median(variable.Denoise)) + "\n";
        report += "    classify        (variable): " + ms(Median(variable.Classify)) + "\n";
        report += "\nRead the WHOLE-BRACKET number. A GPU timestamp is written when the command\n"
                  "stream reaches it, not when the preceding dispatch has drained, so work migrates\n"
                  "between adjacent brackets: a run of this probe measured gtao at 0.10 vs 1.37 ms\n"
                  "while the denoise bracket — which VRCS does not touch — moved 0.07 -> 1.20 ms.\n"
                  "The split is useful for spotting WHERE something changed, not for quoting.\n"
                  "\nThe two configurations are interleaved because a straight A-then-B run is not a\n"
                  "controlled comparison on this box: the identical full-rate path measured 0.947 ms\n"
                  "in one run and 1.368 ms in the next, under a sibling worktree's build load.\n"
                  "\nThe saving is a property of the SCENE, not of the feature. This one is a cube\n"
                  "row on a large flat floor, which coarsens well; a frame of foliage would not.\n";

        // Reported, never asserted — this is an instrument, not a gate. The one
        // thing worth failing on is the probe failing to measure anything,
        // which would leave a future reader trusting a table of zeros.
        EXPECT_GT(full.Count(), 0u) << "no full-rate frames resolved a timing — the probe measured nothing"
                                    << report;
        EXPECT_GT(variable.Count(), 0u) << "no variable-rate frames resolved a timing" << report;
        EXPECT_GT(Median(variable.Classify), 0.0)
            << "no GTAOPass/VRCSClassify timing resolved — VRCS did not run" << report;

        RecordProperty("vrcs_whole_full_ms", std::to_string(fullWhole));
        RecordProperty("vrcs_whole_variable_ms", std::to_string(variableWhole));
        RecordProperty("vrcs_whole_saved_ms", std::to_string(net));
        RecordProperty("vrcs_classify_ms", std::to_string(Median(variable.Classify)));
        GTEST_LOG_(INFO) << report;
    }
} // namespace OloEngine::Tests
