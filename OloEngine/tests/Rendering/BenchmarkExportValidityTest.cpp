// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// BenchmarkExportValidityTest — issue #1337 criterion 4's negative control, on
// the PERSISTED artefact.
//
// `result.json` outlives the session that wrote it. Everything else in this
// change makes a bad timing visible to a reader who is present; this file is
// about the reader who is not. A `"gpuMs": 0` in a file from three months ago
// cannot be questioned afterwards — the process that could have answered is
// gone — so the file itself has to carry the answer.
//
// `WriteResultDirectory` takes its RunInfo as a parameter, so the starved cases
// are reachable here with no GPU, no renderer and no capture: build a RunInfo
// that says a timing was never measured, write it, and read the file back.
// =============================================================================

#include "TestTempDir.h"

#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OloEngine;        // NOLINT(google-build-using-namespace)
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)
namespace fs = std::filesystem;

namespace
{
    // The smallest manifest WriteResultDirectory will accept: it echoes the
    // manifest file verbatim and reads a handful of declared fields, and this
    // test is about what goes in BESIDE that, not about the manifest schema.
    Benchmark::BenchmarkManifest MinimalManifest()
    {
        Benchmark::BenchmarkManifest manifest;
        manifest.ManifestVersion = 1;
        manifest.Id = "timing-validity-probe";
        manifest.ScenePath = "Scenes/Benchmark/MaterialLab.olo";
        manifest.Width = 1920;
        manifest.Height = 1080;
        manifest.RenderScale = 1.0f;
        manifest.WarmupFrames = 1;
        return manifest;
    }

    struct WrittenResult
    {
        nlohmann::json Json;
        fs::path Dir;
    };

    // Write a result directory from `runInfo` and read its result.json back.
    WrittenResult WriteAndRead(const Benchmark::RunInfo& runInfo, std::string_view label)
    {
        const fs::path dir = TempDir(label);
        const fs::path manifestPath = dir / "source-manifest.yaml";
        {
            std::ofstream stub(manifestPath, std::ios::binary | std::ios::trunc);
            stub << "Id: timing-validity-probe\n";
        }

        const fs::path outDir = dir / "result";
        std::string error;
        const bool ok =
            Benchmark::WriteResultDirectory(MinimalManifest(), manifestPath, outDir, {}, runInfo, error);
        EXPECT_TRUE(ok) << error;

        std::ifstream file(outDir / "result.json", std::ios::binary);
        EXPECT_TRUE(file.is_open()) << "result.json was not written to " << outDir.string();
        return WrittenResult{ nlohmann::json::parse(file, nullptr, false), outDir };
    }

    Benchmark::RunInfo HealthyRunInfo()
    {
        Benchmark::RunInfo runInfo;
        runInfo.Backend = "opengl";
        runInfo.CommitSha = "unknown";
        runInfo.MachineTag = "test";
        runInfo.Host = "test-binary";
        runInfo.TotalFramesRendered = 8;
        runInfo.PassTimings = { Benchmark::PassTimingRecord{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} } };
        runInfo.Timing.MeasurementFrameId = 100;
        runInfo.Timing.CurrentFrameId = 102;
        runInfo.Timing.AgeFrames = 2;
        runInfo.Timing.FrameStatus = GpuTimingStatus::Valid;
        runInfo.Resolution = Benchmark::ResolutionRecord{ 1280, 720, 1920, 1080, 0.6667f, true };
        runInfo.Counters = Benchmark::RendererCounters{ 42, 5000, 0, 1024 * 1024 };
        return runInfo;
    }
} // namespace

// =============================================================================
// The negative control.
// =============================================================================

TEST(BenchmarkExportValidity, AStarvedPassTimingIsWrittenAsNullWithAReason)
{
    // THE TEST CRITERION 4 ASKS FOR. A pass whose timestamps never resolved is
    // persisted as `null` with a status naming the cause — not as 0.0, which a
    // later reader would take for a pass that cost nothing.
    Benchmark::RunInfo runInfo = HealthyRunInfo();
    runInfo.PassTimings = {
        Benchmark::PassTimingRecord{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        Benchmark::PassTimingRecord{ "ShadowPass", GpuTimingSample::Absent(GpuTimingStatus::Dropped), false, {} },
        Benchmark::PassTimingRecord{ "GTAOPass", GpuTimingSample::Absent(GpuTimingStatus::NotStamped), false, {} },
    };

    const auto result = WriteAndRead(runInfo, "starved");
    ASSERT_FALSE(result.Json.is_discarded()) << "result.json did not parse";

    const auto& timings = result.Json["passTimingsMs"];
    ASSERT_EQ(timings.size(), 3u);

    EXPECT_DOUBLE_EQ(timings[0]["gpuMs"].get<f64>(), 2.0);
    EXPECT_EQ(timings[0]["status"].get<std::string>(), "valid");

    EXPECT_TRUE(timings[1]["gpuMs"].is_null())
        << "a dropped readback was persisted as " << timings[1]["gpuMs"].dump()
        << "; a number here is indistinguishable from a free pass to anyone reading this file later";
    EXPECT_EQ(timings[1]["status"].get<std::string>(), "dropped");

    EXPECT_TRUE(timings[2]["gpuMs"].is_null());
    EXPECT_EQ(timings[2]["status"].get<std::string>(), "notStamped");
}

TEST(BenchmarkExportValidity, StaleTimingsAreDeclaredStaleInTheFile)
{
    // The other half of "stale data": the numbers ARE measurements, and they
    // describe a frame the ring has since overwritten. Keeping them is right;
    // shipping them without saying how old they are is not.
    Benchmark::RunInfo runInfo = HealthyRunInfo();
    runInfo.Timing.MeasurementFrameId = 100;
    runInfo.Timing.CurrentFrameId = 140;
    runInfo.Timing.AgeFrames = 40;
    runInfo.Timing.DroppedSlots = 9;
    runInfo.Timing.UnstampedFrames = 4;
    runInfo.Timing.Stale = true;

    const auto result = WriteAndRead(runInfo, "stale");
    ASSERT_FALSE(result.Json.is_discarded());

    const auto& validity = result.Json["timingValidity"];
    EXPECT_TRUE(validity["stale"].get<bool>());
    EXPECT_EQ(validity["ageFrames"].get<u64>(), 40u);
    EXPECT_EQ(validity["droppedSlots"].get<u32>(), 9u)
        << "nine frames were never measured at all; a reader comparing two runs has to be able to see that";
    // A separate fault from a ring wrap, so a separate number: a backend that
    // declines timestamps must not read as a GPU backlog.
    EXPECT_EQ(validity["unstampedFrames"].get<u32>(), 4u);
    EXPECT_EQ(validity["measurementFrameId"].get<u64>(), 100u);
    EXPECT_EQ(validity["currentFrameId"].get<u64>(), 140u);
}

// =============================================================================
// The rest of criterion 4: frame IDs, units, actual dimensions.
// =============================================================================

TEST(BenchmarkExportValidity, TheExportCarriesUnitsAndMeasurementFrameIds)
{
    const auto result = WriteAndRead(HealthyRunInfo(), "units");
    ASSERT_FALSE(result.Json.is_discarded());

    EXPECT_EQ(result.Json["resultSchemaVersion"].get<u32>(), 2u)
        << "the schema changed shape; a reader must be able to tell a v1 zero from a v2 null";

    ASSERT_TRUE(result.Json.contains("units"));
    EXPECT_EQ(result.Json["units"]["gpuMs"].get<std::string>(), "milliseconds");
    EXPECT_EQ(result.Json["units"]["bytes"].get<std::string>(), "bytes");
    EXPECT_EQ(result.Json["units"]["dimensions"].get<std::string>(), "pixels");

    EXPECT_EQ(result.Json["timingValidity"]["measurementFrameId"].get<u64>(), 100u);
    EXPECT_EQ(result.Json["timingValidity"]["frameStatus"].get<std::string>(), "valid");
}

TEST(BenchmarkExportValidity, ActualRenderAndDisplayDimensionsAreRecordedSeparately)
{
    // Criterion 4's "actual render/display dimensions". The manifest declares
    // 1920x1080; the graph rendered 1280x720 and presented at 1920x1080. A
    // record carrying only the declared numbers cannot be compared against one
    // taken at a different scale, because both would claim 1920x1080.
    const auto result = WriteAndRead(HealthyRunInfo(), "dims");
    ASSERT_FALSE(result.Json.is_discarded());

    const auto& output = result.Json["output"];
    EXPECT_EQ(output["requested"]["width"].get<u32>(), 1920u);
    EXPECT_EQ(output["requested"]["height"].get<u32>(), 1080u);

    ASSERT_FALSE(output["actual"].is_null());
    EXPECT_EQ(output["actual"]["renderWidth"].get<u32>(), 1280u);
    EXPECT_EQ(output["actual"]["renderHeight"].get<u32>(), 720u);
    EXPECT_EQ(output["actual"]["displayWidth"].get<u32>(), 1920u);
    EXPECT_EQ(output["actual"]["displayHeight"].get<u32>(), 1080u);
    EXPECT_NE(output["actual"]["renderWidth"].get<u32>(), output["requested"]["width"].get<u32>())
        << "this fixture exists precisely because the two can differ";
}

TEST(BenchmarkExportValidity, UnmeasuredDimensionsAreNullRatherThanAnEchoOfTheRequest)
{
    // NEGATIVE CONTROL for the dimensions. With no live graph to ask, echoing
    // the manifest's declared numbers into `actual` would assert a measurement
    // that was never taken — the same defect as the zero timing, in pixels.
    Benchmark::RunInfo runInfo = HealthyRunInfo();
    runInfo.Resolution = Benchmark::ResolutionRecord{}; // Measured == false

    const auto result = WriteAndRead(runInfo, "nodims");
    ASSERT_FALSE(result.Json.is_discarded());

    EXPECT_TRUE(result.Json["output"]["actual"].is_null());
    // The request is still recorded — it is a fact about the manifest.
    EXPECT_EQ(result.Json["output"]["requested"]["width"].get<u32>(), 1920u);
}

TEST(BenchmarkExportValidity, SubPassEntriesDeclareTheirParentSoATotalCanSkipThem)
{
    // Criterion 2 reaching the export: a consumer summing passTimingsMs must
    // be able to leave the nested intervals out, and it must not have to parse
    // the name to find out which they are.
    Benchmark::RunInfo runInfo = HealthyRunInfo();
    runInfo.PassTimings = {
        Benchmark::PassTimingRecord{ "ScenePass", GpuTimingSample::Measured(2.0), false, {} },
        Benchmark::PassTimingRecord{ "ScenePass/DepthPrepass", GpuTimingSample::Measured(0.5), true, "ScenePass" },
    };

    const auto result = WriteAndRead(runInfo, "subpass");
    ASSERT_FALSE(result.Json.is_discarded());

    const auto& timings = result.Json["passTimingsMs"];
    ASSERT_EQ(timings.size(), 2u);
    EXPECT_FALSE(timings[0]["isSubPass"].get<bool>());
    EXPECT_TRUE(timings[1]["isSubPass"].get<bool>());
    EXPECT_EQ(timings[1]["parent"].get<std::string>(), "ScenePass");
}
