#pragma once

// =============================================================================
// BenchmarkManifest — the versioned capture-manifest schema for the renderer
// benchmark scenes (issue #974).
//
// A manifest describes ONE deterministic capture product: which scene, which
// camera pose(s), what resolution, which renderer-side settings the scene
// cannot serialize itself, the determinism levers (seed / mock-clock start /
// fixed dt), how many warm-up frames each temporal history needs, and which
// render-graph attachments to export. The capture entry points (the test
// binary's `--olo-capture-manifest=` tool mode, and the editor MCP tool) both
// parse through here, so the two front doors cannot drift on schema.
//
// Parsing is deliberately strict: an unknown top-level key, an unknown
// attachment format, or a warm-up count smaller than the per-feature map's
// maximum is a hard parse error, never a shrug — the manifests are the
// determinism contract, and a silently-ignored typo would reproduce exactly
// the "why did nothing happen?" failure the TestOptions flags were built to
// kill. See docs/guides/renderer-benchmarks.md for the schema reference.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace OloEngine::Benchmark
{
    enum class ManifestProduct : u8
    {
        Golden = 0, // modest resolution, CI-stable, thresholded
        Diagnostic, // AOV-heavy inspection captures
        Hero        // full resolution, presentation quality
    };

    enum class ExposureMode : u8
    {
        Manual = 0,
        Auto // the auto-exposure convergence capture
    };

    enum class AttachmentFormat : u8
    {
        Png = 0, // LDR 8-bit (float sources clamped to [0,1] unless normalized)
        Hdr      // Radiance .hdr — full float, no clamp
    };

    enum class AttachmentNormalize : u8
    {
        Auto = 0, // normalize depth-like sources, pass colour through
        None,
        On
    };

    // CPU-side derivation applied to the raw texel readback before encoding —
    // how the capture provides AOVs that live as LANES of existing targets
    // (roughness in GBufferNormal.z, metallic in GBufferAlbedo.a) or need a
    // transform (hardware depth -> linear view-space metres) without adding
    // render-graph passes or touching the shipping frame.
    enum class AttachmentDerive : u8
    {
        None = 0,
        LinearDepth, // hardware depth -> view-space metres (camera near/far)
        ChannelR,    // extract one source channel as a grayscale image
        ChannelG,
        ChannelB,
        ChannelA
    };

    struct ManifestCamera
    {
        std::string Id;
        glm::vec3 Position{ 0.0f };
        f32 YawDegrees = 0.0f;
        f32 PitchDegrees = 0.0f;
        f32 FovDegrees = 60.0f;
        f32 NearClip = 0.05f;
        f32 FarClip = 1000.0f;
        // Frames rendered before THIS camera's capture. The first camera
        // defaults to Warmup.Frames; a later camera is a deterministic camera
        // CUT, and defaults to the same full warm-up unless it declares fewer.
        std::optional<u32> WarmupFrames;
    };

    struct ManifestAttachment
    {
        std::string Name;   // file stem in the result directory
        std::string Source; // render-graph resource name (ResourceNames::*)
        AttachmentFormat Format = AttachmentFormat::Png;
        AttachmentNormalize Normalize = AttachmentNormalize::Auto;
        AttachmentDerive Derive = AttachmentDerive::None;
    };

    struct ManifestAssetRecord
    {
        std::string Path;
        std::string Origin;
        std::string License;
    };

    // The subset of renderer-side (non-scene-serialized) state a benchmark
    // scene needs pinned. Optional fields are left untouched when absent, so
    // a manifest states exactly what it depends on and nothing more.
    struct ManifestRendererSettings
    {
        std::optional<RenderingPath> Path;
        std::optional<bool> EnableDDGI;
        std::optional<bool> DepthPrepassEnabled;
        std::optional<bool> OcclusionCullingEnabled;
        std::optional<bool> HZBOcclusionCullingEnabled;
        // Stored in PostProcessSettings, but listed here because the block's
        // contract is "renderer-side state the scene cannot serialize", and
        // TAA is exactly that (the PostProcessSettings scene deserializer does
        // not carry TAAEnabled) — the temporal-history axis needs it pinned.
        std::optional<bool> TAAEnabled;
    };

    struct BenchmarkManifest
    {
        u32 ManifestVersion = 0;
        std::string Id; // [a-z0-9-], becomes the result dir name
        ManifestProduct Product = ManifestProduct::Golden;
        std::string ScenePath; // project-relative, e.g. Scenes/Benchmark/MaterialLab.olo

        std::vector<std::string> SupportedBackends;                             // "opengl", "vulkan"
        std::map<std::string, std::vector<std::string>> UnsupportedAttachments; // backend -> attachment Names

        std::vector<ManifestCamera> Cameras;

        u32 Width = 1280;
        u32 Height = 720;
        f32 RenderScale = 1.0f;

        ManifestRendererSettings RendererSettings;

        ExposureMode Exposure = ExposureMode::Manual;
        f32 ExposureValue = 1.0f;

        u64 Seed = 0;
        f32 StartTimeSeconds = 0.0f;
        f32 FixedDtSeconds = 1.0f / 60.0f;

        u32 WarmupFrames = 0;
        std::map<std::string, u32> WarmupPerFeature;

        std::vector<ManifestAttachment> Attachments;

        // Documented run-twice tolerance (RMSE in 0..255 units; 0 = byte-identical).
        f32 RepeatRmseTolerance = 0.0f;

        std::vector<ManifestAssetRecord> Assets;

        // The manifest file's own bytes hashed (FNV-1a 64) — recorded into
        // result.json so a result directory names the exact manifest revision
        // that produced it.
        u64 SourceHash = 0;

        [[nodiscard]] bool SupportsBackend(std::string_view backend) const;

        /// Attachment names declared unavailable for `backend` — these are
        /// skipped with a per-attachment record in result.json rather than
        /// failing the capture (declared, never silent).
        [[nodiscard]] const std::vector<std::string>* UnsupportedFor(std::string_view backend) const;
    };

    /// Parse + validate a manifest file. Returns nullopt and fills `outError`
    /// (all problems joined, one per line) on any failure — including unknown
    /// keys, which are deliberately fatal.
    [[nodiscard]] std::optional<BenchmarkManifest> LoadBenchmarkManifest(const std::filesystem::path& path,
                                                                         std::string& outError);
} // namespace OloEngine::Benchmark
