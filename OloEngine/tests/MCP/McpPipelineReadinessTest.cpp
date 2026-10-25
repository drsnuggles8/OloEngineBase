// OLO_TEST_LAYER: unit
//
// The JSON shape behind olo_perf_pipeline_readiness (#1345). The two backends
// report different objects, and the report must say which: Vulkan's PSO
// counters and readiness for the selected configuration, or OpenGL's
// program-link model with the pipeline block null and the driver's first-draw
// compile named as unmeasurable — never a block of zeros that reads as "no
// misses".

#include "MCP/McpPipelineReadiness.h"

#include <gtest/gtest.h>

using namespace OloEngine;
using OloEngine::MCP::PipelineReadinessReport::ToJson;

TEST(McpPipelineReadiness, VulkanReportsEveryMissKindAndReadinessSeparately)
{
    PipelineReadinessSnapshot snapshot;
    snapshot.Model = PipelineReadinessModel::VulkanPipelineObjects;
    snapshot.PrecacheEnabled = true;
    snapshot.ConfigurationTag = FString("Deferred/msaa1/native/oit1");
    snapshot.Pipelines.PreparedFirstUses = 40;
    snapshot.Pipelines.NeverRequested = 3;
    snapshot.Pipelines.TooLateQueued = 2;
    snapshot.Pipelines.TooLateInFlight = 1;
    snapshot.Pipelines.FailedCreations = 1;
    snapshot.Pipelines.ReadyClaimViolations = 1;
    snapshot.Pipelines.BlockingCreateMs = 81.5;
    snapshot.Pipelines.DriverCacheHits = 30;
    snapshot.Shaders.SourceCompiles = 5;
    snapshot.Shaders.SpirvCacheHits = 120;
    snapshot.TrackedShaders = 3;
    snapshot.ReadyShaders = 2;
    snapshot.UnknownShaders = 0;
    snapshot.PendingShaders.Add({ FString("PBR_GBuffer_Skinned"), 2, 1, 0 });

    const auto json = ToJson(snapshot);
    EXPECT_EQ(json["model"], "vulkan-pipeline-objects");
    const auto& pipelines = json["pipelines"];
    ASSERT_TRUE(pipelines.is_object());
    EXPECT_EQ(pipelines["preparedFirstUses"], 40);
    EXPECT_EQ(pipelines["misses"]["total"], 6);
    EXPECT_EQ(pipelines["misses"]["neverRequested"], 3);
    EXPECT_EQ(pipelines["misses"]["tooLateQueued"], 2);
    EXPECT_EQ(pipelines["misses"]["tooLateInFlight"], 1);
    EXPECT_EQ(pipelines["failedCreations"], 1);
    EXPECT_EQ(pipelines["readyClaimViolations"], 1);
    EXPECT_DOUBLE_EQ(pipelines["blockingCreateMs"].get<f64>(), 81.5);
    EXPECT_EQ(pipelines["driverCache"]["hits"], 30);
    EXPECT_EQ(json["shaders"]["sourceCompiles"], 5) << "Compilation is reported apart from pipeline creation";
    EXPECT_EQ(json["shaders"]["spirvCacheHits"], 120);

    const auto& readiness = json["readiness"];
    EXPECT_EQ(readiness["configuration"], "Deferred/msaa1/native/oit1");
    EXPECT_FALSE(readiness["renderReady"]);
    ASSERT_EQ(readiness["pendingShaders"].size(), 1u);
    EXPECT_EQ(readiness["pendingShaders"][0]["shader"], "PBR_GBuffer_Skinned");
    EXPECT_EQ(readiness["pendingShaders"][0]["prepared"], 1);
    EXPECT_TRUE(json["unmeasurable"].empty());
}

TEST(McpPipelineReadiness, OpenGLSaysItHasNoPipelineObjectsInsteadOfReportingZeros)
{
    PipelineReadinessSnapshot snapshot;
    snapshot.Model = PipelineReadinessModel::OpenGLProgramLink;
    snapshot.Shaders.ProgramLinks = 12;
    snapshot.Shaders.ProgramBinaryHits = 100;
    snapshot.Shaders.LinksForcedAtUse = 2;

    const auto json = ToJson(snapshot);
    EXPECT_EQ(json["model"], "opengl-program-link");
    EXPECT_TRUE(json["pipelines"].is_null());
    EXPECT_TRUE(json["readiness"].is_null());
    EXPECT_FALSE(json["note"].get<std::string>().empty());
    ASSERT_EQ(json["unmeasurable"].size(), 1u);
    EXPECT_EQ(json["unmeasurable"][0], "firstDrawDriverCompile");
    EXPECT_EQ(json["shaders"]["programLinks"], 12);
    EXPECT_EQ(json["shaders"]["programBinaryHits"], 100);
    EXPECT_EQ(json["shaders"]["linksForcedAtUse"], 2) << "The GL form of a late request is counted";
}
