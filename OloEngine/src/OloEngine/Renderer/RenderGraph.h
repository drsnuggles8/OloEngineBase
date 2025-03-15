#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"

#include <vector>
#include <unordered_map>
#include <string>

namespace OloEngine
{
    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        void Init(uint32_t width, uint32_t height);
        void Shutdown();

        // Add a render pass to the graph
        void AddPass(const Ref<RenderPass>& pass);
        
        // Get a pass by name
        Ref<RenderPass> GetPass(const std::string& name);
        
        // Connect a pass's output to another pass's input
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);
        
        // Set the pass that will be executed last (to the screen)
        void SetFinalPass(const std::string& passName);
        
        // Execute all passes in the graph
        void Execute();
        
        // Resize all framebuffers
        void Resize(uint32_t width, uint32_t height);
        
        // Reset the graph (recreate passes, etc.)
        void Reset();

    private:
        std::vector<Ref<RenderPass>> m_Passes;
        std::unordered_map<std::string, Ref<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::string> m_PassConnections;
        std::string m_FinalPassName;
        
        uint32_t m_Width = 0;
        uint32_t m_Height = 0;
    };
} 