#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Passes/CommandFinalRenderPass.h"

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

		// First pass: Connect framebuffers between dependencies
		for (const auto& [outputPass, inputPasses] : m_DependentPasses)
		{
			auto& outputPassVariant = m_PassLookup[outputPass];
			Ref<Framebuffer> outputFramebuffer;

			// Get output framebuffer from output pass
			if (auto renderPass = std::get_if<Ref<RenderPass>>(&outputPassVariant))
			{
				outputFramebuffer = (*renderPass)->GetTarget();
			}
			else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&outputPassVariant))
			{
				outputFramebuffer = (*cmdRenderPass)->GetTarget();
			}

			// Set this framebuffer as input for all dependent passes
			if (outputFramebuffer)
			{
				for (const auto& inputPass : inputPasses)
				{
					auto& inputPassVariant = m_PassLookup[inputPass];

					if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&inputPassVariant))
					{
						// Handle CommandRenderPass (like CommandFinalRenderPass)
						if (auto* cmdFinalPass = dynamic_cast<CommandFinalRenderPass*>(cmdRenderPass->get()))
						{
							cmdFinalPass->SetInputFramebuffer(outputFramebuffer);
						}
					}
				}
			}
			else
			{
				OLO_CORE_WARN("RenderGraph::Execute: No output framebuffer available for pass {}", outputPass);
			}
		}

		// Second pass: Execute passes in order
		for (const auto& passName : m_PassOrder)
		{
			auto& passVariant = m_PassLookup[passName];

			if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant))
			{
				// OLO_CORE_TRACE("Executing RenderPass: {}", passName);
				(*renderPass)->Execute();
			}
			else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant))
			{
				// OLO_CORE_TRACE("Executing CommandRenderPass: {}", passName);
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

	std::vector<Ref<RenderPass>> RenderGraph::GetAllPasses() const
	{
		OLO_PROFILE_FUNCTION();
		
		std::vector<Ref<RenderPass>> result;
		result.reserve(m_PassOrder.size());
		
		// First add passes in execution order (from m_PassOrder)
		for (const auto& passName : m_PassOrder)
		{
			const auto& passVariant = m_PassLookup.find(passName);
			if (passVariant != m_PassLookup.end())
			{
				if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant->second))
				{
					result.push_back(*renderPass);
				}
				else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant->second))
				{
					// For the debugger, we need to treat CommandRenderPass as RenderPass
					// This works if CommandRenderPass inherits from RenderPass, otherwise
					// we might need a conversion or wrapper here
					Ref<RenderPass> basePass = std::dynamic_pointer_cast<RenderPass>(*cmdRenderPass);
					if (basePass)
					{
						result.push_back(basePass);
					}
				}
			}
		}
		
		// If any passes weren't included in the execution order, add them at the end
		for (const auto& [name, passVariant] : m_PassLookup)
		{
			// Skip if already in execution order
			if (std::find(m_PassOrder.begin(), m_PassOrder.end(), name) != m_PassOrder.end())
				continue;
				
			if (auto renderPass = std::get_if<Ref<RenderPass>>(&passVariant))
			{
				result.push_back(*renderPass);
			}
			else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&passVariant))
			{
				Ref<RenderPass> basePass = std::dynamic_pointer_cast<RenderPass>(*cmdRenderPass);
				if (basePass)
				{
					result.push_back(basePass);
				}
			}
		}
		
		return result;
	}

	bool RenderGraph::IsFinalPass(const std::string& passName) const
	{
		OLO_PROFILE_FUNCTION();
		return passName == m_FinalPassName;
	}

	std::vector<RenderGraph::ConnectionInfo> RenderGraph::GetConnections() const
	{
		OLO_PROFILE_FUNCTION();
		
		std::vector<ConnectionInfo> connections;
		
		// Reserve space to avoid reallocations
		u32 totalConnections = 0;
		for (const auto& [outputPass, inputPasses] : m_DependentPasses)
		{
			totalConnections += static_cast<u32>(inputPasses.size());
		}
		connections.reserve(totalConnections);
		
		// Build connection information from dependency graph
		for (const auto& [outputPass, inputPasses] : m_DependentPasses)
		{
			for (const auto& inputPass : inputPasses)
			{
				ConnectionInfo connection;
				connection.OutputPass = outputPass;
				connection.InputPass = inputPass;
				connection.AttachmentIndex = 0; // Default attachment index
				
				connections.push_back(connection);
			}
		}
		
		return connections;
	}
}
