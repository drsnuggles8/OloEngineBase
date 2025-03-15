#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"

namespace OloEngine
{
    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing RenderGraph with dimensions: {}x{}", width, height);

        // Store dimensions for future passes
        m_Width = width;
        m_Height = height;
        
        // Clear any existing data if reinitialized
        m_Passes.clear();
        m_PassLookup.clear();
        m_PassConnections.clear();
        m_ExplicitFinalPassName.clear();
        m_FinalPassName.clear();
        
        // Clear dependency tracking
        m_Dependencies.clear();
        m_DependentPasses.clear();
        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down RenderGraph");

        // Clear all collections
        m_Passes.clear();
        m_PassLookup.clear();
        m_PassConnections.clear();
        m_FinalPassName.clear();
    }

    void RenderGraph::AddPass(const Ref<RenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!pass)
        {
            OLO_CORE_ERROR("RenderGraph::AddPass: Attempted to add null pass!");
            return;
        }
        
        const auto& name = pass->GetName();
        
        if (m_PassLookup.find(name) != m_PassLookup.end())
        {
            OLO_CORE_WARN("RenderGraph::AddPass: Pass with name '{}' already exists, overwriting", name);
        }
        
        OLO_CORE_INFO("Adding render pass: {}", name);

        // Store the pass in our collections
        m_Passes.push_back(pass);
        m_PassLookup[name] = pass;

        // Initialize dependency tracking
        m_Dependencies[name] = {};
        m_DependentPasses[name] = {};
        
        // Set up the framebuffer for this pass
        pass->SetupFramebuffer(m_Width, m_Height);
        
        // Mark the dependency graph as dirty
        m_DependencyGraphDirty = true;
    }

    Ref<RenderPass> RenderGraph::GetPass(const std::string& name)
    {
        auto it = m_PassLookup.find(name);
        if (it != m_PassLookup.end())
            return it->second;

        OLO_CORE_WARN("RenderGraph::GetPass: No pass found with name: {}", name);
        return nullptr;
    }

    void RenderGraph::ConnectPass(const std::string& outputPass, const std::string& inputPass)
    {
        OLO_PROFILE_FUNCTION();
        
        if (outputPass == inputPass)
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Cannot connect a pass to itself! {} -> {}", 
                           outputPass, inputPass);
            return;
        }
        
        OLO_CORE_INFO("Connecting passes: {} -> {}", outputPass, inputPass);

        // Store the connection
        m_PassConnections[inputPass] = outputPass;

        // Get the passes
        auto output = GetPass(outputPass);
        auto input = GetPass(inputPass);

        if (!output || !input)
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Could not find passes! Output: {}, Input: {}", 
                           outputPass, inputPass);
            return;
        }

        // If the input pass is a FinalRenderPass, we need to set its input framebuffer
        if (auto finalPass = std::dynamic_pointer_cast<FinalRenderPass>(input))
        {
            finalPass->SetInputFramebuffer(output->GetTarget());
        }
        
        // Mark the dependency graph as dirty
        m_DependencyGraphDirty = true;
    }

    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        if (m_PassLookup.find(passName) == m_PassLookup.end())
        {
            OLO_CORE_ERROR("RenderGraph::SetFinalPass: Pass '{}' not found!", passName);
            return;
        }
        
        OLO_CORE_INFO("Explicitly setting final pass: {}", passName);
        m_ExplicitFinalPassName = passName;
        
        // Resolve the final pass (this will validate the explicit pass)
        ResolveFinalPass();
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_Passes.empty())
        {
            OLO_CORE_WARN("RenderGraph::Execute: No passes to execute!");
            return;
        }
        
        // Make sure the dependency graph is up-to-date
        if (m_DependencyGraphDirty)
        {
            UpdateDependencyGraph();
            ResolveFinalPass();
        }

        // Execute passes in dependency order (excluding final pass)
        // For now, a simple approach: execute all non-final passes first
        for (const auto& pass : m_Passes)
        {
            // Skip executing the final pass here, we'll do it last
            if (pass->GetName() == m_FinalPassName)
                continue;

            pass->Execute();
        }

        // Execute the final pass last
        if (!m_FinalPassName.empty())
        {
            auto finalPass = GetPass(m_FinalPassName);
            if (finalPass)
                finalPass->Execute();
            else
                OLO_CORE_ERROR("RenderGraph::Execute: Final pass '{}' not found!", m_FinalPassName);
        }
        else
        {
            OLO_CORE_WARN("RenderGraph::Execute: No final pass resolved!");
        }
    }

    void RenderGraph::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("RenderGraph::Resize: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        OLO_CORE_INFO("Resizing RenderGraph to: {}x{}", width, height);

        m_Width = width;
        m_Height = height;

        // Resize all passes
        for (const auto& pass : m_Passes)
        {
            pass->ResizeFramebuffer(width, height);
        }
    }

    void RenderGraph::Reset()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Resetting RenderGraph");

        for (const auto& pass : m_Passes)
        {
            pass->OnReset();
        }

        for (const auto& [inputPass, outputPass] : m_PassConnections)
        {
            ConnectPass(outputPass, inputPass);
        }
    }

    void RenderGraph::UpdateDependencyGraph()
    {
        OLO_PROFILE_FUNCTION();
        
        // Clear existing dependency information
        for (const auto& pass : m_Passes)
        {
            const std::string& passName = pass->GetName();
            m_Dependencies[passName].clear();
            m_DependentPasses[passName].clear();
        }
        
        // Rebuild the dependency graph from connections
        for (const auto& [inputPass, outputPass] : m_PassConnections)
        {
            // inputPass depends on outputPass
            m_Dependencies[inputPass].insert(outputPass);
            
            // outputPass is depended on by inputPass
            m_DependentPasses[outputPass].insert(inputPass);
        }
        
        m_DependencyGraphDirty = false;
    }

    void RenderGraph::ResolveFinalPass()
    {
        OLO_PROFILE_FUNCTION();
        
        // Make sure the dependency graph is up-to-date
        if (m_DependencyGraphDirty)
        {
            UpdateDependencyGraph();
        }
        
        // Get automatically detected final pass
        std::string autoDetectedPass = DetectFinalPass();
        
        // If we have an explicitly set final pass and it's valid, use it
        if (!m_ExplicitFinalPassName.empty() && 
            m_PassLookup.find(m_ExplicitFinalPassName) != m_PassLookup.end())
        {
            // Check if the explicit final pass is a valid sink node (no dependents)
            if (m_DependentPasses[m_ExplicitFinalPassName].empty())
            {
                m_FinalPassName = m_ExplicitFinalPassName;
                OLO_CORE_INFO("Using explicitly set final pass: {}", m_FinalPassName);
            }
            else
            {
                // The explicit final pass has dependents, which makes it invalid as a final pass
                OLO_CORE_WARN("Explicitly set final pass '{}' is not a valid sink node in the DAG. It has {} dependent passes.", 
                    m_ExplicitFinalPassName, m_DependentPasses[m_ExplicitFinalPassName].size());
                
                if (!autoDetectedPass.empty())
                {
                    m_FinalPassName = autoDetectedPass;
                    OLO_CORE_INFO("Using auto-detected final pass instead: {}", m_FinalPassName);
                }
            }
        }
        else if (!autoDetectedPass.empty())
        {
            // Use the auto-detected pass
            m_FinalPassName = autoDetectedPass;
            OLO_CORE_INFO("Using auto-detected final pass: {}", m_FinalPassName);
        }
        else
        {
            // No valid final pass found
            m_FinalPassName.clear();
            OLO_CORE_WARN("No valid final pass found in the render graph!");
        }
    }

    std::string RenderGraph::DetectFinalPass() const
    {
        OLO_PROFILE_FUNCTION();
        
        std::vector<std::string> sinkNodes;
        
        // Find all sink nodes (passes that don't have any dependent passes)
        for (const auto& pass : m_Passes)
        {
            const std::string& passName = pass->GetName();
            auto it = m_DependentPasses.find(passName);
            
            // If the pass has no dependents or it's not in the map, it's a sink node
            if (it == m_DependentPasses.end() || it->second.empty())
            {
                sinkNodes.push_back(passName);
            }
        }
        
        if (sinkNodes.empty())
        {
            OLO_CORE_WARN("RenderGraph::DetectFinalPass: No sink nodes found in the DAG. Is there a cycle?");
            return "";
        }
        
        if (sinkNodes.size() > 1)
        {
            OLO_CORE_WARN("RenderGraph::DetectFinalPass: Multiple sink nodes found ({}). This may indicate separate render paths.", sinkNodes.size());
            
            // If the explicitly set final pass is one of the sink nodes, prefer it
            if (!m_ExplicitFinalPassName.empty() && 
                std::find(sinkNodes.begin(), sinkNodes.end(), m_ExplicitFinalPassName) != sinkNodes.end())
            {
                return m_ExplicitFinalPassName;
            }
            
            // Otherwise, first check if any of them is a FinalRenderPass type
            for (const auto& passName : sinkNodes)
            {
                auto it = m_PassLookup.find(passName);
                if (it != m_PassLookup.end() && 
                    std::dynamic_pointer_cast<FinalRenderPass>(it->second) != nullptr)
                {
                    OLO_CORE_INFO("Detected final pass of type FinalRenderPass: {}", passName);
                    return passName;
                }
            }
            
            // If no FinalRenderPass is found, return the first sink node
            OLO_CORE_INFO("Using first detected sink node as final pass: {}", sinkNodes[0]);
            return sinkNodes[0];
        }
        
        // Only one sink node found, it's our final pass
        OLO_CORE_INFO("Single sink node detected as final pass: {}", sinkNodes[0]);
        return sinkNodes[0];
    }

    bool RenderGraph::IsFinalPass(const std::string& passName) const
    {
        // Make sure we have a valid final pass name
        if (m_FinalPassName.empty())
        {
            const_cast<RenderGraph*>(this)->ResolveFinalPass();
        }
        
        return passName == m_FinalPassName;
    }
}