// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpSchedulerGraphTest — unit test (headless, no GL, no editor, no Scene).
//
// Pins the shaping half of olo_scheduler_graph (issue #607): the JSON / Mermaid /
// DOT renderings of the gameplay SystemScheduler's DERIVED dependency graph.
//
// Header-only (MCP/McpSchedulerGraph.h) against a SYNTHETIC graph, so no editor TU
// is pulled in and no scheduler has to be built — the same split
// McpRenderGraphTopologyTest uses for the other DAG. The live-graph half (walking
// Scene::GetGameplayScheduler and filling the Snapshot) is pinned separately by
// SystemSchedulerTest's ExportGraph cases, which need a real scheduler.
//
// The load-bearing assertion here is 'mayOverlapWith'. Per CLAUDE.md a MISSING
// edge is invisible in the sequential order — the registration-order tie-break
// silently supplies it — and only becomes a data race under the parallel executor.
// So the field that answers "what is this system actually racing against?" is the
// reason the export exists, and getting it wrong (listing an ordered pair, or
// listing an unmarked system the executor joins before) would make the export
// worse than nothing: confidently wrong instead of absent.
// =============================================================================

#include "MCP/McpSchedulerGraph.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    namespace Graph = OloEngine::MCP::SchedulerGraph;

    Graph::NodeInfo Node(std::string name, u32 order, bool parallel = false,
                         std::vector<std::string> reads = {}, std::vector<std::string> writes = {})
    {
        Graph::NodeInfo node;
        node.Name = std::move(name);
        node.OrderIndex = order;
        node.Parallel = parallel;
        node.Reads = std::move(reads);
        node.Writes = std::move(writes);
        return node;
    }

    // A chain A -> B -> C where A and C are parallel-marked but ORDERED, plus an
    // unconstrained parallel D. The only genuinely concurrent pairs are (A, D) and
    // (C, D) — never (A, C), which an implementation that only checks direct edges
    // would wrongly report as overlapping.
    Graph::Snapshot ChainWithAnIndependentPeer()
    {
        Graph::Snapshot snap;
        snap.ParallelExecutionEnabled = true;
        snap.Nodes = { Node("A", 0, /*parallel*/ true, {}, { "chan" }),
                       Node("B", 1, /*parallel*/ false, { "chan" }, { "chan2" }),
                       Node("C", 2, /*parallel*/ true, { "chan2" }, {}),
                       Node("D", 3, /*parallel*/ true, {}, { "unrelated" }) };
        snap.Edges = { { "A", "B" }, { "B", "C" } };
        return snap;
    }

    [[nodiscard]] bool Contains(const std::vector<std::string>& haystack, const std::string& needle)
    {
        return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
    }
} // namespace

TEST(McpSchedulerGraph, JsonReportsOrderEdgesAndCounts)
{
    const auto json = Graph::BuildJson(ChainWithAnIndependentPeer());

    EXPECT_EQ(json.at("systemCount").get<u32>(), 4u);
    EXPECT_EQ(json.at("parallelSystemCount").get<u32>(), 3u);
    EXPECT_TRUE(json.at("parallelExecutionEnabled").get<bool>());
    EXPECT_EQ(json.at("edgeCount").get<u32>(), 2u);
    EXPECT_EQ(json.at("executionOrder").get<std::vector<std::string>>(),
              (std::vector<std::string>{ "A", "B", "C", "D" }));
}

TEST(McpSchedulerGraph, MayOverlapWithExcludesTransitivelyOrderedSystems)
{
    const auto json = Graph::BuildJson(ChainWithAnIndependentPeer());

    std::vector<std::string> aPeers;
    std::vector<std::string> cPeers;
    for (const auto& system : json.at("systems"))
    {
        if (system.at("name") == "A")
            aPeers = system.value("mayOverlapWith", std::vector<std::string>{});
        if (system.at("name") == "C")
            cPeers = system.value("mayOverlapWith", std::vector<std::string>{});
    }

    // A -> B -> C is a two-hop path, so A and C can never run concurrently even
    // though no direct A->C edge exists. Reporting them as overlapping would send
    // an auditor after a race that cannot happen — and, worse, would suggest the
    // transitive closure is not being computed, which is the same bug that would
    // MISS a real pair elsewhere.
    EXPECT_FALSE(Contains(aPeers, "C"));
    EXPECT_FALSE(Contains(cPeers, "A"));
    EXPECT_TRUE(Contains(aPeers, "D"));
    EXPECT_TRUE(Contains(cPeers, "D"));
}

TEST(McpSchedulerGraph, MayOverlapWithNeverListsAnUnmarkedSystem)
{
    const auto json = Graph::BuildJson(ChainWithAnIndependentPeer());

    for (const auto& system : json.at("systems"))
    {
        if (!system.at("parallel").get<bool>())
        {
            // An unmarked system is a join-all barrier: it overlaps nothing by
            // construction, so it must not carry the field at all.
            EXPECT_FALSE(system.contains("mayOverlapWith"))
                << system.at("name").get<std::string>() << " is unmarked and cannot overlap anything";
            continue;
        }
        // B is unmarked; a marked system must never claim to race it.
        EXPECT_FALSE(Contains(system.at("mayOverlapWith").get<std::vector<std::string>>(), "B"));
    }
}

TEST(McpSchedulerGraph, ChannelIndexPairsReadersWithWritersAndIsSorted)
{
    Graph::Snapshot snap;
    snap.Nodes = { Node("Writer", 0, false, {}, { "zebra", "alpha" }),
                   Node("Reader", 1, false, { "alpha" }, {}),
                   // A misspelled channel: written, never read. The inverse index is
                   // the fastest way to see that, which is why it is exported.
                   Node("Typo", 2, false, {}, { "mango" }) };

    const auto json = Graph::BuildJson(snap);
    const auto& channels = json.at("channels");
    ASSERT_EQ(channels.size(), 3u);

    // Sorted: byChannel is an unordered_map, whose iteration order is
    // implementation-defined and may vary between runs. Two identical schedules
    // must not export differently.
    std::vector<std::string> names;
    for (const auto& channel : channels)
        names.push_back(channel.at("name").get<std::string>());
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));

    for (const auto& channel : channels)
    {
        if (channel.at("name") == "alpha")
        {
            EXPECT_EQ(channel.at("writers").get<std::vector<std::string>>(), (std::vector<std::string>{ "Writer" }));
            EXPECT_EQ(channel.at("readers").get<std::vector<std::string>>(), (std::vector<std::string>{ "Reader" }));
        }
        if (channel.at("name") == "mango")
            EXPECT_TRUE(channel.at("readers").get<std::vector<std::string>>().empty());
    }
}

TEST(McpSchedulerGraph, DeclaredAfterBeforeAreEmittedOnlyWhenPresent)
{
    Graph::Snapshot snap;
    Graph::NodeInfo constrained = Node("Late", 1);
    constrained.After = { "Early" };
    snap.Nodes = { Node("Early", 0), constrained };
    snap.Edges = { { "Early", "Late" } };

    const auto json = Graph::BuildJson(snap);
    for (const auto& system : json.at("systems"))
    {
        if (system.at("name") == "Late")
        {
            ASSERT_TRUE(system.contains("after"));
            EXPECT_EQ(system.at("after").get<std::vector<std::string>>(), (std::vector<std::string>{ "Early" }));
        }
        else
        {
            // Most systems declare neither; emitting empty arrays for all of them
            // would bury the handful that do.
            EXPECT_FALSE(system.contains("after"));
            EXPECT_FALSE(system.contains("before"));
        }
    }
}

TEST(McpSchedulerGraph, MermaidDeclaresEveryNodeAndEdgeAndMarksParallel)
{
    const std::string mermaid = Graph::BuildMermaid(ChainWithAnIndependentPeer());

    EXPECT_TRUE(mermaid.starts_with("flowchart LR\n"));
    for (const char* name : { "A", "B", "C", "D" })
        EXPECT_NE(mermaid.find(std::string(". ") + name), std::string::npos) << "node " << name << " is missing";
    EXPECT_NE(mermaid.find("[par]"), std::string::npos) << "parallel nodes must be visually distinguishable";
    EXPECT_NE(mermaid.find("classDef parallel"), std::string::npos);
    // Exactly the two derived edges, in Mermaid's arrow syntax.
    std::size_t arrows = 0;
    for (std::size_t at = mermaid.find(" --> "); at != std::string::npos; at = mermaid.find(" --> ", at + 1))
        ++arrows;
    EXPECT_EQ(arrows, 2u);
}

TEST(McpSchedulerGraph, DotIsWellFormedAndStylesParallelNodes)
{
    const std::string dot = Graph::BuildDot(ChainWithAnIndependentPeer());

    EXPECT_TRUE(dot.starts_with("digraph GameplaySchedule {\n"));
    EXPECT_TRUE(dot.ends_with("}\n"));
    EXPECT_NE(dot.find("rankdir=LR;"), std::string::npos);
    EXPECT_NE(dot.find("fillcolor=\"#e3f2fd\""), std::string::npos) << "a parallel node must be styled";
    // Exactly the two derived edges, in DOT's arrow syntax.
    std::size_t arrows = 0;
    for (std::size_t at = dot.find(" -> "); at != std::string::npos; at = dot.find(" -> ", at + 1))
        ++arrows;
    EXPECT_EQ(arrows, 2u);
}

TEST(McpSchedulerGraph, LabelsWithQuotesAreEscapedPerFormat)
{
    // A system name is a plain identifier today, but neither renderer is required
    // to accept an arbitrary one, and a bare quote would truncate the label (DOT)
    // or break the diagram outright (Mermaid). The two formats need DIFFERENT
    // escapes, which is exactly the kind of thing a shared helper gets wrong once
    // and then nobody notices until a name changes.
    Graph::Snapshot snap;
    snap.Nodes = { Node("Say \"hi\"", 0) };

    EXPECT_NE(Graph::BuildMermaid(snap).find("&quot;hi&quot;"), std::string::npos);
    EXPECT_NE(Graph::BuildDot(snap).find("\\\"hi\\\""), std::string::npos);
}

TEST(McpSchedulerGraph, EmptyGraphRendersWithoutCrashingOrLying)
{
    const Graph::Snapshot empty;
    const auto json = Graph::BuildJson(empty);
    EXPECT_EQ(json.at("systemCount").get<u32>(), 0u);
    EXPECT_EQ(json.at("edgeCount").get<u32>(), 0u);
    EXPECT_EQ(json.at("channelCount").get<u32>(), 0u);
    EXPECT_FALSE(json.at("parallelExecutionEnabled").get<bool>());

    EXPECT_TRUE(Graph::BuildMermaid(empty).starts_with("flowchart LR"));
    EXPECT_TRUE(Graph::BuildDot(empty).ends_with("}\n"));
}
