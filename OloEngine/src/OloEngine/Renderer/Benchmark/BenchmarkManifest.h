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
//
// ManifestVersion 2 (issue #1239) adds the two things the reference-fixture
// work needed and nothing else: full ASSET PROVENANCE (redistribution class,
// upstream version, SHA-256, units, up axis, colour space, acquisition path,
// and how the licence was verified) and per-frame CAMERA MOTION for moving
// sequences. Both are rejected in a v1 manifest and required in a v2 one, so
// the version number tells you exactly which contract a file signed up to.
// See docs/guides/benchmark-reference-fixtures.md.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

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

    // Per-frame camera movement for a MOVING sequence (issue #1239). A static
    // pose produces a zero velocity buffer, so temporal reprojection, motion
    // blur and the TAA/SSGI/SSR history rejection paths are never exercised by
    // a still capture — the "moving sequences" fixture axis needs the camera to
    // actually advance BETWEEN frames, not just sit at a different pose.
    //
    // Motion is expressed per SECOND and integrated against the manifest's
    // FixedDtSeconds, so it stays a property of the shot rather than of the
    // frame rate: halving the fixed dt doubles the frame count and leaves the
    // captured pose identical. See CameraPoseAtFrame for the exact schedule.
    struct ManifestCameraMotion
    {
        glm::vec3 VelocityPerSecond{ 0.0f }; // world metres / second
        f32 YawRateDegreesPerSecond = 0.0f;
        f32 PitchRateDegreesPerSecond = 0.0f;
    };

    struct ManifestCamera
    {
        FString Id;
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
        // ManifestVersion 2+ only. Absent = the still pose above, unchanged.
        std::optional<ManifestCameraMotion> Motion;
    };

    // The camera state a host must apply before rendering one warm-up frame.
    struct ManifestCameraPose
    {
        glm::vec3 Position{ 0.0f };
        f32 YawDegrees = 0.0f;
        f32 PitchDegrees = 0.0f;
    };

    struct ManifestEntityMotion
    {
        FString Tag;
        glm::vec3 Origin{ 0.0f };
        glm::vec3 Amplitude{ 0.0f };
        f32 FrequencyHz = 0.0f;
        f32 PhaseRadians = 0.0f;
    };

    /// The pose for 0-based warm-up frame `frameInCamera` of `camera`.
    ///
    /// ONE implementation for both capture front doors — the test binary steps
    /// the mock clock itself and the editor host counts live frames, but a
    /// moving shot has to trace the same path through both or the two hosts
    /// would silently disagree about where the camera was. The captured frame
    /// is the LAST warm-up frame, so its pose is the one at
    /// `frameInCamera == warmupFrames - 1`, not at `warmupFrames`.
    [[nodiscard]] ManifestCameraPose CameraPoseAtFrame(const ManifestCamera& camera, u32 frameInCamera,
                                                       f32 fixedDtSeconds);

    struct ManifestAttachment
    {
        FString Name;   // file stem in the result directory
        FString Source; // render-graph resource name (ResourceNames::*)
        AttachmentFormat Format = AttachmentFormat::Png;
        AttachmentNormalize Normalize = AttachmentNormalize::Auto;
        AttachmentDerive Derive = AttachmentDerive::None;
    };

    // Whether the asset's rights let its BYTES live in this repository — the
    // field that decides what a checkout actually contains, which is why it is
    // an enum rather than prose (issue #1239).
    enum class AssetRedistribution : u8
    {
        Committed = 0, // licence permits redistribution; the bytes are in-tree
        FetchRequired, // freely obtainable but NOT redistributed here; the
                       // acquisition path fetches it into place
        LocalOnly      // rights forbid redistribution entirely; the user must
                       // supply their own copy, and the fixture says so rather
                       // than rendering a substitute and calling it the same
    };

    // How the licence claim was established. An asset whose rights nobody
    // checked is recorded as Unverified rather than asserted — the schema's
    // whole point is that "we did not verify this" is a statable, greppable
    // answer instead of an optimistic string.
    enum class LicenseVerification : u8
    {
        InRepoFile = 0,   // a LICENSE file sits beside the asset in this repo
        UpstreamDeclared, // the upstream project declares it; no in-repo copy
        Unverified        // origin unknown or licence not established
    };

    // Scene units the asset's geometry is authored in. Mixing these silently is
    // how a "head" arrives 100x too large; the fixture scenes state the
    // convention so a scale factor is a documented decision.
    enum class AssetUnits : u8
    {
        Metres = 0,
        Centimetres,
        Unitless // procedural / non-metric source (e.g. a texture)
    };

    enum class AssetUpAxis : u8
    {
        YUp = 0,
        ZUp,
        NotApplicable // textures and other non-geometry assets
    };

    // Colour convention of the asset's base-colour / albedo data. Getting this
    // wrong is a gamma bug that looks like a lighting bug.
    enum class AssetColorSpace : u8
    {
        Srgb = 0,
        Linear,
        NotApplicable
    };

    struct ManifestAssetRecord
    {
        FString Path;
        FString Origin;
        FString License;

        // ---- ManifestVersion 2 provenance (issue #1239) -------------------
        // Required from v2; absent (nullopt / empty) in a v1 manifest, which
        // keeps every issue-#974 manifest parsing byte-for-byte as before.
        std::optional<AssetRedistribution> Redistribution;
        std::optional<LicenseVerification> LicenseVerified;
        std::optional<AssetUnits> Units;
        std::optional<AssetUpAxis> UpAxis;
        std::optional<AssetColorSpace> ColorSpace;
        // Upstream release tag / commit, or "generated" for an asset this repo
        // produces from a committed script.
        FString Version;
        // SHA-256 of the file AS ACQUIRED, 64 lowercase hex — what makes the
        // acquisition path checkable instead of merely described. The parser
        // validates the FORMAT; tools/benchmark/reference_assets.py verifies
        // the bytes.
        FString Sha256;
        // URL, or the documented local procedure for a LocalOnly asset. This is
        // the "reproducible local acquisition path" acceptance criterion.
        FString Acquisition;
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
        std::optional<bool> RayTracedShadowsEnabled;
        std::optional<u32> MSAASampleCount;
        std::optional<UpscaleMode> Upscale;
        std::optional<UpscalerTechnique> UpscaleTechnique;
        // The GPU reference path tracer (#1055), stored in PostProcessSettings
        // like TAA but pinned here because a capture of its AOVs is only
        // meaningful with the tracer on and a stated sample budget: the warm-up
        // frames ARE the accumulation, so the per-frame sample count decides
        // how converged the captured planes are.
        std::optional<bool> GpuPathTracerEnabled;
        std::optional<u32> GpuPathTracerSamplesPerFrame;
    };

} // namespace OloEngine::Benchmark

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<Benchmark::ManifestEntityMotion>
    {
        using Record = Benchmark::ManifestEntityMotion;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Tag)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Origin)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Amplitude)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FrequencyHz)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::PhaseRadians)>;
    };
    template<>
    struct TIsTriviallyRelocatable<Benchmark::ManifestCamera>
    {
        using Record = Benchmark::ManifestCamera;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Id)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Position)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::YawDegrees)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::PitchDegrees)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FovDegrees)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::NearClip)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::FarClip)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::WarmupFrames)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Motion)>;
    };
    template<>
    struct TIsTriviallyRelocatable<Benchmark::ManifestAttachment>
    {
        using Record = Benchmark::ManifestAttachment;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Source)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Format)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Normalize)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Derive)>;
    };
    template<>
    struct TIsTriviallyRelocatable<Benchmark::ManifestAssetRecord>
    {
        using Record = Benchmark::ManifestAssetRecord;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Path)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Origin)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::License)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Redistribution)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::LicenseVerified)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Units)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::UpAxis)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::ColorSpace)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Version)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Sha256)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Acquisition)>;
    };
} // namespace OloEngine

namespace OloEngine::Benchmark
{
    struct BenchmarkManifest
    {
        u32 ManifestVersion = 0;
        FString Id; // [a-z0-9-], becomes the result dir name
        ManifestProduct Product = ManifestProduct::Golden;
        FString ScenePath; // project-relative, e.g. Scenes/Benchmark/MaterialLab.olo

        TArray<FString> SupportedBackends; // "opengl", "vulkan"
        std::map<std::string, FString> PresetIdsByBackend;
        std::map<std::string, TArray<FString>> UnsupportedAttachments; // backend -> attachment Names

        TArray<ManifestCamera> Cameras;
        TArray<ManifestEntityMotion> EntityMotions;

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

        // Optional measured frames after each camera's warm-up. The capture is
        // taken after these frames, from the last measured pose. A zero count
        // preserves the original capture-only behavior.
        u32 MeasurementFrames = 0;
        f32 MeasurementDeadlineMs = 0.0f;

        TArray<ManifestAttachment> Attachments;

        // Documented run-twice tolerance (RMSE in 0..255 units; 0 = byte-identical).
        f32 RepeatRmseTolerance = 0.0f;

        TArray<ManifestAssetRecord> Assets;

        // The manifest file's own bytes hashed (FNV-1a 64) — recorded into
        // result.json so a result directory names the exact manifest revision
        // that produced it.
        u64 SourceHash = 0;

        [[nodiscard]] bool SupportsBackend(std::string_view backend) const;

        /// Attachment names declared unavailable for `backend` — these are
        /// skipped with a per-attachment record in result.json rather than
        /// failing the capture (declared, never silent).
        [[nodiscard]] const TArray<FString>* UnsupportedFor(std::string_view backend) const;
    };

    /// Parse + validate a manifest file. Returns nullopt and fills `outError`
    /// (all problems joined, one per line) on any failure — including unknown
    /// keys, which are deliberately fatal.
    [[nodiscard]] std::optional<BenchmarkManifest> LoadBenchmarkManifest(const std::filesystem::path& path,
                                                                         std::string& outError);
} // namespace OloEngine::Benchmark
