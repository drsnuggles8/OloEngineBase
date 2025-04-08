#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Passes/CommandRenderPass.h"

#include <vector>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <variant>

namespace OloEngine
{
    /**
     * @brief Manages a graph of render passes forming a complete rendering pipeline.
     */
    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;
        
        void Init(u32 width, u32 height);
        void Shutdown();
        
        // Add a pass to the render graph. Now supports both RenderPass and CommandRenderPass
        void AddPass(const Ref<RenderPass>& pass);
        void AddPass(const Ref<CommandRenderPass>& pass);
        
        // Connect two passes in the graph
        void ConnectPass(const std::string& outputPass, const std::string& inputPass, u32 outputAttachment = 0);
        
        // Execute all passes in the correct order
        void Execute();
        
        // Resize all passes in the graph
        void Resize(u32 width, u32 height);
        
        // Set the final pass in the graph
        void SetFinalPass(const std::string& passName);
        
        // Get a pass by name and cast to the requested type
        template<typename T>
        Ref<T> GetPass(const std::string& name)
        {
            if (m_PassLookup.find(name) != m_PassLookup.end())
            {
                if (auto renderPass = std::get_if<Ref<RenderPass>>(&m_PassLookup[name]))
                {
                    return std::dynamic_pointer_cast<T>(*renderPass);
                }
                else if (auto cmdRenderPass = std::get_if<Ref<CommandRenderPass>>(&m_PassLookup[name]))
                {
                    return std::dynamic_pointer_cast<T>(*cmdRenderPass);
                }
            }
            return nullptr;
        }
        
    private:
        void UpdateDependencyGraph();
        void ResolveFinalPass();
        
        using PassVariant = std::variant<Ref<RenderPass>, Ref<CommandRenderPass>>;
        
        std::unordered_map<std::string, PassVariant> m_PassLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;
        std::unordered_map<std::string, std::vector<std::string>> m_DependentPasses;
        std::vector<std::string> m_PassOrder;
        std::string m_FinalPassName;
        bool m_DependencyGraphDirty = true;
    };
}