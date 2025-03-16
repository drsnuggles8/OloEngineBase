#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"

#include <vector>
#include <unordered_map>
#include <string>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief Manages a graph of render passes forming a complete rendering pipeline.
     * 
     * The render graph handles the execution order of render passes and manages
     * connections between them. It provides an abstraction for multi-pass rendering
     * techniques such as deferred rendering, post-processing effects, and more.
     */
    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        /**
         * @brief Initialize the render graph with the specified dimensions.
         * @param width The width of the render targets
         * @param height The height of the render targets
         */
        void Init(u32 width, u32 height);
        
        /**
         * @brief Clean up resources used by the render graph.
         */
        void Shutdown();

        /**
         * @brief Add a render pass to the graph.
         * @param pass The render pass to add
         */
        void AddPass(const Ref<RenderPass>& pass);
        
        /**
         * @brief Get a render pass by name.
         * @param name The name of the pass to find
         * @return The render pass, or nullptr if not found
         */
        Ref<RenderPass> GetPass(const std::string& name);
        
        /**
         * @brief Connect an output pass to an input pass.
         * 
         * This establishes a connection where the output pass's framebuffer
         * is used as an input to the input pass.
         * 
         * @param outputPass The name of the pass producing the output
         * @param inputPass The name of the pass consuming the output
         */
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);
        
        /**
         * @brief Explicitly set the final pass that will render to the screen.
         * @param passName The name of the pass to use as the final pass
         * @note This is optional as the final pass can be auto-detected as a sink node in the DAG
         */
        void SetFinalPass(const std::string& passName);
        
        /**
         * @brief Execute all passes in the render graph in the correct order.
         */
        void Execute();
        
        /**
         * @brief Resize all framebuffers in the graph.
         * @param width The new width
         * @param height The new height
         */
        void Resize(u32 width, u32 height);
        
        /**
         * @brief Reset the render graph and all its passes.
         */
        void Reset();
        
        /**
         * @brief Automatically detect the final pass in the render graph.
         * 
         * Identifies the final pass as the one that isn't a dependency for any other pass.
         * If multiple passes are detected as potential final passes, the one explicitly set
         * with SetFinalPass will be used, or the first one found otherwise.
         * 
         * @return Name of the detected final pass
         */
        std::string DetectFinalPass() const;

        // Debug/visualization methods
        /**
         * @brief Get all passes in the render graph.
         * @return Vector of all render passes
         */
        std::vector<Ref<RenderPass>> GetAllPasses() const { return m_Passes; }
        
        /**
         * @brief Get all connections between passes.
         * @return Map of input pass name to output pass name
         */
        const std::unordered_map<std::string, std::string>& GetConnections() const { return m_PassConnections; }
        
        /**
         * @brief Check if a pass is the final pass.
         * @param passName The name of the pass to check
         * @return True if the pass is the final pass, false otherwise
         */
        bool IsFinalPass(std::string_view passName) const;
        
        /**
         * @brief Get the current dimensions of the render graph.
         * @return Pair of width and height
         */
        std::pair<u32, u32> GetDimensions() const { return {m_Width, m_Height}; }

    private:
        /**
         * @brief Update the dependency graph after adding a new pass or connection.
         */
        void UpdateDependencyGraph();

        /**
         * @brief Find and validate the final pass based on the dependency graph.
         * Will use the explicitly set final pass if available and valid.
         */
        void ResolveFinalPass();

    private:
        std::vector<Ref<RenderPass>> m_Passes;
        std::unordered_map<std::string, Ref<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::string> m_PassConnections; // input -> output
        std::string m_ExplicitFinalPassName; // Explicitly set final pass name
        std::string m_FinalPassName;         // Actual final pass resolved from graph structure
        
        u32 m_Width = 0;
        u32 m_Height = 0;
        
        // Dependency tracking
        std::unordered_map<std::string, std::unordered_set<std::string>> m_Dependencies;      // pass -> passes it depends on
        std::unordered_map<std::string, std::unordered_set<std::string>> m_DependentPasses;   // pass -> passes that depend on it
        bool m_DependencyGraphDirty = true;
    };
}
