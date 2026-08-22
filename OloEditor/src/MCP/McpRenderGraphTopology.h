#pragma once

// Pure, engine-light topology export for the olo_render_graph_topology_export MCP
// tool (issue #316, "LLM-analysis exports"). The MCP handler in
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
#include <string_view>
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

        // Resolved physical GL object ids, as of the last executed frame
        // (issue #607) — the one-call answer to "do these two passes touch
        // the same physical texture this frame". 0 = unbacked / not that
        // kind. Texture VIEWS resolve to their PARENT texture object;
        // ViewOfParentLayer carries the layer a layer/face view addresses.
        // Transient ids are only meaningful within the frame they were
        // resolved in (the transient pool memory-aliases across frames).
        u32 GLTextureId = 0;
        u32 GLFramebufferId = 0;
        u32 GLBufferId = 0;
        u32 GLDepthAttachmentId = 0;
        std::vector<u32> GLColorAttachmentIds;
        u32 ViewOfParentLayer = 0;
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
    // The "gl" sub-object of one resource: every resolved id that is set.
    // Returns an empty (null) Json when the resource has no resolved backing.
    [[nodiscard]] inline Json ResourceGLJson(const ResourceInfo& r)
    {
        Json gl;
        if (r.GLTextureId != 0)
            gl["textureId"] = r.GLTextureId;
        if (r.GLFramebufferId != 0)
            gl["framebufferId"] = r.GLFramebufferId;
        if (!r.GLColorAttachmentIds.empty())
            gl["colorAttachmentIds"] = r.GLColorAttachmentIds;
        if (r.GLDepthAttachmentId != 0)
            gl["depthAttachmentId"] = r.GLDepthAttachmentId;
        if (r.GLBufferId != 0)
            gl["bufferId"] = r.GLBufferId;
        if (r.ViewOfParentLayer != 0)
            gl["viewOfParentLayer"] = r.ViewOfParentLayer;
        return gl;
    }

    // The physical texture id a pass ACCESSES through this resource: the
    // resolved texture for texture kinds, otherwise the framebuffer's first
    // colour attachment (or depth attachment for depth-only targets) — the
    // same physical-identity rule the capture tools use. 0 when unbacked.
    [[nodiscard]] inline u32 AccessedPhysicalTextureId(const ResourceInfo& r)
    {
        if (r.GLTextureId != 0)
            return r.GLTextureId;
        if (!r.GLColorAttachmentIds.empty() && r.GLColorAttachmentIds.front() != 0)
            return r.GLColorAttachmentIds.front();
        return r.GLDepthAttachmentId;
    }

    [[nodiscard]] inline Json BuildJson(const Snapshot& snap)
    {
        // Per-pass access lists (issue #607): invert each resource's
        // producers/consumers so a pass entry lists every resource it writes
        // or reads WITH the resolved physical id — "do these two passes touch
        // the same physical texture this frame" becomes a single lookup.
        std::unordered_map<std::string, Json> accessesByPass;
        const auto appendAccess = [&accessesByPass](const std::string& pass, const ResourceInfo& r, const char* mode)
        {
            Json access;
            access["resource"] = r.Name;
            access["mode"] = mode;
            if (const u32 physicalId = AccessedPhysicalTextureId(r); physicalId != 0)
                access["glTextureId"] = physicalId;
            if (r.GLBufferId != 0)
                access["glBufferId"] = r.GLBufferId;
            auto [it, inserted] = accessesByPass.try_emplace(pass, Json::array());
            it->second.push_back(std::move(access));
        };
        for (const auto& r : snap.Resources)
        {
            for (const auto& producer : r.Producers)
                appendAccess(producer, r, "write");
            for (const auto& consumer : r.Consumers)
                appendAccess(consumer, r, "read");
        }

        Json passes = Json::array();
        for (const auto& p : snap.Passes)
        {
            Json pass = Json{ { "name", p.Name },
                              { "workType", p.WorkType },
                              { "declaresResources", p.DeclaresResources },
                              { "asyncComputeCandidate", p.AsyncComputeCandidate },
                              { "culled", p.Culled },
                              { "isFinalPass", p.IsFinalPass } };
            if (const auto it = accessesByPass.find(p.Name); it != accessesByPass.end())
                pass["accesses"] = it->second;
            passes.push_back(std::move(pass));
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
            if (Json gl = ResourceGLJson(r); !gl.is_null() && !gl.empty())
                e["gl"] = std::move(gl);
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
                      "it. Each resource's 'gl' (and each pass access's glTextureId) is the RESOLVED physical "
                      "GL object id as of the last executed frame — two passes whose accesses share an id "
                      "touch the same physical texture; texture views resolve to their parent object "
                      "(see viewOfParentLayer); transient ids are only meaningful within one frame. "
                      "Use format:\"mermaid\" for a flowchart DAG of the pass graph.";
        return out;
    }

    // ---- shared graph-rendering helpers ------------------------------------
    // The Mermaid and DOT renderers below differ only in syntax and in how they
    // escape a quote; everything else — id assignment, label construction — is the
    // same job twice. Mirrors Detail::IdMap / Detail::EscapeQuoted in
    // McpSchedulerGraph.h so the engine's two DAG exports stay recognisably one
    // design.
    namespace Detail
    {
        // Stable synthetic ids (n0, n1, ...). Pass names contain '@', spaces and
        // other characters neither format accepts in a node id, so the real name is
        // only ever used as the quoted label.
        class IdMap
        {
          public:
            // Returns BY VALUE, never a reference into the map: a later Of() can
            // insert and rehash, dangling a reference held across the call.
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

        // Escape a label for a quoted string in either format. `quoteEscape` is the
        // format's replacement for '"' ("&quot;" for Mermaid, "\\\"" for DOT).
        //
        // The backslash is doubled FIRST, and that order is load-bearing for DOT:
        // escaping only the quote would turn a name ending in '\' into '\\"', whose
        // second backslash escapes the closing quote and swallows the rest of the
        // line. Escaping a quote but not the character that escapes it is worse than
        // escaping neither.
        [[nodiscard]] inline std::string EscapeQuoted(const std::string& text, std::string_view quoteEscape,
                                                      bool escapeBackslash)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                if (c == '\\' && escapeBackslash)
                    out += "\\\\";
                else if (c == '"')
                    out += quoteEscape;
                else
                    out.push_back(c);
            }
            return out;
        }

        // A pass's display label: its name, plus the work type when it is not the
        // default Graphics (the one thing worth seeing at a glance in a diagram).
        [[nodiscard]] inline std::string PassLabel(const PassInfo& pass)
        {
            std::string label = pass.Name;
            if (pass.WorkType != "Graphics")
                label += " [" + pass.WorkType + "]";
            return label;
        }
    } // namespace Detail

    // Shape the snapshot as a Mermaid `flowchart LR` of the pass graph. The final
    // pass and any culled passes get a classDef so the diagram reads at a glance.
    [[nodiscard]] inline std::string BuildMermaid(const Snapshot& snap)
    {
        Detail::IdMap ids;
        const auto idOf = [&ids](const std::string& name)
        { return ids.Of(name); };
        // Mermaid takes the HTML entity, and its labels are not backslash-escaped.
        const auto escapeLabel = [](const std::string& s)
        { return Detail::EscapeQuoted(s, "&quot;", /*escapeBackslash*/ false); };

        std::string out = "flowchart LR\n";

        // Declare every pass node up front (before edges) so a culled / final pass
        // carries its style even with no edges, and the output is deterministic.
        bool anyCulled = false;
        bool anyFinal = false;
        for (const auto& p : snap.Passes)
        {
            const std::string id = idOf(p.Name);
            out += "    " + id + "[\"" + escapeLabel(Detail::PassLabel(p)) + "\"]\n";
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

    // Shape the snapshot as Graphviz DOT (issue #607) — the sibling format of the
    // Mermaid renderer above, added so both of the engine's DAGs (this and the
    // gameplay SystemScheduler, see McpSchedulerGraph.h) can be exported in the same
    // two drawable formats. Same synthetic-id and quoted-label treatment; only the
    // quote escape differs (DOT takes a backslash escape, Mermaid an HTML entity).
    [[nodiscard]] inline std::string BuildDot(const Snapshot& snap)
    {
        Detail::IdMap ids;
        const auto idOf = [&ids](const std::string& name)
        { return ids.Of(name); };
        // DOT takes a backslash escape for the quote — and therefore must also
        // double a literal backslash, or a name ending in one would escape the
        // closing quote. See Detail::EscapeQuoted.
        const auto escapeLabel = [](const std::string& s)
        { return Detail::EscapeQuoted(s, "\\\"", /*escapeBackslash*/ true); };

        std::string out = "digraph RenderGraph {\n";
        out += "    rankdir=LR;\n";
        out += "    node [shape=box, style=rounded, fontname=\"sans-serif\"];\n";

        for (const auto& p : snap.Passes)
        {
            out += "    " + idOf(p.Name) + " [label=\"" + escapeLabel(Detail::PassLabel(p)) + "\"";
            if (p.IsFinalPass)
                out += ", style=\"rounded,filled\", fillcolor=\"#d4edda\", color=\"#2e7d32\"";
            else if (p.Culled)
                out += ", style=\"rounded,dashed,filled\", fillcolor=\"#f2f2f2\", color=\"#9e9e9e\"";
            out += "];\n";
        }

        for (const auto& edge : snap.Edges)
            out += "    " + idOf(edge.From) + " -> " + idOf(edge.To) + ";\n";

        out += "}\n";
        return out;
    }
} // namespace OloEngine::MCP::RenderGraphTopology
