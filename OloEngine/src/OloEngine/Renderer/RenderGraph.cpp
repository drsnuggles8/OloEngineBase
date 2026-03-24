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
        m_PassOrder.clear();
        m_FinalPassName.clear();
    }

    void RenderGraph::AddPass(const Ref<RenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();

        std::string name = pass->GetName();
        OLO_CORE_INFO("Adding RenderPass to graph: {}", name);

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
            UpdateDependencyGraph();
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

    void RenderGraph::UpdateDependencyGraph()
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

        // Visit all nodes
        for (const auto& [name, _] : m_PassLookup)
        {
            if (!visited.contains(name))
            {
                if (!visit(name))
                {
                    OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Failed to build execution order!");
                    return;
                }
            }
        }

        OLO_CORE_INFO("RenderGraph execution order updated with {} passes", m_PassOrder.size());
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
} // namespace OloEngine
