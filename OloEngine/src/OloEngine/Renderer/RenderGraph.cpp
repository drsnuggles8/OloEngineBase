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
        m_FinalPassName.clear();
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

        // Set up the framebuffer for this pass
        pass->SetupFramebuffer(m_Width, m_Height);
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
    }

    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        if (m_PassLookup.find(passName) == m_PassLookup.end())
        {
            OLO_CORE_ERROR("RenderGraph::SetFinalPass: Pass '{}' not found!", passName);
            return;
        }
        
        OLO_CORE_INFO("Setting final pass: {}", passName);
        m_FinalPassName = passName;
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_Passes.empty())
        {
            OLO_CORE_WARN("RenderGraph::Execute: No passes to execute!");
            return;
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
            OLO_CORE_WARN("RenderGraph::Execute: No final pass set!");
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
} 