// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "MCP/McpCapturePath.h"
#include "MCP/McpCpuScopeRing.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <system_error>

TEST(McpCpuScopeRingTest, RealSnapshotReportsScopeNestingAndMissingGpuAsNull)
{
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 4);
    ring.BeginFrame(1000000);
    const auto scope = ring.BeginScope("work", 2, 123, 1500000);
    ring.EndScope(scope, 3000000);
    ring.EndFrame(4000000, {});
    ring.Disarm();
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture());
    ASSERT_EQ(json["frames"].size(), 1);
    const auto& frame = json["frames"][0];
    EXPECT_TRUE(frame["gpuMs"].is_null());
    EXPECT_TRUE(frame["sameFrameGpuMs"].is_null());
    EXPECT_EQ(frame["sameFrameGpuStatus"], "unavailable");
    EXPECT_TRUE(frame["rendererCpuMs"].is_null());
    EXPECT_EQ(frame["gpuStatus"], "unavailable");
    EXPECT_DOUBLE_EQ(frame["frameTimeMs"].get<f64>(), 3);
    EXPECT_EQ(frame["scopes"][0]["thread"], 123);
    EXPECT_EQ(frame["scopes"][0]["depth"], 2);
    EXPECT_DOUBLE_EQ(frame["scopes"][0]["durationMs"].get<f64>(), 1.5);
}

TEST(McpCpuScopeRingTest, TruncatedUtf8NamesStillProduceValidJson)
{
    // A three-byte character straddles the recorder's 95-byte name bound.
    const std::string name = std::string(94, 'x') + "\xE2\x82\xAC";
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 2);
    ring.BeginFrame(0);
    const auto token = ring.BeginScope(name, 0, 1, 0);
    ring.EndScope(token, 1000000);
    ring.EndFrame(1000000, {});
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture());
    EXPECT_NO_THROW((void)json.dump());
    EXPECT_EQ(json["frames"][0]["scopes"][0]["name"], std::string(94, 'x'));
    EXPECT_TRUE(json["frames"][0]["scopes"][0]["nameTruncated"]);
}

TEST(McpCpuScopeRingTest, CompleteUtf8CharactersAtTheByteBoundArePreserved)
{
    const std::string prefix = std::string(92, 'x') + "\xE2\x82\xAC";
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 2);
    ring.BeginFrame(0);
    const auto token = ring.BeginScope(prefix + "tail", 0, 1, 0);
    ring.EndScope(token, 1000000);
    ring.EndFrame(1000000, {});
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture());
    EXPECT_NO_THROW((void)json.dump());
    EXPECT_EQ(json["frames"][0]["scopes"][0]["name"], prefix);
    EXPECT_TRUE(json["frames"][0]["scopes"][0]["nameTruncated"]);
}

TEST(McpCpuScopeRingTest, RenderAndGpuIdentitiesRemainSeparateFromTheLoop)
{
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 1);
    ring.BeginFrame(0);
    ring.EndFrame(3000000, { .RendererCpuMs = 1.0,
                             .GpuMs = 0.5,
                             .GpuStatus = OloEngine::GpuTimingStatus::Valid,
                             .GpuFrameId = 7,
                             .GpuIssueFrameId = 10,
                             .RendererFrameId = 15,
                             .RendererFrameAdvanced = true });
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture());
    const auto& frame = json["frames"][0];
    EXPECT_EQ(frame["frameId"], 1);
    EXPECT_EQ(frame["rendererFrameId"], 15);
    EXPECT_EQ(frame["gpuMeasurementFrameId"], 7);
    EXPECT_EQ(frame["gpuIssueFrameId"], 10);
    EXPECT_DOUBLE_EQ(frame["rendererCpuMs"].get<f64>(), 1);
    EXPECT_DOUBLE_EQ(frame["gpuMs"].get<f64>(), 0.5);
}

TEST(McpCpuScopeRingTest, RegistrationDeclaresOverwriteAndOffThreadWork)
{
    OloEngine::MCP::McpServer server{ OloEngine::MCP::EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    const auto tools = server.ToolsSnapshot();
    const OloEngine::MCP::ToolDef* recorder = nullptr;
    for (const auto& tool : *tools)
    {
        if (tool.Name == "olo_perf_cpu_scope_ring")
            recorder = &tool;
    }
    ASSERT_NE(recorder, nullptr);
    EXPECT_FALSE(recorder->Annotations.value("readOnlyHint", true));
    EXPECT_TRUE(recorder->Annotations.value("destructiveHint", true));
    EXPECT_FALSE(recorder->MainMarshaled);
}

TEST(McpCpuScopeRingTest, JsonDumpUsesTheSameFinalPathSandboxAsScreenshots)
{
    namespace Path = OloEngine::MCP::CapturePath;
    std::filesystem::path out;
    EXPECT_FALSE(Path::ValidateRelative("hitches/run", out, ".json").has_value());
    EXPECT_EQ(out.generic_string(), "assets/mcp-captures/hitches/run.json");
    for (const char* rejected : { "../escape", "a\\..\\escape", "C:escape", "/escape", "assets/mcp-captures" })
        EXPECT_TRUE(Path::ValidateRelative(rejected, out, ".json").has_value()) << rejected;
}

TEST(McpCpuScopeRingTest, FilteredResolvedInvalidGpuSerializesNullWithTheRealStatus)
{
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 1);
    ring.BeginFrame(0);
    ring.EndFrame(500000000, { .GpuIssueFrameId = 7, .RendererFrameAdvanced = true, .GpuFrameAdvanced = true });
    ring.BeginFrame(500000000);
    ring.EndFrame(501000000, { .GpuStatus = OloEngine::GpuTimingStatus::OutOfOrder, .GpuFrameId = 7, .GpuIssueFrameId = 8 });
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture(300));
    ASSERT_EQ(json["frames"].size(), 1);
    EXPECT_TRUE(json["frames"][0]["sameFrameGpuMs"].is_null());
    EXPECT_EQ(json["frames"][0]["sameFrameGpuStatus"], "outOfOrder");
}

TEST(McpCpuScopeRingTest, UiOnlyGpuFramesDoNotInheritAStaleRendererCpuTime)
{
    OloEngine::CpuScopeRing ring;
    ring.Arm(2, 1);
    ring.BeginFrame(0);
    ring.EndFrame(500000000, { .RendererCpuMs = 99, .GpuIssueFrameId = 7, .GpuFrameAdvanced = true });
    ring.BeginFrame(500000000);
    ring.EndFrame(501000000, { .GpuMs = 1.5, .GpuStatus = OloEngine::GpuTimingStatus::Valid, .GpuFrameId = 7, .GpuIssueFrameId = 8, .GpuFrameAdvanced = true });
    const auto json = OloEngine::MCP::CpuScopeRecording::ToJson(ring.Capture(300));
    const auto& frame = json["frames"][0];
    EXPECT_TRUE(frame["rendererCpuMs"].is_null());
    EXPECT_TRUE(frame["gpuFrameAdvanced"]);
    EXPECT_EQ(frame["sameFrameGpuStatus"], "valid");
    EXPECT_DOUBLE_EQ(frame["sameFrameGpuMs"].get<f64>(), 1.5);
}
