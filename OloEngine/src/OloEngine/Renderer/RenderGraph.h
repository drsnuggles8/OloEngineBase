#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief Manages a graph of render passes forming a complete rendering pipeline.
     */
    class RenderGraph : public RefCounted
    {
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;
        
        void Init(u32 width, u32 height);
        void Shutdown();
        
        // Only support RenderPass
        void AddPass(const AssetRef<RenderPass>& pass);
          // Connect two passes in the graph
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);
        
        // Execute all passes in the correct order
        void Execute();
        
        // Resize all passes in the graph
        void Resize(u32 width, u32 height);
        
        // Set the final pass in the graph
        void SetFinalPass(const std::string& passName);
        
        /**
         * @brief Get all render passes in the graph for debugging or inspection.
         * @return Vector of render passes in the execution order
         */
        [[nodiscard]] std::vector<AssetRef<RenderPass>> GetAllPasses() const;
        
        // Get a pass by name and cast to the requested type
        template<typename T>
        AssetRef<T> GetPass(const std::string& name)
        {
            if (m_PassLookup.find(name) != m_PassLookup.end())
            {
                return m_PassLookup.at(name).As<T>();
            }
            return nullptr;
        }

        [[nodiscard]] bool IsFinalPass(const std::string& passName) const;
        
        struct ConnectionInfo
        {
            std::string OutputPass;
            std::string InputPass;
            u32 AttachmentIndex = 0;
        };
        
        [[nodiscard]] std::vector<ConnectionInfo> GetConnections() const;
        
    private:
        void UpdateDependencyGraph();
        void ResolveFinalPass();
        
        std::unordered_map<std::string, AssetRef<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;
        std::unordered_map<std::string, std::vector<std::string>> m_DependentPasses;
        std::vector<std::string> m_PassOrder;
        std::string m_FinalPassName;
        bool m_DependencyGraphDirty = false;
    };
}