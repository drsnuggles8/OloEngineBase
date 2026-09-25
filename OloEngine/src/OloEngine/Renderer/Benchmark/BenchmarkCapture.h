#pragma once

// =============================================================================
// BenchmarkCapture — backend-neutral attachment capture + self-describing
// result directory for the renderer benchmark manifests (issue #974).
//
// This is the shared back half of both capture front doors (the test binary's
// `--olo-capture-manifest=` tool mode and the editor MCP tool). The FRAME
// DRIVING deliberately stays host-specific — the test host steps the mock
// clock through Scene::OnUpdateEditor; the editor host counts its own live
// frames — but the moment of capture and the result-directory contract are
// one implementation, so the two fronts cannot drift.
//
// Readback rides `RenderCommand::ReadTextureSubImage` (the same spine every
// MCP diagnostic uses), never raw glGetTextureImage, so it works under both
// backends; row order comes from the one per-backend predicate
// (`RHI::RenderTargetRowsAreBottomUp`). PNG output quantises floats to 8-bit
// exactly like `olo_render_capture_target`; HDR output writes Radiance .hdr
// with the full float values — the export path the PNG clamp cannot provide.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"

#include <filesystem>
#include <optional>
#include <string>
#include <span>

namespace OloEngine
{
    class Scene;
}

namespace OloEngine::Benchmark
{
    struct CapturedAttachment
    {
        ManifestAttachment Spec;
        FString FileName;     // "<Name>.png" / "<Name>.hdr" (empty when skipped/failed)
        TArray<u8> FileBytes; // encoded file content
        u32 Width = 0;
        u32 Height = 0;
        FString FormatName; // source render-graph format
        bool IsDepth = false;
        bool Normalized = false;
        f32 MinValue = 0.0f;
        f32 MaxValue = 0.0f;
        bool SkippedUnsupported = false; // skipped by declaration, not failure
        FString SkipReason;              // recorded when SkippedUnsupported (defaulted
                                         // to the backend declaration by the writer)
        FString Error;                   // non-empty on capture failure
    };

} // namespace OloEngine::Benchmark

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<Benchmark::CapturedAttachment>
    {
        using Record = Benchmark::CapturedAttachment;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Spec)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FileName)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FileBytes)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Width)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Height)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FormatName)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::IsDepth)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Normalized)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::MinValue)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::MaxValue)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::SkippedUnsupported)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::SkipReason)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Error)>;
    };
} // namespace OloEngine

namespace OloEngine::Benchmark
{
    struct CameraCaptureSet
    {
        FString CameraId;
        u32 CaptureFrameIndex = 0; // frames rendered when this capture was taken
        TArray<CapturedAttachment> Attachments;
    };

    struct PassTimingRecord
    {
        FString Name;
        // The measurement and its validity (#1337). A pass whose timestamps
        // were dropped, refused or read back out of order has NO number, and
        // result.json writes null with a status rather than 0.0 — which a
        // reader of a persisted export has no way to question later.
        GpuTimingSample Sample{};
        // Stated by the producer: a sub-pass interval is inside its parent's,
        // so a consumer that sums the list must skip it.
        bool IsSubPass = false;
        FString ParentName;
    };

} // namespace OloEngine::Benchmark

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<Benchmark::CameraCaptureSet>
    {
        using Record = Benchmark::CameraCaptureSet;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::CameraId)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::CaptureFrameIndex)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Attachments)>;
    };
    template<>
    struct TIsTriviallyRelocatable<Benchmark::PassTimingRecord>
    {
        using Record = Benchmark::PassTimingRecord;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Sample)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::IsSubPass)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::ParentName)>;
    };
} // namespace OloEngine

namespace OloEngine::Benchmark
{
    // Frame-level renderer counters recorded into result.json ("renderer
    // timings and memory counters" — the issue-#974 metadata requirement).
    //
    // Every field here has a live producer, and that is a TEST rather than a
    // claim: GpuTimingPoolEvidenceTest.EveryExportedRendererCounterHasALive
    // Producer renders real frames and requires each one to have moved. #1337
    // criterion 3 requires a field with no producer to be removed or marked,
    // never left reading a plausible zero.
    struct RendererCounters
    {
        u32 DrawCalls = 0;
        u32 TrianglesRendered = 0;
        u32 InstancesRendered = 0;
        u64 GpuMemoryTotalBytes = 0;
    };

    // The resolution the frame was actually rendered and presented at (#1337
    // criterion 4). The manifest's declared Width/Height is a REQUEST: the
    // render graph applies its own render scale on top, so the scene can be
    // rasterized at one size and displayed at another, and a benchmark record
    // that carries only the request cannot be compared against one taken at a
    // different scale.
    //
    // Known issue #1397: a non-native UpscaleMode crops the editor viewport
    // instead of scaling it, so on the editor host these dimensions describe
    // the buffers while the visible framing is a crop of them. Recorded as
    // measured; the discrepancy belongs to #1397.
    struct ResolutionRecord
    {
        u32 RenderWidth = 0; ///< Rasterization size (physical * render scale).
        u32 RenderHeight = 0;
        u32 DisplayWidth = 0; ///< Presented/physical framebuffer size.
        u32 DisplayHeight = 0;
        f32 RenderScale = 1.0f;
        bool Measured = false; ///< False when no live graph could be asked.
    };

    // How trustworthy this run's GPU timings are, recorded beside them (#1337
    // criterion 4). A persisted export outlives the session that made it, so
    // "the numbers were stale" has to be IN the file.
    struct TimingValidity
    {
        u64 MeasurementFrameId = 0; ///< Frame the pass timings describe.
        u64 CurrentFrameId = 0;     ///< Frame the counters describe.
        u64 AgeFrames = 0;          ///< Difference; 1-3 is the designed latency.
        u32 DroppedSlots = 0;       ///< Frames the timer ring never measured.
        u32 UnstampedFrames = 0;    ///< Frames the backend declined to stamp. A
                                    ///< separate fault: the instrument is not
                                    ///< working, rather than the GPU being behind.
        bool Stale = false;         ///< Age at or beyond the ring size.
        GpuTimingStatus FrameStatus = GpuTimingStatus::Unavailable;
    };

    struct MeasuredFrame
    {
        FString CameraId;
        u32 Index = 0;
        f64 RenderCallMs = 0.0; // wall-clock Scene::OnUpdateEditor call, excluding readback
        f64 CpuMs = 0.0;        // profiler's last completed frame; independent clock
        f64 FenceWaitMs = 0.0;
        f64 PresentWaitMs = 0.0;
        u64 GpuFrameId = 0; // may lag this row; never infer same-frame attribution
        GpuTimingSample Gpu{};
        u64 TrackedRendererBytes = 0; // tracker combines CPU and GPU allocations
        u32 DrawCalls = 0;
    };

} // namespace OloEngine::Benchmark

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<Benchmark::MeasuredFrame>
    {
        using Record = Benchmark::MeasuredFrame;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::CameraId)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Index)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::RenderCallMs)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::CpuMs)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FenceWaitMs)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::PresentWaitMs)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::GpuFrameId)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Gpu)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::TrackedRendererBytes)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::DrawCalls)>;
    };
} // namespace OloEngine

namespace OloEngine::Benchmark
{

    struct MeasurementRecord
    {
        TArray<MeasuredFrame> Frames;
        f32 DeadlineMs = 0.0f;
    };

    [[nodiscard]] MeasuredFrame SnapshotMeasuredFrame(std::string_view cameraId, u32 index, f64 renderCallMs);
    [[nodiscard]] MeasuredFrame SnapshotEditorMeasuredFrame(std::string_view cameraId, u32 index);

    struct AppliedConfiguration
    {
        RenderingPath Path = RenderingPath::Deferred;
        u32 MSAASampleCount = 1;
        UpscaleMode Upscale = UpscaleMode::Off;
        UpscalerTechnique Technique = UpscalerTechnique::Spatial;
        bool RayTracedShadowsRequested = false;
    };
    [[nodiscard]] AppliedConfiguration SnapshotAppliedConfiguration();
    [[nodiscard]] bool ApplyEntityMotion(Scene& scene, const BenchmarkManifest& manifest, u32 frameIndex,
                                         std::string& outError);
    [[nodiscard]] bool ValidatePresetAdmission(const BenchmarkManifest& manifest, std::string_view backend,
                                               std::string& outError);

    // Host-supplied provenance for result.json.
    struct RunInfo
    {
        FString Backend; // "opengl" | "vulkan"
        FString GpuVendor;
        FString GpuRenderer;
        FString CommitSha; // "unknown" when unavailable
        FString MachineTag;
        FString Host; // "test-binary" | "editor-mcp"
        u32 TotalFramesRendered = 0;
        // Set when a host could not confirm the declared warm-up frames
        // actually rendered (the editor host counts live frames and can time
        // out). The capture is still written — partial evidence is useful for
        // live inspection — but `capturedPose` is derived from the declared
        // frame count, so with this set it may describe a frame that never
        // rendered. result.json says so rather than asserting a pose it cannot
        // back.
        //
        // Despite the name, the editor host ALSO sets it when a MEASUREMENT
        // frame wait times out: McpToolsBenchmark then ends that camera's
        // sampling early, so the run has fewer measured frames than the
        // manifest declares and is not a valid budget sample.
        bool WarmupTimedOut = false;
        f32 FinalMockTimeSeconds = 0.0f;
        TArray<PassTimingRecord> PassTimings;
        TimingValidity Timing;
        ResolutionRecord Resolution;
        RendererCounters Counters;
        MeasurementRecord Measurement;
        std::optional<u64> TrackedRendererBytesAfterSceneRelease;
        AppliedConfiguration Configuration;
    };

    // Per-capture context a derivation needs (Derive: linear-depth converts
    // hardware depth to view-space metres with the capturing camera's planes).
    struct CaptureContext
    {
        f32 CameraNearClip = 0.05f;
        f32 CameraFarClip = 1000.0f;
    };

    /// Capture one manifest attachment from the ACTIVE render graph at native
    /// resolution. Must run on the render thread with a frame's results
    /// resident (i.e. after the warm-up frames). Never downscales.
    [[nodiscard]] CapturedAttachment CaptureAttachment(const ManifestAttachment& spec,
                                                       const CaptureContext& context = {});

    /// Apply the manifest's renderer-side state — the settings a scene cannot
    /// serialize (rendering path, DDGI, TAA, ...), the exposure mode, the
    /// render scale, and the deliberate `Upscale = Off` pin (FSR2's temporal
    /// locks decay on REAL time by contract, so an upscaler would make a
    /// mock-clock capture nondeterministic). ONE implementation for both
    /// front doors, so a new manifest knob cannot land in one host only.
    /// Must run on the render thread.
    void ApplyManifestRendererState(const BenchmarkManifest& manifest);

    /// Capture every manifest attachment for one camera, honouring the
    /// backend's declared-unsupported list (skipped entries are recorded, not
    /// errors). Must run on the render thread after the camera's warm-up.
    [[nodiscard]] CameraCaptureSet CaptureCameraSet(const BenchmarkManifest& manifest, std::string_view cameraId,
                                                    u32 captureFrameIndex, std::string_view backend,
                                                    const CaptureContext& context);

    /// The per-pass GPU timings of the most recently resolved frame, each with
    /// its validity.
    [[nodiscard]] TArray<PassTimingRecord> SnapshotPassTimings();

    /// How trustworthy those timings are: which frame they describe, how old
    /// they are, and how many frames the timer ring lost outright.
    [[nodiscard]] TimingValidity SnapshotTimingValidity();

    /// The resolution the active render graph is actually rendering and
    /// presenting at. `Measured` is false when there is no live graph.
    [[nodiscard]] ResolutionRecord SnapshotResolution();

    /// Frame-level draw/triangle/instance counters + tracked GPU memory.
    [[nodiscard]] RendererCounters SnapshotRendererCounters();

    /// `git rev-parse HEAD`, degraded to "unknown" — provenance metadata for
    /// result.json, never something a capture may fail over.
    [[nodiscard]] std::string QueryCommitShaViaGit();

    /// The machine tag result.json records — same key the perf history uses,
    /// so captures and timings join. `overrideTag` (e.g. --olo-perf-machine)
    /// wins; otherwise COMPUTERNAME / HOSTNAME / "unknown", sanitized.
    [[nodiscard]] std::string ResolveMachineTag(std::string_view overrideTag);

    /// Write the self-describing result directory: every captured file, a
    /// verbatim copy of the manifest, and result.json (schema, provenance,
    /// determinism echo, per-attachment metadata, pass timings). Returns
    /// false and fills `outError` on any I/O failure.
    [[nodiscard]] bool WriteResultDirectory(const BenchmarkManifest& manifest,
                                            const std::filesystem::path& manifestSourcePath,
                                            const std::filesystem::path& outDir,
                                            std::span<const CameraCaptureSet> cameraSets,
                                            const RunInfo& runInfo,
                                            std::string& outError);
} // namespace OloEngine::Benchmark
