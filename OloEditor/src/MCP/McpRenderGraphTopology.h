#pragma once

// Pure, engine-light topology export for the olo_render_graph_topology_export MCP
// tool (issue #316 Part 4, "LLM-analysis exports"). The MCP handler in
// McpTools.cpp reads the live RenderGraph on the editor main thread, fills the
// engine-free Snapshot below, and hands it here to be turned into the JSON (or a
// Mermaid DAG) an agent can reason about: the render passes, their topologically-
// sorted execution order, the pass-dependency edges, and every registered resource
// with the passes that produce / consume it.
//
// Keeping the shaping in free functions that touch ONLY the plain Snapshot (no
// RenderGraph / renderer / editor / GPU types — the handler pre-resolves every enum
// to a string) means it unit-tests directly against a synthetic graph: the test
// binary compiles this header but deliberately NOT McpTools.cpp (the editor-backed
// handler). This mirrors the sibling pattern of McpFrameBreakdown.h /
// McpRenderExplain.h, which the MCP test target links header-only.
//
// Only OloEngine/Core/Base.h is pulled in (for the integer typedefs); everything
// else is the standard library + nlohmann::json.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::MCP::RenderGraphTopology
{
    using Json = nlohmann::json;

    // One render pass / graph node. WorkType is pre-resolved by the handler to a
    // string ("Graphics" | "Compute" | "Copy") so this header stays free of the
    // RenderGraphPassWorkType enum.
    struct PassInfo
    {
        std::string Name;
        std::string WorkType = "Graphics";
        bool DeclaresResources = false;     // pass declared at least one resource read/write
        bool AsyncComputeCandidate = false; // scheduler may hoist onto a compute queue
        bool Culled = false;                // unreachable from the final pass this frame
        bool IsFinalPass = false;           // the graph's designated final/output pass
    };

    // One dependency edge: From must execute before To. The render graph's public
    // edge list (RenderGraph::GetConnections) is execution-ordering only and does
    // not distinguish framebuffer piping (ConnectPass) from ordering-only deps
    // (AddExecutionDependency), so no edge kind is exposed here.
    struct EdgeInfo
    {
        std::string From;
        std::string To;
    };

    // One registered graph resource (texture / framebuffer / buffer) and the passes
    // that produce / consume it.
    struct ResourceInfo
    {
        std::string Name;
        std::string Kind;   // "Texture2D" | "Framebuffer" | "UniformBuffer" | ...
        std::string Format; // empty when Unknown / not a typed image
        u32 Width = 0;
        u32 Height = 0;
        u32 Samples = 1;
        bool Imported = false;           // entered via ImportTexture/ImportFramebuffer/ImportBuffer
        bool HasExternalBacking = false; // resolves to caller-supplied frame-local backing
        std::vector<std::string> Producers;
        std::vector<std::string> Consumers;
    };

    // The whole-graph snapshot the handler gathers off the live RenderGraph.
    struct Snapshot
    {
        std::vector<PassInfo> Passes;
        std::vector<std::string> ExecutionOrder;
        std::vector<EdgeInfo> Edges;
        std::vector<ResourceInfo> Resources;
        std::string FinalPass;
    };

    // Shape the snapshot as the structured JSON document the tool returns by
    // default. Counts are reported alongside each array so a truncating client can
    // tell the full size, and a trailing `note` documents the edge / culled / final
    // semantics for the reader.
    [[nodiscard]] inline Json BuildJson(const Snapshot& snap)
    {
        Json passes = Json::array();
        for (const auto& p : snap.Passes)
        {
            passes.push_back(Json{ { "name", p.Name },
                                   { "workType", p.WorkType },
                                   { "declaresResources", p.DeclaresResources },
                                   { "asyncComputeCandidate", p.AsyncComputeCandidate },
                                   { "culled", p.Culled },
                                   { "isFinalPass", p.IsFinalPass } });
        }

        Json edges = Json::array();
        for (const auto& edge : snap.Edges)
            edges.push_back(Json{ { "from", edge.From }, { "to", edge.To } });

        Json resources = Json::array();
        for (const auto& r : snap.Resources)
        {
            Json e;
            e["name"] = r.Name;
            e["kind"] = r.Kind;
            if (!r.Format.empty())
                e["format"] = r.Format;
            if (r.Width > 0 && r.Height > 0)
            {
                e["width"] = r.Width;
                e["height"] = r.Height;
            }
            if (r.Samples > 1)
                e["samples"] = r.Samples;
            e["imported"] = r.Imported;
            e["hasExternalBacking"] = r.HasExternalBacking;
            e["producers"] = r.Producers;
            e["consumers"] = r.Consumers;
            resources.push_back(std::move(e));
        }

        Json out;
        out["finalPass"] = snap.FinalPass;
        out["passCount"] = static_cast<u32>(snap.Passes.size());
        out["passes"] = std::move(passes);
        out["executionOrder"] = snap.ExecutionOrder;
        out["edgeCount"] = static_cast<u32>(snap.Edges.size());
        out["edges"] = std::move(edges);
        out["resourceCount"] = static_cast<u32>(snap.Resources.size());
        out["resources"] = std::move(resources);
        out["note"] = "Live topology of the active render graph for the current rendering path. 'edges' are "
                      "execution-ordering dependencies (from must run before to). 'executionOrder' is the "
                      "topologically-sorted run order. 'culled' passes were unreachable from the final pass "
                      "this frame and are skipped. Each resource's 'producers' write it and 'consumers' read "
                      "it. Use format:\"mermaid\" for a flowchart DAG of the pass graph.";
        return out;
    }

    // Shape the snapshot as a Mermaid `flowchart LR` of the pass graph. Pass names
    // can contain '@', spaces and other characters Mermaid rejects in a node id, so
    // each pass is assigned a stable synthetic id (n0, n1, ...) and the real name is
    // only ever used as the quoted label. The final pass and any culled passes get a
    // classDef so the diagram reads at a glance.
    [[nodiscard]] inline std::string BuildMermaid(const Snapshot& snap)
    {
        std::unordered_map<std::string, std::string> idByName;
        // Return by value (never a reference into the map): a later idOf() can insert
        // and rehash, which would dangle a reference held across the call.
        const auto idOf = [&idByName](const std::string& name) -> std::string
        {
            if (const auto it = idByName.find(name); it != idByName.end())
                return it->second;
            std::string id = "n" + std::to_string(idByName.size());
            idByName.emplace(name, id);
            return id;
        };

        // Escape a label for a Mermaid quoted string ("...").
        const auto escapeLabel = [](const std::string& s) -> std::string
        {
            std::string r;
            r.reserve(s.size());
            for (const char c : s)
            {
                if (c == '"')
                    r += "&quot;";
                else
                    r.push_back(c);
            }
            return r;
        };

        std::string out = "flowchart LR\n";

        // Declare every pass node up front (before edges) so a culled / final pass
        // carries its style even with no edges, and the output is deterministic.
        bool anyCulled = false;
        bool anyFinal = false;
        for (const auto& p : snap.Passes)
        {
            const std::string id = idOf(p.Name);
            std::string label = p.Name;
            if (p.WorkType != "Graphics")
                label += " [" + p.WorkType + "]";
            out += "    " + id + "[\"" + escapeLabel(label) + "\"]\n";
            anyCulled = anyCulled || p.Culled;
            anyFinal = anyFinal || p.IsFinalPass;
        }

        for (const auto& edge : snap.Edges)
        {
            const std::string from = idOf(edge.From);
            const std::string to = idOf(edge.To);
            out += "    " + from + " --> " + to + "\n";
        }

        if (anyFinal)
            out += "    classDef finalPass fill:#d4edda,stroke:#2e7d32;\n";
        if (anyCulled)
            out += "    classDef culled fill:#f2f2f2,stroke:#9e9e9e,stroke-dasharray:4 3;\n";
        for (const auto& p : snap.Passes)
        {
            if (p.IsFinalPass)
                out += "    class " + idOf(p.Name) + " finalPass;\n";
            else if (p.Culled)
                out += "    class " + idOf(p.Name) + " culled;\n";
        }

        return out;
    }
} // namespace OloEngine::MCP::RenderGraphTopology
