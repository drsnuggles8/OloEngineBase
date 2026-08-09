#pragma once

// Pure, engine-light shaping for the olo_scheduler_graph MCP tool (issue #607):
// the gameplay SystemScheduler's DERIVED dependency graph rendered as structured
// JSON, a Mermaid flowchart, or Graphviz DOT.
//
// Why it exists. Scene::SimulateRuntimeStep no longer hard-codes an update order:
// each system declares Reads/Writes/After/Before against named channels and the
// order is DERIVED by a topological sort. That is the point — but it also means the
// thing you actually have to reason about is written down nowhere. Until this tool
// the graph could only be interrogated one yes/no question at a time
// (SystemScheduler::DependsOn), so understanding the schedule meant writing a test
// per hypothesis, and understanding a schedule you had just changed meant writing
// several.
//
// Parallelism is a first-class part of the export, not decoration. Per CLAUDE.md a
// missing edge is INVISIBLE in the sequential order — the registration-order
// tie-break silently supplies it — and only becomes a data race once the systems
// run concurrently. So an export showing edges but not which nodes are
// Parallelizable would hide exactly the half that makes the graph worth looking at.
// Every renderer below marks parallel nodes, and the JSON additionally lists, per
// node, the OTHER marked systems it may overlap with: the direct answer to "what is
// this system racing against?", derived by removing every node it is transitively
// ordered against.
//
// Keeping this in free functions over a plain Snapshot (no Scene / SystemScheduler /
// editor types) means it unit-tests against a synthetic graph without an engine —
// the same split as McpRenderGraphTopology.h, whose JSON/Mermaid shape this
// deliberately mirrors so the two DAG exports read alike.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::MCP::SchedulerGraph
{
    using Json = nlohmann::json;

    // One registered gameplay system.
    struct NodeInfo
    {
        std::string Name;
        std::vector<std::string> Reads;
        std::vector<std::string> Writes;
        std::vector<std::string> After;  // as DECLARED
        std::vector<std::string> Before; // as DECLARED
        bool Parallel = false;           // marked Parallelizable => may run on a worker
        u32 OrderIndex = 0;              // position in the derived execution order
    };

    // One derived edge: From must finish before To may start. Covers BOTH the
    // explicit After()/Before() edges and the ones derived from the read/write
    // declarations — the latter being the majority, and the ones no source file
    // shows.
    struct EdgeInfo
    {
        std::string From;
        std::string To;
    };

    struct Snapshot
    {
        std::vector<NodeInfo> Nodes; // in derived execution order
        std::vector<EdgeInfo> Edges;
        bool ParallelExecutionEnabled = false;
    };

    // ---- reachability -------------------------------------------------------

    // Transitive closure of the edge set, as name -> every name reachable from it.
    // Computed here rather than asked of the scheduler one pair at a time because
    // the interesting question ("which systems may overlap?") is quadratic in the
    // node count and there are only a few dozen nodes.
    [[nodiscard]] inline std::unordered_map<std::string, std::unordered_set<std::string>>
    ReachableFrom(const Snapshot& snap)
    {
        std::unordered_map<std::string, std::vector<std::string>> successors;
        for (const EdgeInfo& edge : snap.Edges)
            successors[edge.From].push_back(edge.To);

        std::unordered_map<std::string, std::unordered_set<std::string>> reachable;
        reachable.reserve(snap.Nodes.size());
        for (const NodeInfo& node : snap.Nodes)
        {
            std::unordered_set<std::string> seen;
            std::vector<std::string> stack{ node.Name };
            while (!stack.empty())
            {
                const std::string current = std::move(stack.back());
                stack.pop_back();
                const auto it = successors.find(current);
                if (it == successors.end())
                    continue;
                for (const std::string& next : it->second)
                {
                    if (seen.insert(next).second)
                        stack.push_back(next);
                }
            }
            reachable.emplace(node.Name, std::move(seen));
        }
        return reachable;
    }

    // For each Parallelizable node, the other Parallelizable nodes it is NOT
    // transitively ordered against — i.e. the set it can genuinely run
    // concurrently with, and therefore the exact set its thread-safety audit has
    // to cover. An unmarked system never appears: the executor joins every
    // in-flight task before running one, so it overlaps nothing by construction.
    [[nodiscard]] inline std::unordered_map<std::string, std::vector<std::string>>
    ConcurrentPeers(const Snapshot& snap)
    {
        const auto reachable = ReachableFrom(snap);
        std::unordered_map<std::string, std::vector<std::string>> peers;
        for (const NodeInfo& a : snap.Nodes)
        {
            if (!a.Parallel)
                continue;
            std::vector<std::string> overlapping;
            for (const NodeInfo& b : snap.Nodes)
            {
                if (!b.Parallel || b.Name == a.Name)
                    continue;
                const auto forward = reachable.find(a.Name);
                const auto backward = reachable.find(b.Name);
                const bool aBeforeB = forward != reachable.end() && forward->second.contains(b.Name);
                const bool bBeforeA = backward != reachable.end() && backward->second.contains(a.Name);
                if (!aBeforeB && !bBeforeA)
                    overlapping.push_back(b.Name);
            }
            peers.emplace(a.Name, std::move(overlapping));
        }
        return peers;
    }

    // ---- JSON ---------------------------------------------------------------

    [[nodiscard]] inline Json BuildJson(const Snapshot& snap)
    {
        const auto peers = ConcurrentPeers(snap);

        Json nodes = Json::array();
        u32 parallelCount = 0;
        for (const NodeInfo& node : snap.Nodes)
        {
            Json entry{ { "name", node.Name },
                        { "orderIndex", node.OrderIndex },
                        { "parallel", node.Parallel },
                        { "reads", node.Reads },
                        { "writes", node.Writes } };
            // Only present when actually declared: After/Before are the exception
            // (a handful of documented orderings the read/write model cannot
            // express), and emitting empty arrays for every node would bury them.
            if (!node.After.empty())
                entry["after"] = node.After;
            if (!node.Before.empty())
                entry["before"] = node.Before;
            if (node.Parallel)
            {
                ++parallelCount;
                if (const auto it = peers.find(node.Name); it != peers.end())
                    entry["mayOverlapWith"] = it->second;
            }
            nodes.push_back(std::move(entry));
        }

        Json edges = Json::array();
        for (const EdgeInfo& edge : snap.Edges)
            edges.push_back(Json{ { "from", edge.From }, { "to", edge.To } });

        Json executionOrder = Json::array();
        for (const NodeInfo& node : snap.Nodes)
            executionOrder.push_back(node.Name);

        // Every channel any system touches, with its readers and writers — the
        // inverse index of the per-node lists, and the fastest way to spot the
        // classic authoring mistake: a channel name misspelled in one declaration
        // shows up here as a channel with (say) one writer and no readers.
        std::unordered_map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> byChannel;
        for (const NodeInfo& node : snap.Nodes)
        {
            for (const std::string& channel : node.Reads)
                byChannel[channel].first.push_back(node.Name);
            for (const std::string& channel : node.Writes)
                byChannel[channel].second.push_back(node.Name);
        }
        std::vector<std::string> channelNames;
        channelNames.reserve(byChannel.size());
        for (const auto& [name, users] : byChannel)
            channelNames.push_back(name);
        // Sorted: an unordered_map's iteration order is implementation-defined, and
        // two identical schedules must not export differently between runs.
        std::sort(channelNames.begin(), channelNames.end());

        Json channels = Json::array();
        for (const std::string& name : channelNames)
        {
            const auto& [readers, writers] = byChannel[name];
            channels.push_back(Json{ { "name", name }, { "readers", readers }, { "writers", writers } });
        }

        Json out;
        out["systemCount"] = static_cast<u32>(snap.Nodes.size());
        out["parallelSystemCount"] = parallelCount;
        out["parallelExecutionEnabled"] = snap.ParallelExecutionEnabled;
        out["executionOrder"] = std::move(executionOrder);
        out["systems"] = std::move(nodes);
        out["edgeCount"] = static_cast<u32>(snap.Edges.size());
        out["edges"] = std::move(edges);
        out["channelCount"] = static_cast<u32>(channels.size());
        out["channels"] = std::move(channels);
        out["note"] =
            "Derived dependency graph of the per-tick gameplay systems (Scene::GetGameplayScheduler). 'edges' are "
            "the DERIVED ordering constraints — from must finish before to — and include the read/write hazard "
            "edges (RAW/WAW/WAR over the named channels), not just the explicit after/before declarations. "
            "'executionOrder' is the topological order actually run, with registration order as the tie-break. "
            "'parallel' marks a system dispatched to a worker thread; an unmarked system is a join-all barrier, so "
            "a marked system only ever overlaps OTHER marked systems — 'mayOverlapWith' lists exactly which, and "
            "is the set that system's thread-safety audit must cover. A MISSING edge is invisible in the "
            "execution order (the registration tie-break supplies it anyway) and only becomes a data race under "
            "the parallel executor, which is why this export exists. "
            "Use format:\"mermaid\" or format:\"dot\" for a drawable DAG.";
        return out;
    }

    // ---- shared id assignment for the graph renderers -----------------------

    namespace Detail
    {
        // Stable synthetic ids (n0, n1, ...). System names are plain identifiers
        // today, but neither Mermaid nor DOT is required to accept an arbitrary
        // one, and the real name is always available as the quoted label.
        class IdMap
        {
          public:
            [[nodiscard]] std::string Of(const std::string& name)
            {
                if (const auto it = m_Ids.find(name); it != m_Ids.end())
                    return it->second;
                std::string id = "n" + std::to_string(m_Ids.size());
                m_Ids.emplace(name, id);
                return id;
            }

          private:
            std::unordered_map<std::string, std::string> m_Ids;
        };

        [[nodiscard]] inline std::string EscapeQuoted(const std::string& text, const char* quoteEscape)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                if (c == '"')
                    out += quoteEscape;
                else
                    out.push_back(c);
            }
            return out;
        }
    } // namespace Detail

    // ---- Mermaid ------------------------------------------------------------

    [[nodiscard]] inline std::string BuildMermaid(const Snapshot& snap)
    {
        Detail::IdMap ids;
        std::string out = "flowchart LR\n";

        // Nodes first, so a system with no edges still appears and still carries
        // its parallel styling, and the output is deterministic.
        bool anyParallel = false;
        for (const NodeInfo& node : snap.Nodes)
        {
            const std::string id = ids.Of(node.Name);
            std::string label = std::to_string(node.OrderIndex) + ". " + node.Name;
            if (node.Parallel)
            {
                label += " [par]";
                anyParallel = true;
            }
            out += "    " + id + "[\"" + Detail::EscapeQuoted(label, "&quot;") + "\"]\n";
        }

        for (const EdgeInfo& edge : snap.Edges)
            out += "    " + ids.Of(edge.From) + " --> " + ids.Of(edge.To) + "\n";

        if (anyParallel)
        {
            out += "    classDef parallel fill:#e3f2fd,stroke:#1565c0,stroke-width:2px;\n";
            for (const NodeInfo& node : snap.Nodes)
            {
                if (node.Parallel)
                    out += "    class " + ids.Of(node.Name) + " parallel;\n";
            }
        }
        return out;
    }

    // ---- Graphviz DOT -------------------------------------------------------

    [[nodiscard]] inline std::string BuildDot(const Snapshot& snap)
    {
        Detail::IdMap ids;
        std::string out = "digraph GameplaySchedule {\n";
        out += "    rankdir=LR;\n";
        out += "    node [shape=box, style=rounded, fontname=\"sans-serif\"];\n";

        for (const NodeInfo& node : snap.Nodes)
        {
            std::string label = std::to_string(node.OrderIndex) + ". " + node.Name;
            out += "    " + ids.Of(node.Name) + " [label=\"" + Detail::EscapeQuoted(label, "\\\"") + "\"";
            if (node.Parallel)
                out += ", style=\"rounded,filled\", fillcolor=\"#e3f2fd\", color=\"#1565c0\"";
            out += "];\n";
        }

        for (const EdgeInfo& edge : snap.Edges)
            out += "    " + ids.Of(edge.From) + " -> " + ids.Of(edge.To) + ";\n";

        out += "}\n";
        return out;
    }
} // namespace OloEngine::MCP::SchedulerGraph
