#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

namespace OloEngine
{
    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing RenderGraph with dimensions: {}x{}", width, height);

        // Initialize all passes
        for (auto& [name, pass] : m_PassLookup)
        {
            pass->SetupFramebuffer(width, height);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down RenderGraph");

        // Clear all pass references
        m_PassLookup.clear();
        m_Dependencies.clear();
        m_FramebufferConnections.clear();
        m_InsertionOrder.clear();
        m_PassOrder.clear();
        m_FinalPassName.clear();
    }

    void RenderGraph::AddPass(const Ref<RenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();

        std::string name = pass->GetName();
        OLO_CORE_INFO("Adding RenderPass to graph: {}", name);

        // Track insertion order so topological sort has a stable tie-break
        // when multiple passes have no dependencies between them. Without
        // this, the unordered_map iteration order leaks into m_PassOrder
        // and varies between platforms/standard-library versions, causing
        // the hazard validator to classify RAW vs WAR differently.
        if (!m_PassLookup.contains(name))
            m_InsertionOrder.push_back(name);
        m_PassLookup[name] = pass;
        m_DependencyGraphDirty = true;
    }

    void RenderGraph::ConnectPass(const std::string& outputPass, const std::string& inputPass)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_PassLookup.contains(outputPass))
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Output pass '{}' not found!", outputPass);
            return;
        }

        if (!m_PassLookup.contains(inputPass))
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Input pass '{}' not found!", inputPass);
            return;
        }

        OLO_CORE_INFO("Connecting passes (with framebuffer piping): {} -> {}", outputPass, inputPass);

        // Add dependency for execution ordering (avoid duplicates)
        auto& deps = m_Dependencies[inputPass];
        if (std::find(deps.begin(), deps.end(), outputPass) == deps.end())
        {
            deps.push_back(outputPass);
        }
        // Mark for framebuffer piping (avoid duplicates)
        auto& conns = m_FramebufferConnections[outputPass];
        if (std::find(conns.begin(), conns.end(), inputPass) == conns.end())
        {
            conns.push_back(inputPass);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::AddExecutionDependency(const std::string& beforePass, const std::string& afterPass)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_PassLookup.contains(beforePass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", beforePass);
            return;
        }

        if (!m_PassLookup.contains(afterPass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", afterPass);
            return;
        }

        OLO_CORE_INFO("Adding execution dependency (ordering only): {} -> {}", beforePass, afterPass);

        // Only add dependency for execution ordering, no framebuffer piping (avoid duplicates)
        auto& deps = m_Dependencies[afterPass];
        if (std::find(deps.begin(), deps.end(), beforePass) == deps.end())
        {
            deps.push_back(beforePass);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Cycle detected — m_PassOrder is empty. Abort execution;
                // running with a partial topo order would execute the wrong
                // subset of passes in the wrong order. Keep the dirty flag
                // set so a corrected graph can retry.
                OLO_CORE_ERROR("RenderGraph::Execute: aborting because dependency graph rebuild failed");
                return;
            }
            ResolveFinalPass();
            RebuildExecutionCache();
            m_DependencyGraphDirty = false;
        }

        // First pass: Connect framebuffers between passes that use framebuffer piping
        for (const auto& pipe : m_CachedPipes)
        {
            Ref<Framebuffer> outputFramebuffer = pipe.OutputPass->GetTarget();
            if (outputFramebuffer)
            {
                for (auto* inputPass : pipe.InputPasses)
                {
                    inputPass->SetInputFramebuffer(outputFramebuffer);
                }
            }
        }

        // Second pass: Execute passes in order (zero-overhead — no lookups, no branching)
        for (auto* pass : m_CachedExecutionOrder)
        {
            pass->Execute();
        }
    }

    void RenderGraph::RebuildExecutionCache()
    {
        OLO_PROFILE_FUNCTION();

        // Build framebuffer piping cache with validation
        m_CachedPipes.clear();
        for (const auto& [outputPassName, inputPassNames] : m_FramebufferConnections)
        {
            auto outputIt = m_PassLookup.find(outputPassName);
            if (outputIt == m_PassLookup.end() || !outputIt->second)
            {
                OLO_CORE_ERROR("RenderGraph: Output pass '{}' not found or null in framebuffer piping — skipping connection", outputPassName);
                continue;
            }

            FramebufferPipe pipe;
            pipe.OutputPass = outputIt->second.Raw();

            for (const auto& inputPassName : inputPassNames)
            {
                auto inputIt = m_PassLookup.find(inputPassName);
                if (inputIt == m_PassLookup.end() || !inputIt->second)
                {
                    OLO_CORE_ERROR("RenderGraph: Input pass '{}' not found or null in framebuffer piping — skipping", inputPassName);
                    continue;
                }
                pipe.InputPasses.push_back(inputIt->second.Raw());
            }

            m_CachedPipes.push_back(std::move(pipe));
        }

        // Build execution order cache with validation
        m_CachedExecutionOrder.clear();
        m_CachedExecutionOrder.reserve(m_PassOrder.size());

        for (const auto& passName : m_PassOrder)
        {
            auto it = m_PassLookup.find(passName);
            if (it == m_PassLookup.end() || !it->second)
            {
                OLO_CORE_ERROR("RenderGraph: Pass '{}' is null or missing from lookup — it will be skipped during execution!", passName);
                continue;
            }
            m_CachedExecutionOrder.push_back(it->second.Raw());
        }

        OLO_CORE_INFO("RenderGraph: Execution cache rebuilt — {} passes, {} framebuffer pipes",
                      m_CachedExecutionOrder.size(), m_CachedPipes.size());
    }

    void RenderGraph::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        for (auto& [name, pass] : m_PassLookup)
        {
            pass->ResizeFramebuffer(width, height);
        }
    }

    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        m_FinalPassName = passName;
    }

    std::vector<Ref<RenderPass>> RenderGraph::GetAllPasses() const
    {
        std::vector<Ref<RenderPass>> result;
        result.reserve(m_PassLookup.size());
        for (const auto& [name, pass] : m_PassLookup)
        {
            result.push_back(pass);
        }
        return result;
    }

    bool RenderGraph::IsFinalPass(const std::string& passName) const
    {
        return passName == m_FinalPassName;
    }

    std::vector<RenderGraph::ConnectionInfo> RenderGraph::GetConnections() const
    {
        std::vector<ConnectionInfo> result;
        for (const auto& [input, outputs] : m_Dependencies)
        {
            for (const auto& output : outputs)
            {
                result.push_back({ output, input, 0 });
            }
        }
        return result;
    }

    bool RenderGraph::UpdateDependencyGraph()
    {
        OLO_PROFILE_FUNCTION();

        m_PassOrder.clear();

        // Topological sort to determine execution order
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> inProgress;

        std::function<bool(const std::string&)> visit = [&](const std::string& node)
        {
            if (inProgress.contains(node))
            {
                OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Cycle detected in graph!");
                return false;
            }

            if (visited.contains(node))
            {
                return true;
            }

            inProgress.insert(node);

            if (m_Dependencies.contains(node))
            {
                for (const auto& dep : m_Dependencies[node])
                {
                    if (!visit(dep))
                        return false;
                }
            }

            visited.insert(node);
            inProgress.erase(node);
            m_PassOrder.push_back(node);

            return true;
        };

        // Visit all nodes in insertion order so ties are broken
        // deterministically (independent of std::unordered_map hashing).
        for (const auto& name : m_InsertionOrder)
        {
            if (!m_PassLookup.contains(name))
                continue;
            if (!visited.contains(name))
            {
                if (!visit(name))
                {
                    OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Failed to build execution order!");
                    m_PassOrder.clear();
                    return false;
                }
            }
        }

        OLO_CORE_INFO("RenderGraph execution order updated with {} passes", m_PassOrder.size());
        return true;
    }

    void RenderGraph::ResolveFinalPass()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FinalPassName.empty())
        {
            // If no final pass was explicitly set, try to find a pass with no dependents
            // Iterate m_PassOrder (deterministic) instead of m_PassLookup (unordered_map)
            for (auto it = m_PassOrder.rbegin(); it != m_PassOrder.rend(); ++it)
            {
                const auto& name = *it;
                if (!m_FramebufferConnections.contains(name) ||
                    m_FramebufferConnections[name].empty())
                {
                    m_FinalPassName = name;
                    OLO_CORE_INFO("RenderGraph: Auto-selected final pass: {}", name);
                    break;
                }
            }
        }

        if (m_FinalPassName.empty())
        {
            OLO_CORE_WARN("RenderGraph: Could not determine final pass!");
        }
    }

    // =========================================================================
    // Resource-aware hazard validation
    // =========================================================================
    // Algorithm: walk passes in topological order, maintain a per-resource
    // state (last writer + live readers since that writer), and for every
    // declared access check that the current pass transitively depends on
    // any prior conflicting accesses.
    //
    // Transitive closure is built on-demand from m_Dependencies (which holds
    // every edge — ConnectPass + AddExecutionDependency both append here).
    std::vector<RenderGraph::Hazard> RenderGraph::ValidateResourceHazards()
    {
        OLO_PROFILE_FUNCTION();

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Partial topo order is useless — running the remaining
                // validator over an incomplete m_PassOrder would produce
                // misleading "missing dependency" reports for passes the
                // cycle excluded. Surface a synthetic Cycle hazard so
                // callers can distinguish "no hazards" from "could not
                // validate" (the empty-vector overload used to conflate
                // both).
                OLO_CORE_ERROR("RenderGraph::ValidateResourceHazards: aborting (graph has a cycle)");
                Hazard h;
                h.Kind = HazardKind::Cycle;
                h.Message = "RenderGraph contains a cycle; resource hazard validation aborted";
                return { std::move(h) };
            }
            // Note: we deliberately leave m_DependencyGraphDirty set so the
            // first Execute() after validation still runs ResolveFinalPass +
            // RebuildExecutionCache. The validator only needs topo order.
        }

        std::vector<Hazard> hazards;

        // Build transitive dependency closure: for each pass P, closure[P]
        // is the set of all passes that must execute before P.
        std::unordered_map<std::string, std::unordered_set<std::string>> closure;
        closure.reserve(m_PassOrder.size());
        for (const auto& passName : m_PassOrder)
        {
            std::unordered_set<std::string>& cls = closure[passName];
            std::vector<std::string> frontier;
            auto depsIt = m_Dependencies.find(passName);
            if (depsIt != m_Dependencies.end())
            {
                frontier.insert(frontier.end(), depsIt->second.begin(), depsIt->second.end());
            }
            while (!frontier.empty())
            {
                const std::string parent = std::move(frontier.back());
                frontier.pop_back();
                if (!cls.insert(parent).second)
                    continue;
                auto parentDeps = m_Dependencies.find(parent);
                if (parentDeps == m_Dependencies.end())
                    continue;
                for (const auto& grand : parentDeps->second)
                {
                    if (!cls.contains(grand))
                        frontier.push_back(grand);
                }
            }
        }

        auto dependsOn = [&closure](const std::string& later, const std::string& earlier) -> bool
        {
            auto it = closure.find(later);
            if (it == closure.end())
                return false;
            return it->second.contains(earlier);
        };

        struct ResourceState
        {
            std::string LastWriter;                      // empty => never written
            std::unordered_set<std::string> LiveReaders; // readers since lastWriter
        };
        std::unordered_map<std::string, ResourceState> state;

        for (const auto& passName : m_PassOrder)
        {
            auto passIt = m_PassLookup.find(passName);
            if (passIt == m_PassLookup.end() || !passIt->second)
                continue;
            const RenderPass& pass = *passIt->second;

            // Record reads first — a same-pass read+write on the same
            // resource isn't a hazard against itself.
            for (const ResourceHandle& r : pass.GetReads())
            {
                ResourceState& st = state[r.Name];
                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::ReadAfterWrite;
                    h.Resource = r.Name;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "RAW: pass '" + passName + "' reads resource '" + r.Name +
                                "' written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                }
                st.LiveReaders.insert(passName);
            }

            for (const ResourceHandle& w : pass.GetWrites())
            {
                ResourceState& st = state[w.Name];

                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::WriteAfterWrite;
                    h.Resource = w.Name;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "WAW: pass '" + passName + "' writes resource '" + w.Name +
                                "' previously written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                }

                for (const auto& reader : st.LiveReaders)
                {
                    if (reader == passName)
                        continue;
                    if (!dependsOn(passName, reader))
                    {
                        Hazard h;
                        h.Kind = HazardKind::WriteAfterRead;
                        h.Resource = w.Name;
                        h.Producer = reader;
                        h.Consumer = passName;
                        h.Message = "WAR: pass '" + passName + "' overwrites resource '" + w.Name +
                                    "' still live for reader '" + reader +
                                    "' without declaring a dependency";
                        OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                        hazards.push_back(std::move(h));
                    }
                }

                st.LastWriter = passName;
                st.LiveReaders.clear();
            }
        }

        return hazards;
    }
} // namespace OloEngine
