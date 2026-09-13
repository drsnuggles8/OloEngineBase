// =============================================================================
// ReferenceFixtureManifestTest.cpp
//
// CPU contract tests for the ManifestVersion 2 benchmark-manifest schema and
// the five reference fixtures it carries (issue #1239).
//
// WHAT THESE PIN, AND WHY IT MATTERS MORE THAN USUAL
//
// The fixtures are a MEASURING INSTRUMENT: every later quality issue in epics
// #1222/#1223/#1224/#1225 is judged against numbers taken from them. An
// instrument that silently accepts a malformed provenance record, or that
// renders a "moving" sequence as a still frame, produces numbers that look
// fine and mean nothing. So the strict-rejection cases below are the point of
// the file, not padding around the happy path — each one is a way the
// instrument could lie quietly.
//
// These are pure parse/maths tests: no GPU, no scene load, no capture. The
// capture itself is the `--olo-capture-manifest=` tool mode
// (BenchmarkCaptureTest), which is not a suite-run case.
//
// OLO_TEST_LAYER: plumbing
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "TestTempDir.h"

#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace OloEngine;
using namespace OloEngine::Benchmark;

namespace
{
    // The tests run from the repo root (the test binary's documented CWD), but
    // a capture run switches the process CWD to OloEditor/. Resolve both so a
    // developer running this case under either condition gets the same answer
    // instead of a confusing "manifest directory not found".
    fs::path ManifestDir()
    {
        for (const auto& candidate : { fs::path("OloEditor/assets/benchmark/manifests"),
                                       fs::path("assets/benchmark/manifests"),
                                       fs::path("../OloEditor/assets/benchmark/manifests") })
        {
            if (fs::is_directory(candidate))
            {
                return candidate;
            }
        }
        return {};
    }

    std::vector<fs::path> ManifestFiles()
    {
        std::vector<fs::path> out;
        const auto dir = ManifestDir();
        if (dir.empty())
        {
            return out;
        }
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".yaml")
            {
                out.push_back(entry.path());
            }
        }
        std::ranges::sort(out);
        return out;
    }

    // The five fixtures issue #1239 delivers, by manifest Id.
    const std::vector<std::string> kFixtureIds = { "reference-head", "animal-short-coat", "animal-long-coat",
                                                   "meadow", "woodland" };

    // A minimal but VALID v2 manifest, used as the base for the rejection
    // cases: each one mutates exactly one field, so a failure names the field
    // rather than "the manifest is broken somewhere".
    std::string ValidV2Manifest()
    {
        return R"(ManifestVersion: 2
Id: unit-test-fixture
Product: diagnostic
Scene: Scenes/Benchmark/ReferenceHead.olo
Backends:
  Supported: [opengl]
Camera:
  Id: frontal
  Position: [0.0, 1.0, 3.0]
  YawDegrees: 0.0
  PitchDegrees: 0.0
  FovDegrees: 45.0
  Near: 0.05
  Far: 100.0
Output:
  Resolution: [640, 360]
  RenderScale: 1.0
Exposure:
  Mode: Manual
  Exposure: 1.0
Determinism:
  Seed: 1239
  StartTimeSeconds: 0.0
  FixedDtSeconds: 0.016666666
Warmup:
  Frames: 8
Attachments:
  - Name: Beauty
    Source: UIComposite
    Format: png
Tolerance:
  RepeatRmse: 0.0
Assets:
  - Path: assets/models/Suzanne/glTF/Suzanne.gltf
    Origin: KhronosGroup glTF-Sample-Assets
    License: CC0-1.0
    Redistribution: committed
    LicenseVerified: in-repo-file
    Units: metres
    UpAxis: "+Y"
    ColorSpace: srgb
    Version: test
    Sha256: 34d5fbc23fd081101f26d8b0df1394533ffd9c9b53d8fd550b027b17595636ba
    Acquisition: https://example.invalid/suzanne
)";
    }

    fs::path WriteManifest(std::string_view label, const std::string& text)
    {
        const fs::path path = OloEngine::Tests::TempFile(std::string(label) + ".yaml");
        std::ofstream out(path, std::ios::binary);
        out << text;
        out.close();
        return path;
    }

    // Parse `text` and require it to FAIL, returning the joined error text so
    // the caller can assert on which field was named.
    std::string ExpectParseFailure(std::string_view label, const std::string& text)
    {
        std::string error;
        const auto parsed = LoadBenchmarkManifest(WriteManifest(label, text), error);
        EXPECT_FALSE(parsed.has_value()) << "expected a parse failure for '" << label << "' but it was accepted";
        return error;
    }

    std::string Replaced(std::string text, std::string_view from, std::string_view to)
    {
        const auto at = text.find(from);
        EXPECT_NE(at, std::string::npos) << "test fixture text does not contain '" << from << "'";
        if (at == std::string::npos)
        {
            return text;
        }
        return text.replace(at, from.size(), to);
    }
} // namespace

// -----------------------------------------------------------------------------
// The committed manifests
// -----------------------------------------------------------------------------

// Every committed manifest parses, v1 and v2 alike. This is the regression
// guard on the version gate: widening it to accept 2 must not have narrowed it
// away from 1, which would break every issue-#974 capture at once.
TEST(ReferenceFixtureManifest, EveryCommittedManifestParses)
{
    const auto files = ManifestFiles();
    ASSERT_FALSE(files.empty()) << "no manifests found — looked for OloEditor/assets/benchmark/manifests "
                                   "relative to cwd " << fs::current_path().string();

    u32 v1Count = 0;
    u32 v2Count = 0;
    for (const auto& file : files)
    {
        std::string error;
        const auto parsed = LoadBenchmarkManifest(file, error);
        ASSERT_TRUE(parsed.has_value()) << file.filename().string() << ":\n" << error;
        EXPECT_TRUE(parsed->ManifestVersion == 1u || parsed->ManifestVersion == 2u);
        (parsed->ManifestVersion == 1u ? v1Count : v2Count) += 1u;
    }
    EXPECT_GT(v1Count, 0u) << "the issue-#974 v1 manifests should still be present and parsing";
    EXPECT_GE(v2Count, kFixtureIds.size()) << "expected at least the five issue-#1239 fixture manifests";
}

// The four sequences the issue asks for, per fixture, by name — and the moving
// one must actually move. A fixture that quietly lost its `moving` camera, or
// whose `moving` camera lost its Motion block, would still capture four images
// and still look complete.
TEST(ReferenceFixtureManifest, EveryFixtureDeclaresFourSequences)
{
    std::vector<std::string> found;
    for (const auto& file : ManifestFiles())
    {
        std::string error;
        const auto parsed = LoadBenchmarkManifest(file, error);
        ASSERT_TRUE(parsed.has_value()) << error;
        if (std::ranges::find(kFixtureIds, parsed->Id) == kFixtureIds.end())
        {
            continue;
        }
        found.push_back(parsed->Id);

        for (const char* required : { "frontal", "grazing", "backlit", "moving" })
        {
            const auto camera = std::ranges::find_if(parsed->Cameras, [required](const ManifestCamera& c)
                                                     { return c.Id == required; });
            ASSERT_NE(camera, parsed->Cameras.end())
                << parsed->Id << " is missing the '" << required << "' sequence";
            if (std::string_view(required) == "moving")
            {
                EXPECT_TRUE(camera->Motion.has_value())
                    << parsed->Id << "'s moving camera has no Motion block — it would capture a still frame";
            }
            else
            {
                EXPECT_FALSE(camera->Motion.has_value())
                    << parsed->Id << "'s '" << required << "' camera should be a still shot";
            }
        }
    }
    EXPECT_EQ(found.size(), kFixtureIds.size()) << "not every issue-#1239 fixture manifest was found";
}

// Provenance is only useful if it is COMPLETE — one undocumented asset is
// enough to make a capture unpublishable, so the check is per record.
TEST(ReferenceFixtureManifest, EveryFixtureAssetCarriesFullProvenance)
{
    for (const auto& file : ManifestFiles())
    {
        std::string error;
        const auto parsed = LoadBenchmarkManifest(file, error);
        ASSERT_TRUE(parsed.has_value()) << error;
        if (parsed->ManifestVersion < 2u)
        {
            continue;
        }
        EXPECT_FALSE(parsed->Assets.empty()) << parsed->Id << " declares no assets";
        for (const auto& asset : parsed->Assets)
        {
            SCOPED_TRACE(parsed->Id + " / " + asset.Path);
            EXPECT_TRUE(asset.Redistribution.has_value());
            EXPECT_TRUE(asset.LicenseVerified.has_value());
            EXPECT_TRUE(asset.Units.has_value());
            EXPECT_TRUE(asset.UpAxis.has_value());
            EXPECT_TRUE(asset.ColorSpace.has_value());
            EXPECT_FALSE(asset.Origin.empty());
            EXPECT_FALSE(asset.License.empty());
            EXPECT_FALSE(asset.Version.empty());
            EXPECT_FALSE(asset.Acquisition.empty());
            EXPECT_EQ(asset.Sha256.size(), 64u);
        }
    }
}

// -----------------------------------------------------------------------------
// Strict rejection — one case per way the instrument could lie quietly
// -----------------------------------------------------------------------------

TEST(ReferenceFixtureManifest, RejectsMissingProvenanceField)
{
    const auto error = ExpectParseFailure(
        "missing-redistribution", Replaced(ValidV2Manifest(), "    Redistribution: committed\n", ""));
    EXPECT_NE(error.find("Redistribution"), std::string::npos) << error;
    EXPECT_NE(error.find("ManifestVersion 2"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsUnknownRedistributionClass)
{
    // A typo here is the most consequential one in the schema: it decides
    // whether bytes may live in a public repository.
    const auto error = ExpectParseFailure(
        "typo-redistribution",
        Replaced(ValidV2Manifest(), "Redistribution: committed", "Redistribution: comitted"));
    EXPECT_NE(error.find("Redistribution"), std::string::npos) << error;
    EXPECT_NE(error.find("comitted"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsMalformedSha256)
{
    for (const auto* bad : { "Sha256: deadbeef", "Sha256: 34D5FBC23FD081101F26D8B0DF1394533FFD9C9B53D8FD550B027B17595636BA" })
    {
        const auto error = ExpectParseFailure(
            "bad-sha", Replaced(ValidV2Manifest(),
                                "Sha256: 34d5fbc23fd081101f26d8b0df1394533ffd9c9b53d8fd550b027b17595636ba", bad));
        EXPECT_NE(error.find("Sha256"), std::string::npos) << error;
    }
}

TEST(ReferenceFixtureManifest, RejectsInRepoLicenceClaimForUnshippedAsset)
{
    // "verified against the in-repo licence file" is meaningless for an asset
    // this repo does not ship — the cross-field rule catches a copy-paste that
    // would otherwise read as a stronger claim than was made.
    const auto error = ExpectParseFailure(
        "inrepo-but-local-only",
        Replaced(ValidV2Manifest(), "Redistribution: committed", "Redistribution: local-only"));
    EXPECT_NE(error.find("in-repo-file"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsUnknownProvenanceKey)
{
    const auto error = ExpectParseFailure(
        "unknown-key", Replaced(ValidV2Manifest(), "    Version: test\n", "    Version: test\n    Licence: CC0\n"));
    EXPECT_NE(error.find("Licence"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsEmptyAssetsInV2)
{
    const auto text = Replaced(ValidV2Manifest(), "Assets:", "NotAssets:");
    std::string error;
    const auto parsed = LoadBenchmarkManifest(WriteManifest("no-assets", text), error);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_NE(error.find("Assets"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsCameraMotionInV1)
{
    // A v1 manifest that grew a Motion block would be silently ignored by an
    // older parser and honoured by a newer one — the exact drift the version
    // number exists to prevent.
    auto text = Replaced(ValidV2Manifest(), "ManifestVersion: 2", "ManifestVersion: 1");
    text = Replaced(text, "  Far: 100.0\n",
                    "  Far: 100.0\n  Motion:\n    VelocityPerSecond: [1.0, 0.0, 0.0]\n");
    const auto error = ExpectParseFailure("motion-in-v1", text);
    EXPECT_NE(error.find("Motion"), std::string::npos) << error;
    EXPECT_NE(error.find("ManifestVersion 2"), std::string::npos) << error;
}

TEST(ReferenceFixtureManifest, RejectsAllZeroMotion)
{
    // An all-zero Motion block advertises a moving sequence and produces a
    // still one.
    const auto text = Replaced(ValidV2Manifest(), "  Far: 100.0\n",
                               "  Far: 100.0\n  Motion:\n    VelocityPerSecond: [0.0, 0.0, 0.0]\n"
                               "    YawRateDegreesPerSecond: 0.0\n");
    const auto error = ExpectParseFailure("zero-motion", text);
    EXPECT_NE(error.find("zero"), std::string::npos) << error;
}

// A NaN or otherwise unusable velocity must be REJECTED, never quietly read as
// zero. Both outcomes are legitimate depending on whether yaml-cpp decodes
// `.nan` on this platform — what must not happen is acceptance, so the
// assertion is on the rejection and on the field being named, not on which of
// the two messages came back. (Asserting one exact message is how the first
// version of this test passed while the parser was silently zeroing the value.)
TEST(ReferenceFixtureManifest, RejectsNonFiniteMotion)
{
    for (const auto* bad : { "[.nan, 0.0, 0.0]", "[not-a-number, 0.0, 0.0]", "[1e400, 0.0, 0.0]" })
    {
        const auto text = Replaced(ValidV2Manifest(), "  Far: 100.0\n",
                                   std::string("  Far: 100.0\n  Motion:\n    VelocityPerSecond: ") + bad + "\n");
        const auto error = ExpectParseFailure("nan-motion", text);
        SCOPED_TRACE(bad);
        EXPECT_NE(error.find("VelocityPerSecond"), std::string::npos) << error;
        EXPECT_EQ(error.find("all motion rates are zero"), std::string::npos)
            << "a malformed velocity was silently read as zero:\n" << error;
    }
}

TEST(ReferenceFixtureManifest, V1ManifestRejectsProvenanceRequirement)
{
    // The v1 contract is unchanged: a v1 manifest with only Path/Origin/License
    // still parses. If this ever fails, every issue-#974 manifest is broken.
    std::string text = Replaced(ValidV2Manifest(), "ManifestVersion: 2", "ManifestVersion: 1");
    for (const auto* line : { "    Redistribution: committed\n", "    LicenseVerified: in-repo-file\n",
                              "    Units: metres\n", "    UpAxis: \"+Y\"\n", "    ColorSpace: srgb\n",
                              "    Version: test\n",
                              "    Sha256: 34d5fbc23fd081101f26d8b0df1394533ffd9c9b53d8fd550b027b17595636ba\n",
                              "    Acquisition: https://example.invalid/suzanne\n" })
    {
        text = Replaced(text, line, "");
    }
    std::string error;
    const auto parsed = LoadBenchmarkManifest(WriteManifest("plain-v1", text), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->ManifestVersion, 1u);
    ASSERT_EQ(parsed->Assets.size(), 1u);
    EXPECT_FALSE(parsed->Assets[0].Redistribution.has_value());
}

// -----------------------------------------------------------------------------
// The camera-motion schedule
// -----------------------------------------------------------------------------

// The pose schedule is a CLOSED FORM of the frame index, which is what lets the
// test binary and the editor host trace the same path while stepping frames
// completely differently. If this ever became an accumulation, the two hosts
// would drift apart by float error and nobody would notice until two captures
// of the same manifest disagreed.
TEST(ReferenceFixtureManifest, CameraPoseAtFrameIsClosedForm)
{
    ManifestCamera still;
    still.Id = "still";
    still.Position = { 1.0f, 2.0f, 3.0f };
    still.YawDegrees = 30.0f;
    still.PitchDegrees = -5.0f;

    // A still camera returns the declared pose at EVERY frame — this is what
    // makes the per-frame re-pose a no-op for the issue-#974 manifests.
    for (const u32 frame : { 0u, 1u, 63u, 100000u })
    {
        const auto pose = CameraPoseAtFrame(still, frame, 1.0f / 60.0f);
        EXPECT_FLOAT_EQ(pose.Position.x, 1.0f);
        EXPECT_FLOAT_EQ(pose.Position.y, 2.0f);
        EXPECT_FLOAT_EQ(pose.Position.z, 3.0f);
        EXPECT_FLOAT_EQ(pose.YawDegrees, 30.0f);
        EXPECT_FLOAT_EQ(pose.PitchDegrees, -5.0f);
    }

    ManifestCamera moving = still;
    moving.Motion = ManifestCameraMotion{ { 6.0f, 0.0f, -12.0f }, 9.0f, -3.0f };

    // Frame 0 is the declared pose: the shot STARTS where the manifest says.
    const auto first = CameraPoseAtFrame(moving, 0u, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(first.Position.x, 1.0f);
    EXPECT_FLOAT_EQ(first.YawDegrees, 30.0f);

    // After 60 frames at 1/60 s the camera has travelled exactly one second's
    // worth of every rate.
    const auto oneSecond = CameraPoseAtFrame(moving, 60u, 1.0f / 60.0f);
    EXPECT_NEAR(oneSecond.Position.x, 7.0f, 1e-3f);
    EXPECT_NEAR(oneSecond.Position.y, 2.0f, 1e-3f);
    EXPECT_NEAR(oneSecond.Position.z, -9.0f, 1e-3f);
    EXPECT_NEAR(oneSecond.YawDegrees, 39.0f, 1e-3f);
    EXPECT_NEAR(oneSecond.PitchDegrees, -8.0f, 1e-3f);

    // Rates are per SECOND, not per frame: halving the dt and doubling the
    // frame count lands on the same pose, so a manifest's shot does not change
    // meaning when its frame budget does.
    const auto halfDt = CameraPoseAtFrame(moving, 120u, 1.0f / 120.0f);
    EXPECT_NEAR(halfDt.Position.x, oneSecond.Position.x, 1e-3f);
    EXPECT_NEAR(halfDt.Position.z, oneSecond.Position.z, 1e-3f);
    EXPECT_NEAR(halfDt.YawDegrees, oneSecond.YawDegrees, 1e-3f);
}

// The committed fixtures' moving cameras must stay inside their scenes over the
// whole shot. A velocity that flies the camera out of the world would still
// capture — of the sky.
TEST(ReferenceFixtureManifest, FixtureMotionStaysBounded)
{
    for (const auto& file : ManifestFiles())
    {
        std::string error;
        const auto parsed = LoadBenchmarkManifest(file, error);
        ASSERT_TRUE(parsed.has_value()) << error;
        if (parsed->ManifestVersion < 2u)
        {
            continue;
        }
        for (const auto& camera : parsed->Cameras)
        {
            if (!camera.Motion)
            {
                continue;
            }
            const u32 warmFrames = camera.WarmupFrames.value_or(parsed->WarmupFrames);
            const auto start = CameraPoseAtFrame(camera, 0u, parsed->FixedDtSeconds);
            const auto end = CameraPoseAtFrame(camera, warmFrames - 1u, parsed->FixedDtSeconds);
            const f32 travelled = glm::length(end.Position - start.Position);
            SCOPED_TRACE(parsed->Id + " / " + camera.Id);
            // A shot that moves less than a centimetre is a still frame with
            // extra ceremony; one that moves more than the scenes are wide has
            // left the fixture behind. Both are authoring mistakes.
            EXPECT_GT(travelled, 0.01f) << "moving camera barely moves over its warm-up";
            EXPECT_LT(travelled, 96.0f) << "moving camera leaves the fixture's terrain tile";
        }
    }
}
