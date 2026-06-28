#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit

// Unit tests for the pure JSON / Mermaid shaping behind olo_render_graph_topology_export
// (issue #316 Part 4, "LLM-analysis exports"). The shaping lives in a header that
// touches ONLY a plain engine-free Snapshot (MCP/McpRenderGraphTopology.h), so it is
// exercised here against a synthetic graph — the test binary deliberately does NOT
// compile McpTools.cpp (the editor-backed handler that reads the live RenderGraph).
// The live tool's marshal + RenderGraph enumeration path is verified over the MCP
// attach loop; this pins the structured-export shape. Mirrors McpFrameBreakdownTest.
#include "MCP/McpRenderGraphTopology.h"

#include <string>

namespace
{
    using OloEngine::MCP::RenderGraphTopology::BuildJson;
    using OloEngine::MCP::RenderGraphTopology::BuildMermaid;
    using OloEngine::MCP::RenderGraphTopology::EdgeInfo;
    using OloEngine::MCP::RenderGraphTopology::PassInfo;
    using OloEngine::MCP::RenderGraphTopology::ResourceInfo;
    using OloEngine::MCP::RenderGraphTopology::Snapshot;
    using Json = OloEngine::MCP::RenderGraphTopology::Json;

    // A small but representative three-pass graph: Shadow -> GBuffer -> Lighting,
    // plus a culled Debug pass, a compute async candidate, and two resources.
    Snapshot MakeSnapshot()
    {
        Snapshot snap;
        snap.FinalPass = "Lighting";

        snap.Passes.push_back(PassInfo{ "Shadow", "Graphics", true, false, false, false });
        snap.Passes.push_back(PassInfo{ "GBuffer", "Graphics", true, false, false, false });
        snap.Passes.push_back(PassInfo{ "AOCompute", "Compute", true, true, false, false });
        snap.Passes.push_back(PassInfo{ "Lighting", "Graphics", true, false, false, true });
        snap.Passes.push_back(PassInfo{ "DebugOverlay", "Graphics", false, false, true, false });

        snap.ExecutionOrder = { "Shadow", "GBuffer", "AOCompute", "Lighting" };

        snap.Edges.push_back(EdgeInfo{ "Shadow", "Lighting" });
        snap.Edges.push_back(EdgeInfo{ "GBuffer", "AOCompute" });
        snap.Edges.push_back(EdgeInfo{ "AOCompute", "Lighting" });

        ResourceInfo shadowMap;
        shadowMap.Name = "ShadowMapCSM";
        shadowMap.Kind = "Texture2DArray";
        shadowMap.Format = "Depth32Float";
        shadowMap.Width = 2048;
        shadowMap.Height = 2048;
        shadowMap.Samples = 1;
        shadowMap.Imported = false;
        shadowMap.HasExternalBacking = false;
        shadowMap.Producers = { "Shadow" };
        shadowMap.Consumers = { "Lighting" };
        snap.Resources.push_back(std::move(shadowMap));

        ResourceInfo sceneColor;
        sceneColor.Name = "SceneColor";
        sceneColor.Kind = "Framebuffer";
        sceneColor.Format = "RGBA16Float";
        sceneColor.Width = 1280;
        sceneColor.Height = 720;
        sceneColor.Samples = 4;
        sceneColor.Imported = true;
        sceneColor.HasExternalBacking = true;
        sceneColor.Producers = { "Lighting" };
        sceneColor.Consumers = {};
        snap.Resources.push_back(std::move(sceneColor));

        return snap;
    }
} // namespace

TEST(McpRenderGraphTopology, EmptyGraphProducesValidShape)
{
    const Json j = BuildJson(Snapshot{});

    EXPECT_EQ("", j["finalPass"].get<std::string>());
    EXPECT_EQ(0u, j["passCount"].get<u32>());
    EXPECT_EQ(0u, j["edgeCount"].get<u32>());
    EXPECT_EQ(0u, j["resourceCount"].get<u32>());
    EXPECT_TRUE(j["passes"].is_array());
    EXPECT_TRUE(j["passes"].empty());
    EXPECT_TRUE(j["executionOrder"].is_array());
    EXPECT_TRUE(j["edges"].is_array());
    EXPECT_TRUE(j["resources"].is_array());
    EXPECT_TRUE(j.contains("note"));
}

TEST(McpRenderGraphTopology, PassFlagsAndWorkTypeSerialized)
{
    const Json j = BuildJson(MakeSnapshot());

    EXPECT_EQ("Lighting", j["finalPass"].get<std::string>());
    EXPECT_EQ(5u, j["passCount"].get<u32>());
    ASSERT_EQ(5u, j["passes"].size());

    const Json& gbuffer = j["passes"][1];
    EXPECT_EQ("GBuffer", gbuffer["name"].get<std::string>());
    EXPECT_EQ("Graphics", gbuffer["workType"].get<std::string>());
    EXPECT_TRUE(gbuffer["declaresResources"].get<bool>());
    EXPECT_FALSE(gbuffer["asyncComputeCandidate"].get<bool>());
    EXPECT_FALSE(gbuffer["culled"].get<bool>());
    EXPECT_FALSE(gbuffer["isFinalPass"].get<bool>());

    const Json& compute = j["passes"][2];
    EXPECT_EQ("Compute", compute["workType"].get<std::string>());
    EXPECT_TRUE(compute["asyncComputeCandidate"].get<bool>());

    const Json& lighting = j["passes"][3];
    EXPECT_TRUE(lighting["isFinalPass"].get<bool>());

    const Json& debug = j["passes"][4];
    EXPECT_TRUE(debug["culled"].get<bool>());
    EXPECT_FALSE(debug["declaresResources"].get<bool>());
}

TEST(McpRenderGraphTopology, ExecutionOrderAndEdgesSerialized)
{
    const Json j = BuildJson(MakeSnapshot());

    const Json& order = j["executionOrder"];
    ASSERT_EQ(4u, order.size());
    EXPECT_EQ("Shadow", order[0].get<std::string>());
    EXPECT_EQ("Lighting", order[3].get<std::string>());

    EXPECT_EQ(3u, j["edgeCount"].get<u32>());
    const Json& edges = j["edges"];
    ASSERT_EQ(3u, edges.size());
    EXPECT_EQ("Shadow", edges[0]["from"].get<std::string>());
    EXPECT_EQ("Lighting", edges[0]["to"].get<std::string>());
    EXPECT_EQ("GBuffer", edges[1]["from"].get<std::string>());
    EXPECT_EQ("AOCompute", edges[1]["to"].get<std::string>());
}

TEST(McpRenderGraphTopology, ResourcesCarryDescAndProducerConsumerLists)
{
    const Json j = BuildJson(MakeSnapshot());

    EXPECT_EQ(2u, j["resourceCount"].get<u32>());
    const Json& shadow = j["resources"][0];
    EXPECT_EQ("ShadowMapCSM", shadow["name"].get<std::string>());
    EXPECT_EQ("Texture2DArray", shadow["kind"].get<std::string>());
    EXPECT_EQ("Depth32Float", shadow["format"].get<std::string>());
    EXPECT_EQ(2048u, shadow["width"].get<u32>());
    EXPECT_EQ(2048u, shadow["height"].get<u32>());
    EXPECT_FALSE(shadow["imported"].get<bool>());
    EXPECT_FALSE(shadow["hasExternalBacking"].get<bool>());
    ASSERT_EQ(1u, shadow["producers"].size());
    EXPECT_EQ("Shadow", shadow["producers"][0].get<std::string>());
    ASSERT_EQ(1u, shadow["consumers"].size());
    EXPECT_EQ("Lighting", shadow["consumers"][0].get<std::string>());
    // Samples == 1 is omitted (the common case); only MSAA reports samples.
    EXPECT_FALSE(shadow.contains("samples"));

    const Json& scene = j["resources"][1];
    EXPECT_EQ("Framebuffer", scene["kind"].get<std::string>());
    EXPECT_TRUE(scene["imported"].get<bool>());
    EXPECT_TRUE(scene["hasExternalBacking"].get<bool>());
    EXPECT_EQ(4u, scene["samples"].get<u32>());
    EXPECT_TRUE(scene["consumers"].is_array());
    EXPECT_TRUE(scene["consumers"].empty());
}

TEST(McpRenderGraphTopology, ResourceOmitsUnknownFormatAndZeroSize)
{
    Snapshot snap;
    ResourceInfo buf;
    buf.Name = "LightCullingSSBO";
    buf.Kind = "StorageBuffer";
    // No format, no size, samples default 1.
    buf.Producers = { "AOCompute" };
    snap.Resources.push_back(std::move(buf));

    const Json j = BuildJson(snap);
    const Json& res = j["resources"][0];
    EXPECT_EQ("StorageBuffer", res["kind"].get<std::string>());
    EXPECT_FALSE(res.contains("format"));
    EXPECT_FALSE(res.contains("width"));
    EXPECT_FALSE(res.contains("height"));
    EXPECT_FALSE(res.contains("samples"));
}

TEST(McpRenderGraphTopology, MermaidEmitsFlowchartNodesAndEdges)
{
    const std::string mermaid = BuildMermaid(MakeSnapshot());

    // Header + a synthetic id per pass + arrow edges.
    EXPECT_NE(std::string::npos, mermaid.find("flowchart LR"));
    EXPECT_NE(std::string::npos, mermaid.find("n0[\"Shadow\"]"));
    // Non-graphics passes annotate their work type in the label.
    EXPECT_NE(std::string::npos, mermaid.find("AOCompute [Compute]"));
    // Edges are rendered between the synthetic node ids, never the raw names.
    EXPECT_NE(std::string::npos, mermaid.find(" --> "));
    // The final pass and the culled pass get a style class.
    EXPECT_NE(std::string::npos, mermaid.find("classDef finalPass"));
    EXPECT_NE(std::string::npos, mermaid.find("classDef culled"));
    EXPECT_NE(std::string::npos, mermaid.find(" finalPass;"));
    EXPECT_NE(std::string::npos, mermaid.find(" culled;"));
}

TEST(McpRenderGraphTopology, MermaidEscapesQuotesAndUsesSyntheticIds)
{
    Snapshot snap;
    // A versioned name with characters Mermaid rejects in a node id, plus a quote.
    snap.Passes.push_back(PassInfo{ "Post\"Process@v2", "Graphics", true, false, false, true });
    snap.FinalPass = "Post\"Process@v2";

    const std::string mermaid = BuildMermaid(snap);
    // The id is synthetic (n0); the raw name appears only inside the quoted, escaped label.
    EXPECT_NE(std::string::npos, mermaid.find("n0[\"Post&quot;Process@v2\"]"));
    // The literal double quote must not leak into the label unescaped.
    EXPECT_EQ(std::string::npos, mermaid.find("\"Post\"Process"));
}
