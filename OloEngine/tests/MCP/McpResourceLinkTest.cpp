// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Resource links + the dynamic resource registry (#673 Tier 1, bullet 3).
// Drives the httplib-free dispatch seam (McpServer.cpp is compiled into the
// test binary) with fake resources/tools — no live editor, GPU, or socket:
//   * the binary `blob` contents variant of resources/read (ResourceDef::BlobReader);
//   * RegisterEphemeralResource: runtime publish, duplicate-URI replace,
//     oldest-first eviction under the count/byte bounds, startup resources
//     never evicted, notifications/resources/list_changed fan-out + the
//     ResourcesGeneration() counter the SSE stream polls;
//   * the copy-on-write snapshot property (an eviction never invalidates a
//     held snapshot — what keeps capture bytes alive under an in-flight read);
//   * ToolResult::ResourceLinkBlock — the resource_link content block the
//     capture tools emit in link-delivery mode — including its pass-through in
//     tools/call and path redaction of its human-readable fields.
#include "MCP/McpServer.h"

#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ResourceDef;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json MakeRequest(const Json& id, const std::string& method, const Json& params = Json::object())
    {
        Json req = { { "jsonrpc", "2.0" }, { "method", method } };
        if (!id.is_null())
            req["id"] = id;
        if (!params.is_null())
            req["params"] = params;
        return req;
    }

    ResourceDef MakeBlobResource(const std::string& uri, std::vector<u8> bytes,
                                 const std::string& name = "capture")
    {
        ResourceDef resource;
        resource.Uri = uri;
        resource.Name = name;
        resource.Description = "A fake capture.";
        resource.MimeType = "image/png";
        resource.SizeBytes = bytes.size();
        resource.BlobReader = [bytes = std::move(bytes)](McpServer&)
        { return bytes; };
        return resource;
    }

    Json ListResources(McpServer& server)
    {
        const Json response = server.HandleMessage(MakeRequest(1, "resources/list"));
        EXPECT_TRUE(response.contains("result")) << response.dump(2);
        return response["result"]["resources"];
    }

    bool ListContains(const Json& resources, const std::string& uri)
    {
        for (const auto& entry : resources)
        {
            if (entry.value("uri", std::string{}) == uri)
                return true;
        }
        return false;
    }
} // namespace

// ---- blob contents variant ---------------------------------------------------

TEST(McpResourceLink, BlobResourceReadReturnsBase64BlobContents)
{
    McpServer server{ EditorMcpContext{} };
    const std::vector<u8> bytes = { 0x89, 'P', 'N', 'G', 0x00, 0xFF };
    server.RegisterResource(MakeBlobResource("olo://capture/000001/fake.png", bytes));

    const Json response = server.HandleMessage(
        MakeRequest(2, "resources/read", Json{ { "uri", "olo://capture/000001/fake.png" } }));

    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    const Json& contents = response["result"]["contents"];
    ASSERT_TRUE(contents.is_array());
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(contents[0]["uri"], "olo://capture/000001/fake.png");
    EXPECT_EQ(contents[0]["mimeType"], "image/png");
    EXPECT_EQ(contents[0]["blob"], OloEngine::MCP::Base64Encode(bytes));
    EXPECT_FALSE(contents[0].contains("text")) << "binary contents must use blob, not text";
}

TEST(McpResourceLink, ResourcesListAdvertisesSizeOnlyWhenKnown)
{
    McpServer server{ EditorMcpContext{} };
    server.RegisterResource(MakeBlobResource("olo://capture/1", { 1, 2, 3 }));

    ResourceDef textResource;
    textResource.Uri = "olo://fake/text";
    textResource.Name = "text";
    textResource.Description = "Text resource with unknown size.";
    textResource.MimeType = "text/plain";
    textResource.Reader = [](McpServer&)
    { return std::string("hello"); };
    server.RegisterResource(std::move(textResource));

    const Json resources = ListResources(server);
    ASSERT_EQ(resources.size(), 2u);
    for (const auto& entry : resources)
    {
        if (entry["uri"] == "olo://capture/1")
            EXPECT_EQ(entry["size"], 3);
        else
            EXPECT_FALSE(entry.contains("size"));
    }
}

// ---- ephemeral publish / eviction --------------------------------------------

TEST(McpResourceLink, EphemeralPublishAppearsInListNotifiesAndBumpsGeneration)
{
    McpServer server{ EditorMcpContext{} };
    std::vector<std::string> notified;
    server.AddNotificationListener([&notified](const Json& notification)
                                   { notified.push_back(notification.value("method", std::string{})); });

    const u64 generationBefore = server.ResourcesGeneration();
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/000001/shot.png", { 1, 2, 3, 4 }));

    EXPECT_GT(server.ResourcesGeneration(), generationBefore);
    ASSERT_EQ(notified.size(), 1u);
    EXPECT_EQ(notified[0], "notifications/resources/list_changed");
    EXPECT_TRUE(ListContains(ListResources(server), "olo://capture/000001/shot.png"));
}

TEST(McpResourceLink, EphemeralCountEvictionDropsOldestFirst)
{
    McpServer server{ EditorMcpContext{} };
    server.SetEphemeralResourceLimits(2, 1024 * 1024);

    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/1", { 1 }));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/2", { 2 }));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/3", { 3 }));

    const Json resources = ListResources(server);
    EXPECT_FALSE(ListContains(resources, "olo://capture/1")) << "oldest ephemeral must be evicted";
    EXPECT_TRUE(ListContains(resources, "olo://capture/2"));
    EXPECT_TRUE(ListContains(resources, "olo://capture/3"));
}

TEST(McpResourceLink, EphemeralByteBudgetEvictsUntilWithinBounds)
{
    McpServer server{ EditorMcpContext{} };
    server.SetEphemeralResourceLimits(16, 100);

    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/big1", std::vector<u8>(60, 0xAA)));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/big2", std::vector<u8>(60, 0xBB)));

    const Json resources = ListResources(server);
    EXPECT_FALSE(ListContains(resources, "olo://capture/big1")) << "byte budget must evict the oldest";
    EXPECT_TRUE(ListContains(resources, "olo://capture/big2"));
}

TEST(McpResourceLink, OversizedSingleCaptureIsStillServed)
{
    // A capture bigger than the whole byte budget must not evict ITSELF into a
    // dead link — the bound only displaces older entries.
    McpServer server{ EditorMcpContext{} };
    server.SetEphemeralResourceLimits(16, 10);
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/huge", std::vector<u8>(64, 0xCC)));
    EXPECT_TRUE(ListContains(ListResources(server), "olo://capture/huge"));
}

TEST(McpResourceLink, StartupResourcesAreNeverEvicted)
{
    McpServer server{ EditorMcpContext{} };
    server.SetEphemeralResourceLimits(1, 1024);

    ResourceDef startup;
    startup.Uri = "olo://scene/fake";
    startup.Name = "scene";
    startup.Description = "Startup resource.";
    startup.MimeType = "text/yaml";
    startup.Reader = [](McpServer&)
    { return std::string("scene: {}"); };
    server.RegisterResource(std::move(startup));

    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/1", { 1 }));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/2", { 2 }));

    const Json resources = ListResources(server);
    EXPECT_TRUE(ListContains(resources, "olo://scene/fake"));
    EXPECT_FALSE(ListContains(resources, "olo://capture/1"));
    EXPECT_TRUE(ListContains(resources, "olo://capture/2"));
}

TEST(McpResourceLink, ClearEphemeralResourcesRemovesOnlyEphemerals)
{
    McpServer server{ EditorMcpContext{} };

    ResourceDef startup;
    startup.Uri = "olo://logs/fake";
    startup.Name = "logs";
    startup.Description = "Startup resource.";
    startup.MimeType = "text/plain";
    startup.Reader = [](McpServer&)
    { return std::string("log line"); };
    server.RegisterResource(std::move(startup));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/1", { 1 }));

    server.ClearEphemeralResources();

    const Json resources = ListResources(server);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0]["uri"], "olo://logs/fake");
}

TEST(McpResourceLink, DuplicateUriReplacesInsteadOfShadowing)
{
    McpServer server{ EditorMcpContext{} };
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/same", { 1, 1, 1 }));
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/same", { 2, 2 }));

    const Json resources = ListResources(server);
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0]["size"], 2);

    const Json response =
        server.HandleMessage(MakeRequest(3, "resources/read", Json{ { "uri", "olo://capture/same" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["contents"][0]["blob"],
              OloEngine::MCP::Base64Encode({ 2, 2 }));
}

// The copy-on-write property the whole design leans on: a snapshot taken before
// an eviction keeps every ResourceDef (and the capture bytes its BlobReader
// owns) alive and readable — the resource-registry mirror of the tool-registry
// swap test in McpProtocolIconsTest.
TEST(McpResourceLink, HeldSnapshotSurvivesEviction)
{
    McpServer server{ EditorMcpContext{} };
    server.SetEphemeralResourceLimits(1, 1024);
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/old", { 7, 7, 7 }));

    const McpServer::ResourceSnapshot held = server.ResourcesSnapshot();
    server.RegisterEphemeralResource(MakeBlobResource("olo://capture/new", { 8 })); // evicts "old"

    EXPECT_FALSE(ListContains(ListResources(server), "olo://capture/old"));
    const OloEngine::MCP::ResourceDef* old = nullptr;
    for (const auto& entry : *held)
    {
        if (entry.Uri == "olo://capture/old")
            old = &entry;
    }
    ASSERT_NE(old, nullptr) << "held snapshot lost an entry across a swap";
    EXPECT_EQ(old->BlobReader(server), (std::vector<u8>{ 7, 7, 7 }));
}

// ---- the resource_link content block -----------------------------------------

TEST(McpResourceLink, ResourceLinkBlockFactoryShape)
{
    const Json block = ToolResult::ResourceLinkBlock("olo://capture/000001/shot.png", "shot.png",
                                                     "Viewport capture 1024x768.", "image/png", 123456);
    EXPECT_EQ(block["type"], "resource_link");
    EXPECT_EQ(block["uri"], "olo://capture/000001/shot.png");
    EXPECT_EQ(block["name"], "shot.png");
    EXPECT_EQ(block["description"], "Viewport capture 1024x768.");
    EXPECT_EQ(block["mimeType"], "image/png");
    EXPECT_EQ(block["size"], 123456);

    // Unknown size is omitted, never emitted as 0.
    const Json noSize = ToolResult::ResourceLinkBlock("olo://x", "x", "d", "image/png");
    EXPECT_FALSE(noSize.contains("size"));
}

TEST(McpResourceLink, ToolCallPassesResourceLinkBlockThroughAndRedactsIt)
{
    McpServer server{ EditorMcpContext{} };
    ToolDef tool;
    tool.Name = "fake_capture";
    tool.Description = "Returns a resource_link block.";
    tool.Handler = [](McpServer&, const Json&)
    {
        ToolResult result;
        result.Content = Json::array(
            { Json{ { "type", "text" }, { "text", "Capture published." } },
              ToolResult::ResourceLinkBlock("olo://capture/000001/shot.png", "shot.png",
                                            "Golden at C:\\Projects\\Game\\golden.png", "image/png", 42) });
        return result;
    };
    server.RegisterTool(std::move(tool));

    // Pass-through, unredacted by default.
    Json response = server.HandleMessage(MakeRequest(4, "tools/call", Json{ { "name", "fake_capture" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    ASSERT_EQ(response["result"]["content"].size(), 2u);
    EXPECT_EQ(response["result"]["content"][1]["type"], "resource_link");
    EXPECT_EQ(response["result"]["content"][1]["size"], 42);
    EXPECT_EQ(response["result"]["content"][1]["description"], "Golden at C:\\Projects\\Game\\golden.png");

    // With redaction on, the human-readable fields lose absolute paths.
    server.SetRedactPaths(true);
    response = server.HandleMessage(MakeRequest(5, "tools/call", Json{ { "name", "fake_capture" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["content"][1]["description"], "Golden at <path>");
    EXPECT_EQ(response["result"]["content"][1]["uri"], "olo://capture/000001/shot.png")
        << "an olo:// uri carries no path and must survive redaction unchanged";
}
