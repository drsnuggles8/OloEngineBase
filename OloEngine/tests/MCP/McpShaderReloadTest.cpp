#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure result-shaping behind olo_shader_reload (issue #316
// Part 4 — the shader inner-loop harness). The status-token mapping and the
// response JSON schema live in a header-only free function with no renderer /
// editor / GPU dependencies, precisely so they can be exercised here without a
// live editor or GL context — the same split McpRenderExplain.h /
// McpPhysicsExplain.h use (the MCP test binary compiles the dispatch core but
// deliberately NOT McpTools.cpp, the editor-backed handler). The live reload is
// verified separately over the MCP attach loop; this pins the status contract
// the agent reads ("ready"/"failed"/...) and the response shape.
#include "MCP/McpShaderReload.h"

#include <string>

namespace
{
    using OloEngine::ShaderCompilationStatus;
    using OloEngine::MCP::ShaderReload::Result;
    using OloEngine::MCP::ShaderReload::StatusToken;
    using OloEngine::MCP::ShaderReload::ToJson;
    using Json = nlohmann::json;
} // namespace

TEST(McpShaderReload, StatusTokenMapsEveryEnumToLowercase)
{
    EXPECT_STREQ("pending", StatusToken(ShaderCompilationStatus::Pending));
    EXPECT_STREQ("compiling", StatusToken(ShaderCompilationStatus::Compiling));
    EXPECT_STREQ("ready", StatusToken(ShaderCompilationStatus::Ready));
    EXPECT_STREQ("failed", StatusToken(ShaderCompilationStatus::Failed));
}

TEST(McpShaderReload, CleanReloadIsReadyWithEmptyLog)
{
    Result r;
    r.Name = "PBR";
    r.Found = true;
    r.Libraries = { "Renderer3D" };
    r.Status = ShaderCompilationStatus::Ready;
    r.Ok = true;
    r.RendererId = 42;
    r.Log = "";

    const Json j = ToJson(r);
    EXPECT_EQ("PBR", j.at("name").get<std::string>());
    EXPECT_TRUE(j.at("found").get<bool>());
    EXPECT_EQ("ready", j.at("status").get<std::string>());
    EXPECT_TRUE(j.at("ok").get<bool>());
    EXPECT_EQ(42u, j.at("rendererId").get<u32>());
    EXPECT_TRUE(j.at("log").get<std::string>().empty());
    ASSERT_EQ(1u, j.at("libraries").size());
    EXPECT_EQ("Renderer3D", j.at("libraries").at(0).get<std::string>());
}

TEST(McpShaderReload, FailedReloadCarriesStatusAndLog)
{
    Result r;
    r.Name = "Water";
    r.Found = true;
    r.Libraries = { "Renderer3D", "Renderer2D" };
    r.Status = ShaderCompilationStatus::Failed;
    r.Ok = false;
    r.RendererId = 0; // a failed link resets the program id
    r.Log = "ERROR: 0:12: 'vec5' : undeclared identifier";

    const Json j = ToJson(r);
    EXPECT_EQ("failed", j.at("status").get<std::string>());
    EXPECT_FALSE(j.at("ok").get<bool>());
    EXPECT_EQ(0u, j.at("rendererId").get<u32>());
    EXPECT_NE(std::string::npos, j.at("log").get<std::string>().find("undeclared identifier"));
    EXPECT_EQ(2u, j.at("libraries").size());
}

TEST(McpShaderReload, NotFoundReportsFoundFalseAndNoLibraries)
{
    Result r;
    r.Name = "DoesNotExist";
    r.Found = false; // handler returns an error before shaping when truly absent,
                     // but the schema must still degrade cleanly if asked to shape.

    const Json j = ToJson(r);
    EXPECT_FALSE(j.at("found").get<bool>());
    EXPECT_FALSE(j.at("ok").get<bool>());
    EXPECT_TRUE(j.at("libraries").empty());
    EXPECT_TRUE(j.at("log").get<std::string>().empty());
}
