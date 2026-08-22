#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit

// Unit tests for the pure JSON / Mermaid shaping behind olo_render_graph_topology_export
// (issue #316, "LLM-analysis exports"). The shaping lives in a header that
// touches ONLY a plain engine-free Snapshot (MCP/McpRenderGraphTopology.h), so it is
// exercised here against a synthetic graph — the test binary deliberately does NOT
// compile McpTools.cpp (the editor-backed handler that reads the live RenderGraph).
// The live tool's marshal + RenderGraph enumeration path is verified over the MCP
// attach loop; this pins the structured-export shape. Mirrors McpFrameBreakdownTest.
#include "MCP/McpRenderGraphTopology.h"

#include <string>

namespace
{
    using OloEngine::MCP::RenderGraphTopology::BuildDot;
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

// ---- Graphviz DOT (issue #607) ---------------------------------------------
//
// Added so both of the engine's derived DAGs — this one and the gameplay
// SystemScheduler (olo_scheduler_graph) — can be exported in the same two
// drawable formats, rather than the reader having to know which tool speaks
// which language.

TEST(McpRenderGraphTopology, DotEmitsDigraphNodesEdgesAndStyles)
{
    const std::string dot = BuildDot(MakeSnapshot());

    EXPECT_TRUE(dot.starts_with("digraph RenderGraph {\n"));
    EXPECT_TRUE(dot.ends_with("}\n"));
    EXPECT_NE(std::string::npos, dot.find("rankdir=LR;"));
    EXPECT_NE(std::string::npos, dot.find("n0 [label=\"Shadow\""));
    EXPECT_NE(std::string::npos, dot.find("AOCompute [Compute]"));
    // The same three edges as the Mermaid rendering, in DOT's arrow syntax.
    std::size_t arrows = 0;
    for (std::size_t at = dot.find(" -> "); at != std::string::npos; at = dot.find(" -> ", at + 1))
        ++arrows;
    EXPECT_EQ(arrows, 3u);
    // Final and culled passes stay visually distinct, as they are in Mermaid.
    EXPECT_NE(std::string::npos, dot.find("#d4edda"));
    EXPECT_NE(std::string::npos, dot.find("dashed"));
}

TEST(McpRenderGraphTopology, DotEscapesQuotesWithABackslashNotAnEntity)
{
    // Mermaid takes an HTML entity, DOT a backslash. Sharing one escape between the
    // two renderers would silently corrupt one of them, and a pass name only grows
    // an awkward character long after the code was written.
    Snapshot snap;
    snap.Passes.push_back(PassInfo{ "Post\"Process@v2", "Graphics", true, false, false, true });

    const std::string dot = BuildDot(snap);
    EXPECT_NE(std::string::npos, dot.find("n0 [label=\"Post\\\"Process@v2\""));
    EXPECT_EQ(std::string::npos, dot.find("&quot;"));
}

TEST(McpRenderGraphTopology, DotDoublesALiteralBackslashSoItCannotEscapeTheClosingQuote)
{
    // Escaping the quote but not the character that escapes it is worse than
    // escaping neither: a name ending in a backslash would emit ...\" — whose
    // backslash consumes the closing quote and swallows the rest of the line,
    // producing DOT that will not parse. Mermaid needs no such treatment (its
    // escape is an HTML entity, so a backslash is an ordinary character there).
    Snapshot snap;
    snap.Passes.push_back(PassInfo{ "Path\\To\\Pass", "Graphics", true, false, false, false });

    const std::string dot = BuildDot(snap);
    EXPECT_NE(std::string::npos, dot.find("n0 [label=\"Path\\\\To\\\\Pass\""));

    // The trailing-backslash case is the one that actually breaks the parse.
    Snapshot trailing;
    trailing.Passes.push_back(PassInfo{ "Trailing\\", "Graphics", true, false, false, false });
    EXPECT_NE(std::string::npos, BuildDot(trailing).find("n0 [label=\"Trailing\\\\\"];"));

    // Mermaid leaves it alone.
    EXPECT_NE(std::string::npos, BuildMermaid(snap).find("n0[\"Path\\To\\Pass\"]"));
}

// ---- Resolved physical backing + per-pass access lists (issues #607, #890) --

TEST(McpRenderGraphTopology, ResourceEmitsBothCurrenciesWhenSet)
{
    Snapshot snap;
    ResourceInfo tex;
    tex.Name = "SceneDepth";
    tex.Kind = "Texture2D";
    tex.NativeTextureHandle = 42;
    tex.TextureIdentity = 0x0000000100000007ull;
    tex.ViewOfParentLayer = 3;
    snap.Resources.push_back(std::move(tex));

    ResourceInfo fb;
    fb.Name = "SceneColor";
    fb.Kind = "Framebuffer";
    fb.NativeFramebufferHandle = 7;
    fb.NativeColorAttachmentHandles = { 10, 11 };
    fb.ColorAttachmentIdentities = { 0x0000000100000021ull, 0x0000000100000022ull };
    fb.NativeDepthAttachmentHandle = 12;
    fb.DepthAttachmentIdentity = 0x0000000100000023ull;
    snap.Resources.push_back(std::move(fb));

    ResourceInfo unbacked;
    unbacked.Name = "OptionalVelocity";
    unbacked.Kind = "Texture2D"; // nothing resolved, in either currency
    snap.Resources.push_back(std::move(unbacked));

    const Json j = BuildJson(snap);
    const Json& texJson = j["resources"][0];
    ASSERT_TRUE(texJson.contains("native"));
    EXPECT_EQ("0x2A", texJson["native"]["texture"].get<std::string>());
    ASSERT_TRUE(texJson.contains("identity"));
    EXPECT_EQ("#7:1", texJson["identity"]["texture"].get<std::string>());
    EXPECT_EQ(3u, texJson["identity"]["viewOfParentLayer"].get<u32>());

    const Json& fbJson = j["resources"][1];
    EXPECT_EQ("0x7", fbJson["native"]["framebuffer"].get<std::string>());
    ASSERT_EQ(2u, fbJson["native"]["colorAttachments"].size());
    EXPECT_EQ("0xA", fbJson["native"]["colorAttachments"][0].get<std::string>());
    EXPECT_EQ("0xC", fbJson["native"]["depthAttachment"].get<std::string>());
    EXPECT_EQ("#33:1", fbJson["identity"]["colorAttachments"][0].get<std::string>());
    EXPECT_EQ("#35:1", fbJson["identity"]["depthAttachment"].get<std::string>());

    // No resolved backing -> neither block at all (absence is the signal).
    EXPECT_FALSE(j["resources"][2].contains("native"));
    EXPECT_FALSE(j["resources"][2].contains("identity"));
}

// The Vulkan shape (issue #890): a framebuffer-backed resource whose native
// attachment handles are all 0 by design still reports its identity, so the
// "which physical object is this" question stays answerable. Emitting only the
// native block made every such resource look identical — and unbacked.
TEST(McpRenderGraphTopology, IdentityIsEmittedWhenTheNativeHandleIsZero)
{
    Snapshot snap;
    ResourceInfo fb;
    fb.Name = "SceneColor";
    fb.Kind = "Framebuffer";
    fb.NativeColorAttachmentHandles = { 0, 0, 0, 0 }; // VulkanFramebuffer, by design
    fb.ColorAttachmentIdentities = { 0x0000000100000007ull, 0, 0, 0 };
    snap.Resources.push_back(std::move(fb));

    const Json j = BuildJson(snap);
    const Json& fbJson = j["resources"][0];

    // The native block is still emitted, and that is deliberate: the array
    // carries the attachment COUNT and keeps index-correspondence with the
    // identity array, so "0x0" reads as "this attachment has no native name"
    // rather than the resource vanishing from the export entirely.
    ASSERT_TRUE(fbJson.contains("native"));
    ASSERT_EQ(4u, fbJson["native"]["colorAttachments"].size());
    EXPECT_EQ("0x0", fbJson["native"]["colorAttachments"][0].get<std::string>());
    EXPECT_FALSE(fbJson["native"].contains("framebuffer")) << "a scalar 0 IS omitted";

    // ...and the identity is there to answer the question the native handles
    // cannot. This is the whole point of #890: on Vulkan this is the only
    // field that distinguishes one framebuffer attachment from another.
    ASSERT_TRUE(fbJson.contains("identity"));
    EXPECT_EQ("#7:1", fbJson["identity"]["colorAttachments"][0].get<std::string>());
    EXPECT_EQ("", fbJson["identity"]["colorAttachments"][1].get<std::string>())
        << "an attachment with no identity reads as empty, not as a token naming nothing";

    // And the pass-access key still resolves through the identity.
    ResourceInfo probe;
    probe.ColorAttachmentIdentities = { 0x0000000100000007ull, 0, 0, 0 };
    probe.NativeColorAttachmentHandles = { 0, 0, 0, 0 };
    EXPECT_EQ(0x0000000100000007ull,
              OloEngine::MCP::RenderGraphTopology::AccessedPhysicalKey(probe));
}

TEST(McpRenderGraphTopology, PassAccessesInvertProducersConsumersWithPhysicalKeys)
{
    Snapshot snap;
    snap.Passes.push_back(PassInfo{ "GTAOPass", "Compute", true, false, false, false });
    snap.Passes.push_back(PassInfo{ "ParticlePass", "Graphics", true, false, false, false });

    ResourceInfo depth;
    depth.Name = "SceneDepth";
    depth.Kind = "Texture2D";
    depth.NativeTextureHandle = 99;
    depth.TextureIdentity = 0x0000000100000009ull;
    depth.Producers = { "ParticlePass" };
    depth.Consumers = { "GTAOPass" };
    snap.Resources.push_back(std::move(depth));

    const Json j = BuildJson(snap);
    // GTAOPass reads SceneDepth; ParticlePass writes it — both accesses carry
    // the SAME physical key, the one-call aliasing answer.
    const Json& gtao = j["passes"][0];
    ASSERT_TRUE(gtao.contains("accesses"));
    ASSERT_EQ(1u, gtao["accesses"].size());
    EXPECT_EQ("SceneDepth", gtao["accesses"][0]["resource"].get<std::string>());
    EXPECT_EQ("read", gtao["accesses"][0]["mode"].get<std::string>());
    EXPECT_EQ("#9:1", gtao["accesses"][0]["physicalKey"].get<std::string>());
    EXPECT_EQ("0x63", gtao["accesses"][0]["nativeTexture"].get<std::string>());

    const Json& particle = j["passes"][1];
    ASSERT_TRUE(particle.contains("accesses"));
    EXPECT_EQ("write", particle["accesses"][0]["mode"].get<std::string>());
    EXPECT_EQ("#9:1", particle["accesses"][0]["physicalKey"].get<std::string>());
}

TEST(McpRenderGraphTopology, AccessPhysicalKeyFallsBackToFramebufferAttachments)
{
    using OloEngine::MCP::RenderGraphTopology::AccessedPhysicalKey;

    ResourceInfo colorFb;
    colorFb.ColorAttachmentIdentities = { 21 };
    colorFb.DepthAttachmentIdentity = 22;
    EXPECT_EQ(21ull, AccessedPhysicalKey(colorFb));

    ResourceInfo depthOnlyFb;
    depthOnlyFb.DepthAttachmentIdentity = 22;
    EXPECT_EQ(22ull, AccessedPhysicalKey(depthOnlyFb));

    ResourceInfo tex;
    tex.TextureIdentity = 5;
    tex.ColorAttachmentIdentities = { 21 }; // the texture identity wins when both are set
    EXPECT_EQ(5ull, AccessedPhysicalKey(tex));

    // The IDENTITY outranks the native handle, which is the #890 ordering: on
    // Vulkan the native value is 0 for exactly the resources whose identity is
    // the only thing that can distinguish them.
    ResourceInfo both;
    both.TextureIdentity = 5;
    both.NativeTextureHandle = 999;
    EXPECT_EQ(5ull, AccessedPhysicalKey(both));

    // Native-only (a resource imported as a bare native id) still resolves.
    ResourceInfo nativeOnly;
    nativeOnly.NativeTextureHandle = 999;
    EXPECT_EQ(999ull, AccessedPhysicalKey(nativeOnly));
}
