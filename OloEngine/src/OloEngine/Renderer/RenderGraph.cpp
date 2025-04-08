#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

namespace OloEngine
{
    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing RenderGraph with dimensions: {}x{}", width, height);
        
        // Initialize all passes
        for (auto& [name, passVariant] : m_PassLookup)
        {
            if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant))
            {
                (*renderPass)->SetupFramebuffer(width, height);
            }
            else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant))
            {
                (*cmdRenderPass)->SetupFramebuffer(width, height);
            }
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
        m_DependentPasses.clear();
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
    
    void RenderGraph::AddPass(const Ref<CommandRenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();
        
        std::string name = pass->GetName();
        OLO_CORE_INFO("Adding CommandRenderPass to graph: {}", name);
        
        m_PassLookup[name] = pass;
        m_DependencyGraphDirty = true;
    }
    
    void RenderGraph::ConnectPass(const std::string& outputPass, const std::string& inputPass, u32 outputAttachment)
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_PassLookup.find(outputPass) == m_PassLookup.end())
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Output pass '{}' not found!", outputPass);
            return;
        }
        
        if (m_PassLookup.find(inputPass) == m_PassLookup.end())
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Input pass '{}' not found!", inputPass);
            return;
        }
        
        OLO_CORE_INFO("Connecting passes: {} -> {}", outputPass, inputPass);
        
        // Add dependency
        m_Dependencies[inputPass].push_back(outputPass);
        m_DependentPasses[outputPass].push_back(inputPass);
        
        m_DependencyGraphDirty = true;
    }
    
    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_DependencyGraphDirty)
        {
            UpdateDependencyGraph();
            ResolveFinalPass();
            m_DependencyGraphDirty = false;
        }
        
        for (const auto& passName : m_PassOrder)
        {
            auto& passVariant = m_PassLookup[passName];
            
            if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant))
            {
                OLO_CORE_TRACE("Executing RenderPass: {}", passName);
                (*renderPass)->Execute();
            }
            else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant))
            {
                OLO_CORE_TRACE("Executing CommandRenderPass: {}", passName);
                (*cmdRenderPass)->Execute();
            }
        }
    }
    
    void RenderGraph::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Resizing RenderGraph to {}x{}", width, height);
        
        for (auto& [name, passVariant] : m_PassLookup)
        {
            if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant))
            {
                (*renderPass)->ResizeFramebuffer(width, height);
            }
            else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant))
            {
                (*cmdRenderPass)->ResizeFramebuffer(width, height);
            }
        }
    }
    
    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_PassLookup.find(passName) == m_PassLookup.end())
        {
            OLO_CORE_ERROR("RenderGraph::SetFinalPass: Pass '{}' not found!", passName);
            return;
        }
        
        OLO_CORE_INFO("Setting final pass: {}", passName);
        m_FinalPassName = passName;
        m_DependencyGraphDirty = true;
    }
    
    void RenderGraph::UpdateDependencyGraph()
    {
        OLO_PROFILE_FUNCTION();
        
        m_PassOrder.clear();
        
        // Topological sort to determine execution order
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> inProgress;
        
        std::function<bool(const std::string&)> visit = [&](const std::string& node) {
            if (inProgress.find(node) != inProgress.end())
            {
                OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Cycle detected in graph!");
                return false;
            }
            
            if (visited.find(node) != visited.end())
            {
                return true;
            }
            
            inProgress.insert(node);
            
            if (m_Dependencies.find(node) != m_Dependencies.end())
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
            if (visited.find(name) == visited.end())
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
            for (const auto& [name, _] : m_PassLookup)
            {
                if (m_DependentPasses.find(name) == m_DependentPasses.end() || 
                    m_DependentPasses[name].empty())
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
}