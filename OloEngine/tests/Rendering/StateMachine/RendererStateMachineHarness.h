#pragma once

// =============================================================================
// The device half of the renderer state-machine harness (issue #1349).
//
// A fixture over the real renderer (RendererAttachedTest: a GL 4.6 context and
// the production Renderer3D pipeline, driven through Scene::OnUpdateEditor)
// that executes a Trace (StateMachineTrace.h) operation by operation and, at
// every checkpoint, renders the same state through each equivalence pair in the
// manifest and compares the results.
//
// What it substitutes, and therefore does not test (substituted-seams-compound.md):
//
//   * The canonical reset is not a new process. `ResetToCanonical` puts every
//     setting, the scene, the transient pool, the declaration cache and the
//     temporal histories back, but process-wide singletons the renderer never
//     resets (the stochastic frame index, shader programs already compiled,
//     GPU-scene slot allocators) carry over. A trace that only fails from a
//     cold process is replayed with --olo-state-machine-replay in a fresh one.
//   * Captures are read in post-pass hooks. On GL a hook changes nothing about
//     execution; on Vulkan it would decline every parallel recording group, which
//     is one of several reasons the Vulkan rows are live-only.
//   * The editor frame (ImGui, the viewport blit) is not rendered. The
//     fixture's GLStateGuard around every tick stands in for "something else
//     touched GL between frames", which is what ImGui does in the editor.
// =============================================================================

#include "PropertyTests/RendererAttachedTest.h"
#include "StateMachineTrace.h"

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class Model;
    class MeshSource;
    class Scene;
} // namespace OloEngine

namespace OloEngine::Tests::StateMachine
{
    // One intermediate target, read at a pinned point in the frame: right
    // after the last pass that reads it (or, with no readers, its first
    // writer). After that pass a transient's backing may legally be handed to
    // another resource, so a read at end of frame would compare two executions'
    // alias layouts rather than their contents.
    struct TargetCapture
    {
        std::string Name;
        std::string PinnedPass;
        u32 Width = 0;
        u32 Height = 0;
        std::vector<f32> Texels; // RGBA float, bottom-up
    };

    struct FrameCapture
    {
        std::vector<u8> Composite; // UIComposite RT0, RGBA8
        u32 Width = 0;
        u32 Height = 0;
        std::vector<TargetCapture> Targets;
        // Render-graph resolve failures during the captured frame.
        std::string ResolveFailures;
        // Tracked targets this frame did not capture, and why: not declared
        // this frame, not resolvable, or not a colour format a float readback
        // can take. Printed by the premise checks.
        std::string Skipped;
    };

    // How one target compared. `Exact` says which criterion it was held to:
    // true when both executions' own controls were bit-identical.
    struct TargetVerdict
    {
        std::string Name;
        bool Exact = true;
        bool Held = true;
        u32 DifferingTexels = 0;
        f64 MaxDelta = 0.0;
        std::string Detail;
    };

    struct Comparison
    {
        bool Held = true;
        bool AnyDistributionFallback = false;
        std::vector<TargetVerdict> Targets;
        [[nodiscard]] std::string Describe() const;
    };

    // Per-target noise measured from two consecutive frames of one state.
    struct ControlFloor
    {
        std::string Name;
        bool Exact = true;
        f64 MeanShift = 0.0;     // largest per-channel mean difference
        f64 HistogramL1 = 0.0;   // luminance histogram L1 distance, as a fraction of texels
    };

    // Compare `a` with `b`, holding each target exact when `controls` say it is
    // deterministic and at distribution level otherwise. Pure; CPU-tested.
    [[nodiscard]] Comparison CompareCaptures(const FrameCapture& a, const FrameCapture& b,
                                             const std::vector<ControlFloor>& controls);
    [[nodiscard]] std::vector<ControlFloor> MeasureControls(const FrameCapture& first, const FrameCapture& second);
    // The luminance-histogram L1 distance between two targets of equal size,
    // as a fraction of the texel count (0 = identical distributions, 2 = disjoint).
    [[nodiscard]] f64 HistogramDistance(const TargetCapture& a, const TargetCapture& b);
    [[nodiscard]] f64 MeanShift(const TargetCapture& a, const TargetCapture& b);

    struct PairFailure
    {
        std::string PairId;
        std::string Where; // "after op 4 (path deferred)" and the like
        std::string Detail;
    };

    struct TraceResult
    {
        std::vector<PairFailure> Failures;
        u32 Checkpoints = 0;
        [[nodiscard]] bool Passed() const
        {
            return Failures.empty();
        }
    };

    struct RunOptions
    {
        // Stop at the first checkpoint that fails; what minimisation wants.
        bool StopAtFirstFailure = true;
        // Only this pair (by manifest id) is checked; empty checks every pair.
        std::string OnlyPair;
        // Frames rendered after each operation before anything is compared.
        u32 SettleFrames = 3;
    };

    class RendererStateMachineFixture : public RendererAttachedTest
    {
      public:
        // Out of line: the members hold Refs to types this header only
        // forward-declares.
        RendererStateMachineFixture();
        ~RendererStateMachineFixture() override;

      protected:
        void BuildScene() override;
        void TearDown() override;

        // --- The state machine ----------------------------------------------
        void ResetToCanonical();
        void ApplyOp(const Op& op);
        // Reach `config` from the canonical reset with no history at all.
        void ConfigureDirectly(const ModelConfig& config);
        void RenderFrames(u32 count);

        // --- Captures and pairs ----------------------------------------------
        [[nodiscard]] FrameCapture CaptureFrame();
        // Every Required GL pair (or only `options.OnlyPair`) at the current
        // state. Appends failures, records comparisons in the coverage tally.
        void CheckPairs(const std::string& where, const RunOptions& options, TraceResult& result);
        // fresh-vs-sequence: the current state against the same ModelConfig
        // reached directly from the canonical reset.
        void CheckFreshVersusSequence(const std::string& where, const RunOptions& options, TraceResult& result);

        [[nodiscard]] TraceResult RunTrace(const Trace& trace, const RunOptions& options);

        // Run `trace`; on failure persist it, minimise it against the first
        // failing pair, and report both through ADD_FAILURE. Returns whether it
        // passed.
        bool RunTraceAndReport(const Trace& trace, const std::string& label);

        [[nodiscard]] const ModelConfig& CurrentConfig() const
        {
            return m_Config;
        }

        // --- Scene ------------------------------------------------------------
        void PopulateScene(Scene& scene, bool withBlendedMesh);
        void ReloadScene();
        void SetBlendedMesh(bool present);
        [[nodiscard]] EditorCamera MakeCamera() const;
        [[nodiscard]] Size CurrentSize() const;

        // The canonical settings every trace starts from, captured once in
        // BuildScene after the deterministic post chain is selected.
        RendererSettings m_BaselineRenderer{};
        PostProcessSettings m_BaselinePost{};

      private:
        void ApplyFeature(FeatureId feature, bool on);
        void RenderOtherSceneFrame();
        [[nodiscard]] bool TraceStillFails(const Trace& trace, const std::string& pairId);

        ModelConfig m_Config{};
        Ref<Scene> m_OtherScene;
        Ref<Model> m_FieldModel;
        std::string m_FieldModelPath;
        Ref<MeshSource> m_Cube;
        Ref<MeshSource> m_Plane;
        Entity m_BlendedMesh;
        bool m_SavedDoubleBuffering = true;
        bool m_LeversSaved = false;
        bool m_SavedVerify = false;
        bool m_SavedDisableAliasing = false;
        bool m_SavedSerialSubmission = false;
        bool m_SavedFaultStaleKey = false;
        bool m_SavedFaultBindingReset = false;
        bool m_SavedFaultShortLifetimes = false;
    };

    // The committed regression corpus (manifest kRegressions), resolved against
    // the source tree.
    [[nodiscard]] std::string CorpusDirectory();
    [[nodiscard]] std::optional<Trace> LoadTraceFile(const std::string& path, std::string* error = nullptr);
} // namespace OloEngine::Tests::StateMachine
