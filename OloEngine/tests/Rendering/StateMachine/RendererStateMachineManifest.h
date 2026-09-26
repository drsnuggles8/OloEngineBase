#pragma once

// =============================================================================
// The coverage manifest of the renderer state-machine harness (issue #1349).
//
// This file is the finite list of what the harness must cover: the operations
// a generated sequence is built from, the execution pairs that must render the
// same thing, the comparison criterion each pair is held to, the backends, the
// negative controls, and the regression traces. It was committed before the
// harness it describes, so the harness grows toward it rather than the manifest
// being written afterwards to fit whatever got built.
//
// Completion is NOT "every combination ran". The Cartesian product of paths,
// features, sizes and orders is unbounded; the manifest is the bound. Every row
// is in one of three states and none of them can go quiet:
//
//   * Required   -- a test executes it. `RendererStateMachineManifest.EveryRequiredCaseHasAnExecutingTest`
//                   fails if a Required row names no test, and the run-end
//                   report (StateMachineCoverage) prints each Required row as
//                   executed or skipped, with the skip's reason, after gtest's
//                   own summary.
//   * Prerequisite -- cannot run yet because something it needs does not exist.
//                   The row names the issue that owns the gap. A Prerequisite
//                   row with no issue fails the manifest test.
//   * LiveOnly   -- cannot run in this process on any machine (a fact about
//                   the fixtures, not a choice), so its evidence is a live
//                   editor measurement recorded in the PR. The row says why.
//
// Adding a case: add a row here, then the test that executes it, naming the
// row's Id in `Coverage::RecordComparison` (or `Coverage::RecordVacuous` when
// the premise is absent). Removing a Required row
// is a scope change and belongs in the PR description, not in a quiet edit.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>

namespace OloEngine::Tests::StateMachine
{
    enum class RowStatus : u8
    {
        Required,
        Prerequisite,
        LiveOnly
    };

    // Where a row executes. CPU rows run in every job, including the hosted
    // runners with no GPU. Device rows need a GL 4.6 context and skip without
    // one, which the run-end report counts.
    enum class Placement : u8
    {
        CpuEverywhere,
        DeviceGL,
        LiveVulkan
    };

    // -------------------------------------------------------------------------
    // Comparison criteria. Every pair names exactly one.
    // -------------------------------------------------------------------------
    enum class Criterion : u8
    {
        // Bit-identical texels on every captured target whose own control pair
        // (the same state rendered on two consecutive frames) is bit-identical.
        // A target whose control is NOT identical -- something in it depends on
        // the frame index -- cannot be held to this, so that one target is
        // compared under Distribution instead, and the run reports how many
        // comparisons were exact and how many fell back. A pair never passes
        // because its control was noisy; it passes a weaker test, and says so.
        ExactTexels,
        // Same distribution, not the same texels: per-channel mean, a 16-bin
        // luminance histogram and 16x16 tile means (the spatial term: the
        // first two are blind to a permutation), compared against the noise
        // floor a same-state control pair measures. A target the controls
        // could not calibrate fails; it never becomes a tolerance. For temporally accumulated or
        // stochastic beauty (TAA on), where two equivalent executions converge
        // to the same image by different paths.
        Distribution,
        // RenderGraph::ComputeCompiledPlanDigest equality: declarations,
        // descriptors, imported identities, culled state and order.
        PlanDigest,
        // A property of one execution checked against an independent model:
        // an alias plan has no two overlapping lifetimes in one slot; a replay
        // order keeps every blended draw back to front.
        Invariant
    };

    struct CriterionRow
    {
        Criterion Id;
        std::string_view Name;
        std::string_view Definition;
    };

    inline constexpr std::array kCriteria{
        CriterionRow{ Criterion::ExactTexels, "exact-texels",
                      "0 differing texels (float bits) on each target whose same-state control pair is itself 0; a "
                      "target with a noisy control is compared under distribution and counted as a fallback" },
        CriterionRow{ Criterion::Distribution, "distribution",
                      "per-channel mean and 16-bin luminance histogram L1 within max(2 x control, floor), 16x16 tile means "
                      "within max(3 x control, 1% of the brightest tile); every target first finite, same shape, and "
                      "calibrated by its control" },
        CriterionRow{ Criterion::PlanDigest, "plan-digest", "RenderGraph::ComputeCompiledPlanDigest equal" },
        CriterionRow{ Criterion::Invariant, "invariant",
                      "an independent model of the property (lifetimes recomputed from declarations, compositing "
                      "order recomputed from depth) agrees with the execution" },
    };

    // -------------------------------------------------------------------------
    // Operations. A generated trace is a sequence of these. Every operation is
    // TOTAL -- legal in every state -- so any subsequence of a valid trace is
    // valid, which is what makes minimisation sound.
    // -------------------------------------------------------------------------
    struct OperationRow
    {
        std::string_view Id;
        // The acceptance-criterion wording in #1349 this operation covers.
        std::string_view Covers;
        std::string_view Entry; // the production entry point it drives
        RowStatus Status;
        std::string_view Note;
    };

    inline constexpr std::array kOperations{
        OperationRow{ "resize", "resize", "RendererAttachedTest::ResizeRenderTarget (Renderer3D::OnWindowResize path)",
                      RowStatus::Required, "Also produces the non-native resolutions: sizes are drawn from a finite set." },
        OperationRow{ "path", "path/feature changes", "RendererSettings::Path + Renderer3D::ApplyRendererSettings",
                      RowStatus::Required, "Forward, Forward+, Deferred." },
        OperationRow{ "feature", "path/feature changes", "PostProcessSettings fields (+ ApplyRendererSettings for AO)",
                      RowStatus::Required, "Bloom, FXAA, GTAO, GTAO denoise, SSR, vignette + grading." },
        OperationRow{ "msaa", "path/feature changes", "RendererSettings::Deferred.MSAASampleCount + ApplyRendererSettings",
                      RowStatus::Required, "1 and 4; set on every path, read only by Deferred." },
        OperationRow{ "upscale", "path/feature changes", "PostProcessSettings::Upscale", RowStatus::Required,
                      "Off, Performance, Quality: the scene band moves while the display size does not (#563)." },
        OperationRow{ "shader-reload", "hot reload", "ShaderLibrary::ReloadShaders", RowStatus::Required,
                      "Every library shader recompiles; program identities change." },
        OperationRow{ "scene-reload", "destruction/recreation", "Scene::DestroyEntity on every entity, then rebuild",
                      RowStatus::Required, "GPU-scene records, submissions and per-entity resources are all new." },
        OperationRow{ "entity-churn", "destruction/recreation", "Scene::DestroyEntity + CreateEntity of one mesh",
                      RowStatus::Required,
                      "Toggles a blended mesh in and out, so bucket-gated passes gain and lose draws (#1315)." },
        OperationRow{ "fence-drain", "delayed fences", "FrameResourceManager::WaitForFrame + FlushAllDeletionQueues",
                      RowStatus::Required, "Retires every in-flight frame and deferred deletion now instead of later." },
        OperationRow{ "frames-in-flight", "delayed fences", "FrameResourceManager::SetDoubleBufferingEnabled",
                      RowStatus::Required, "One versus two buffered frames: when fences are waited on and allocators reused." },
        OperationRow{ "scene-swap", "scene ownership changes", "Render a second Scene for a frame, then the first again",
                      RowStatus::Required, "A different scene owns the renderer's per-scene state for one frame." },
        OperationRow{ "history-advance", "frame history advancement",
                      "Frames with camera motion + Renderer3D::InvalidateTemporalHistories", RowStatus::Required,
                      "TAA history accumulates, is invalidated, and comes back." },
        OperationRow{ "camera-move", "frame history advancement", "EditorCamera pose", RowStatus::Required,
                      "Execution-only: must never recompile the graph." },
        OperationRow{ "pool-trim", "destruction/recreation", "RenderGraph::GetTransientPool().Clear", RowStatus::Required,
                      "Every pooled transient is released; the next frame acquires fresh storage." },
    };

    // -------------------------------------------------------------------------
    // Equivalence pairs: two executions that must render the same thing.
    // -------------------------------------------------------------------------
    struct PairRow
    {
        std::string_view Id;
        std::string_view A;
        std::string_view B;
        std::string_view Lever; // how the harness selects B
        Criterion Compare;
        Placement Where;
        RowStatus Status;
        std::string_view Owner; // test that executes it, or the issue that blocks it
        std::string_view Note;
    };

    inline constexpr std::array kPairs{
        PairRow{ "cached-vs-rebuild.gl", "frame served from the declaration cache", "same frame compiled from scratch",
                 "Levers::VerifyDeclarationCache for one frame", Criterion::ExactTexels, Placement::DeviceGL,
                 RowStatus::Required, "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair", "" },
        PairRow{ "cached-vs-rebuild.plan", "cached compiled plan", "forced rebuild's compiled plan",
                 "Levers::VerifyDeclarationCache for one frame", Criterion::PlanDigest, Placement::DeviceGL,
                 RowStatus::Required, "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair",
                 "At every checkpoint the cached frame is re-compiled and its plan compared (stale detections)." },
        PairRow{ "cached-vs-rebuild.cpu", "RenderGraph cached build of mock passes", "forced rebuild",
                 "RenderGraph::InvalidateBuildFrameGraphCache", Criterion::PlanDigest, Placement::CpuEverywhere,
                 RowStatus::Required, "RendererStateMachinePolicy.CachedGraphMatchesARebuildOverGeneratedSequences",
                 "The cache mechanism itself, over randomised pass gates, with no device." },
        PairRow{ "alias-vs-noalias.gl", "transient pool with alias-slot sharing", "every transient on its own backing",
                 "Levers::DisableTransientAliasing + pool Clear", Criterion::ExactTexels, Placement::DeviceGL,
                 RowStatus::Required, "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair", "" },
        PairRow{ "alias-plan.cpu", "planner's alias-slot assignment", "lifetimes recomputed from the declarations",
                 "none (independent model)", Criterion::Invariant, Placement::CpuEverywhere, RowStatus::Required,
                 "RendererStateMachinePolicy.AliasPlansNeverShareASlotAcrossOverlappingLifetimes",
                 "Randomised graphs, version renames and end-of-frame extractions." },
        PairRow{ "batch-vs-nobatch.gl", "geometry bucket auto-batching on", "auto-batching off",
                 "SceneRenderPass command bucket EnableBatching", Criterion::ExactTexels, Placement::DeviceGL,
                 RowStatus::Required, "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair", "" },
        PairRow{ "batch-order.cpu", "sorted + batched replay order", "sorted, unbatched replay order",
                 "CommandBucketConfig::EnableBatching", Criterion::Invariant, Placement::CpuEverywhere,
                 RowStatus::Required, "RendererStateMachineDrawOrder.BatchingAndParallelSubmissionKeepBlendedDrawOrder",
                 "Every blended draw back to front, with batching and with parallel submission." },
        PairRow{ "serial-vs-parallel.cpu-submit", "packets submitted from one thread",
                 "packets submitted from worker threads and merged", "CommandBucket::SubmitPacketParallel",
                 Criterion::Invariant, Placement::CpuEverywhere, RowStatus::Required,
                 "RendererStateMachineDrawOrder.BatchingAndParallelSubmissionKeepBlendedDrawOrder", "" },
        PairRow{ "serial-vs-parallel.gl", "SubmitMeshesParallel on the worker pool (DrawMeshParallel)",
                 "the same batch on the calling thread (DrawMesh)", "Levers::SerialMeshSubmission",
                 Criterion::ExactTexels, Placement::DeviceGL, RowStatus::Required,
                 "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair",
                 "GL never forks recording, so its CPU-parallel half is mesh submission. The scene carries a "
                 "40-mesh model so the production ModelComponent path crosses the 32-mesh parallel threshold. "
                 "Levers::NoThreading is latched once per process and cannot be the lever." },
        PairRow{ "serial-vs-parallel.vulkan", "Vulkan parallel graph recording", "every region recorded inline",
                 "Levers::VulkanParallelRecording", Criterion::ExactTexels, Placement::LiveVulkan, RowStatus::LiveOnly,
                 "PR body: live editor A/B",
                 "Scene-level Vulkan is unreachable in-process (testing-architecture.md s9). The pass-level half is "
                 "VulkanParallelRecordingDevice.* (#806); the scene-level half is a live capture." },
        PairRow{ "binding-cache-cold.gl", "dispatcher binding caches warm across passes and frames",
                 "every binding cache forgotten before the frame and after every pass",
                 "CommandDispatch::InvalidateBindingCaches in a post-pass hook", Criterion::ExactTexels,
                 Placement::DeviceGL, RowStatus::Required, "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair",
                 "The redundant-bind cache is an optimisation: a frame that relies on a binding nobody reset (a "
                 "missing frame-start reset, or #1404's missing per-pass one) renders differently cold. A skipped "
                 "reset breaks every frame alike, so fresh-vs-sequence cannot see it; this pair can." },
        PairRow{ "fresh-vs-sequence.gl", "state reached by a generated sequence",
                 "the same configuration reached from the canonical reset", "Harness Reset + direct configure",
                 Criterion::ExactTexels, Placement::DeviceGL, RowStatus::Required,
                 "RendererStateMachineEvidence.GeneratedSequencesHoldEveryPair",
                 "The state-machine relation: where a frame ends up must not depend on how it got there." },
        PairRow{ "fresh-vs-reloaded.gl", "frame before a shader or scene reload", "frame after it",
                 "shader-reload / scene-reload", Criterion::ExactTexels, Placement::DeviceGL, RowStatus::Required,
                 "RendererStateMachineEvidence.ReloadsAreIdentityOperations", "" },
        PairRow{ "temporal-fresh-vs-sequence.gl", "TAA beauty after a generated sequence",
                 "TAA beauty from the canonical reset", "Harness Reset + direct configure", Criterion::Distribution,
                 Placement::DeviceGL, RowStatus::Required, "RendererStateMachineEvidence.TemporalBeautyMatchesInDistribution",
                 "The stochastic half of the split: same distribution, not the same texels." },
        PairRow{ "isolated-vs-shared-view", "a view rendered alone", "the same view rendered beside a second view",
                 "none yet", Criterion::ExactTexels, Placement::DeviceGL, RowStatus::Prerequisite, "#1352",
                 "Needs two real scene views with isolated histories, culling and exposure; #1352 is blocked by "
                 "#1330 (view/frame ownership). No partial substitute is counted." },
        PairRow{ "sequences.vulkan", "generated sequences on Vulkan", "same pairs as the GL rows",
                 "--rhi vulkan editor + MCP", Criterion::ExactTexels, Placement::LiveVulkan, RowStatus::LiveOnly,
                 "PR body: live editor A/B",
                 "RendererAttachedTest holds a GL context only (testing-architecture.md s9/s10); a Vulkan scene "
                 "fixture is refused there on cost. Hardware Vulkan CI is #1294/#1358." },
    };

    // -------------------------------------------------------------------------
    // Negative controls: a known-bad mutation, injected through a lever, that a
    // named check must catch. A control that survives is a finding about the
    // check, not a pass.
    // -------------------------------------------------------------------------
    struct NegativeControlRow
    {
        std::string_view Id;
        std::string_view Fault;
        std::string_view Lever;
        std::string_view CaughtBy; // the checks that must report it
        Placement Where;
        std::string_view Owner;
    };

    inline constexpr std::array kNegativeControls{
        NegativeControlRow{ "stale-declaration-key", "bad cache invalidation: per-pass declaration inputs left out of the key",
                            "Levers::FaultStaleDeclarationKey",
                            "cached-vs-rebuild.plan (stale detection) and fresh-vs-sequence.gl", Placement::DeviceGL,
                            "RendererStateMachineNegativeControl.StaleDeclarationKeyIsCaught" },
        NegativeControlRow{ "stale-declaration-key.cpu", "a mock pass whose Setup gate is not reported to the key",
                            "test-local pass", "cached-vs-rebuild.cpu", Placement::CpuEverywhere,
                            "RendererStateMachinePolicy.UnreportedSetupGateIsCaught" },
        NegativeControlRow{ "missing-binding-reset", "the dispatcher's bound-state cache survives into the next frame",
                            "Levers::FaultSkipDispatchBindingReset", "binding-cache-cold.gl", Placement::DeviceGL,
                            "RendererStateMachineNegativeControl.MissingBindingResetIsCaught" },
        NegativeControlRow{ "alias-lifetime", "the transient planner ends every lifetime one pass early",
                            "Levers::FaultShortenTransientLifetimes", "alias-plan.cpu and alias-vs-noalias.gl",
                            Placement::DeviceGL, "RendererStateMachineNegativeControl.ShortenedAliasLifetimeIsCaught" },
        NegativeControlRow{ "alias-lifetime.cpu", "the same planner fault on randomised graphs",
                            "Levers::FaultShortenTransientLifetimes", "alias-plan.cpu", Placement::CpuEverywhere,
                            "RendererStateMachinePolicy.ShortenedLifetimesAreCaughtByTheIndependentModel" },
        NegativeControlRow{ "draw-order.cpu", "a batcher that groups blended draws on a partial key",
                            "test-local key", "batch-order.cpu", Placement::CpuEverywhere,
                            "RendererStateMachineDrawOrder.PartialKeyBatchingIsCaught" },
    };

    // -------------------------------------------------------------------------
    // Regression traces: the sequences that broke before, in the harness's own
    // operation language, replayed on every run. The trace files live in
    // corpus/ next to this header; the harness replays each against every
    // Required GL pair.
    // -------------------------------------------------------------------------
    struct RegressionRow
    {
        std::string_view File; // under OloEngine/tests/Rendering/StateMachine/corpus/
        std::string_view Origin;
        std::string_view What;
    };

    inline constexpr std::array kRegressions{
        RegressionRow{ "issue-530-reenter-deferred.trace", "#530",
                       "Re-entering Deferred recomputed a matching fingerprint over a wiped blackboard; the graph culled." },
        RegressionRow{ "issue-563-upscale-off-same-display.trace", "#563",
                       "Upscale off at an unchanged display size left stale reduced-band transients in the pool." },
        RegressionRow{ "issue-1315-bucket-gains-first-draw.trace", "#1315",
                       "A bucket-gated pass cached empty; its first draw did not move the key." },
        RegressionRow{ "issue-1397-repopulate-under-cached-graph.trace", "#1397",
                       "SSR history resize re-populated the blackboard under a cached graph." },
        RegressionRow{ "issue-1333-denoise-gate.trace", "#1333 (U3)",
                       "Turning GTAO denoise on did not move the key; Execute found no pong." },
        RegressionRow{ "frames-in-flight-off-double-deletes-fence.trace", "#1349 (found here)",
                       "Double buffering off deleted a frame fence twice, destroying another owner's reissued fence." },
        RegressionRow{ "batched-model-inherits-stale-gpuscene-ref.trace", "#1349 (found here)",
                       "An auto-batched Model kept the previous batch's GPU-scene references and took its material." },
    };

    // Generator seeds replayed in the ordinary suite. Fixed so a red run names
    // a seed anyone can replay; `--olo-state-machine-seeds=` runs others.
    // A seed that ever found a real defect is added to kRegressions as a
    // minimised trace, not left here.
    inline constexpr std::array<u64, 4> kSmokeSeeds{ 1349u, 20260924u, 0x5EEDu, 7u };

    // -------------------------------------------------------------------------
    // Backends.
    // -------------------------------------------------------------------------
    struct BackendRow
    {
        std::string_view Backend;
        Placement Where;
        std::string_view Evidence;
    };

    inline constexpr std::array kBackends{
        BackendRow{ "CPU (no device)", Placement::CpuEverywhere,
                    "Every job, including hosted runners: generator, replay, minimiser, cache-key, alias-plan and "
                    "draw-order policy." },
        BackendRow{ "OpenGL 4.6", Placement::DeviceGL,
                    "RendererAttachedTest; artefact-backed. Skips with no context, counted by the run-end report." },
        BackendRow{ "Vulkan", Placement::LiveVulkan,
                    "Live-only for scene sequences (no in-process scene fixture). Pass-level parallel recording is "
                    "VulkanParallelRecordingDevice.*, nightly on lavapipe (#1301)." },
    };
} // namespace OloEngine::Tests::StateMachine
