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

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Benchmark
{
    struct CapturedAttachment
    {
        ManifestAttachment Spec;
        std::string FileName;      // "<Name>.png" / "<Name>.hdr" (empty when skipped/failed)
        std::vector<u8> FileBytes; // encoded file content
        u32 Width = 0;
        u32 Height = 0;
        std::string FormatName; // source render-graph format
        bool IsDepth = false;
        bool Normalized = false;
        f32 MinValue = 0.0f;
        f32 MaxValue = 0.0f;
        bool SkippedUnsupported = false; // skipped by declaration, not failure
        std::string SkipReason;          // recorded when SkippedUnsupported (defaulted
                                         // to the backend declaration by the writer)
        std::string Error;               // non-empty on capture failure
    };

    struct CameraCaptureSet
    {
        std::string CameraId;
        u32 CaptureFrameIndex = 0; // frames rendered when this capture was taken
        std::vector<CapturedAttachment> Attachments;
    };

    struct PassTimingRecord
    {
        std::string Name;
        f64 GpuMs = 0.0;
    };

    // Frame-level renderer counters recorded into result.json ("renderer
    // timings and memory counters" — the issue-#974 metadata requirement).
    struct RendererCounters
    {
        u32 DrawCalls = 0;
        u32 TrianglesRendered = 0;
        u32 InstancesRendered = 0;
        u64 GpuMemoryTotalBytes = 0;
    };

    // Host-supplied provenance for result.json.
    struct RunInfo
    {
        std::string Backend; // "opengl" | "vulkan"
        std::string GpuVendor;
        std::string GpuRenderer;
        std::string CommitSha; // "unknown" when unavailable
        std::string MachineTag;
        std::string Host; // "test-binary" | "editor-mcp"
        u32 TotalFramesRendered = 0;
        f32 FinalMockTimeSeconds = 0.0f;
        std::vector<PassTimingRecord> PassTimings;
        RendererCounters Counters;
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

    /// The per-pass GPU timings of the most recently resolved frame.
    [[nodiscard]] std::vector<PassTimingRecord> SnapshotPassTimings();

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
                                            const std::vector<CameraCaptureSet>& cameraSets,
                                            const RunInfo& runInfo,
                                            std::string& outError);
} // namespace OloEngine::Benchmark
