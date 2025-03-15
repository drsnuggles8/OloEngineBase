#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"

#include <vector>
#include <unordered_map>
#include <string>

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
        void Init(uint32_t width, uint32_t height);
        
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
         * @brief Set the final pass that will render to the screen.
         * @param passName The name of the pass to use as the final pass
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
        void Resize(uint32_t width, uint32_t height);
        
        /**
         * @brief Reset the render graph and all its passes.
         */
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