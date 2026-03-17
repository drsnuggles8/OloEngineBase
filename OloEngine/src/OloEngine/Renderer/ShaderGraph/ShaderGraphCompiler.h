#pragma once

#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    /// Information about a parameter exposed by the shader graph
    struct ShaderGraphParameterInfo
    {
        std::string Name;        // Uniform name (e.g., "u_Albedo")
        ShaderGraphPinType Type; // Data type
        ShaderGraphPinValue DefaultValue;
    };

    /// Result of compiling a shader graph to GLSL
    struct ShaderGraphCompileResult
    {
        bool Success = false;
        bool IsCompute = false;   // True if compiled as a compute shader
        std::string ShaderSource; // Full GLSL with #type vertex / #type fragment (or standalone compute)
        std::string ErrorLog;
        std::vector<ShaderGraphParameterInfo> ExposedParameters;
    };

    /// Compiles a ShaderGraph into a complete GLSL shader source string.
    class ShaderGraphCompiler
    {
      public:
        ShaderGraphCompiler() = default;

        /// Compile the graph into GLSL source code
        ShaderGraphCompileResult Compile(const ShaderGraph& graph) const;

      private:
        /// Generate the vertex shader stage
        std::string GenerateVertexShader() const;

        /// Generate the fragment shader stage from the graph's topological node order
        std::string GenerateFragmentShader(
            const ShaderGraph& graph,
            const std::vector<const ShaderGraphNode*>& sortedNodes,
            std::vector<ShaderGraphParameterInfo>& outParameters) const;

        /// Generate a compute shader from the graph's topological node order
        std::string GenerateComputeShader(
            const ShaderGraph& graph,
            const std::vector<const ShaderGraphNode*>& sortedNodes,
            std::vector<ShaderGraphParameterInfo>& outParameters) const;

        /// Check if the graph is a compute graph (has ComputeOutput instead of PBROutput)
        static bool IsComputeGraph(const ShaderGraph& graph);

        /// Generate a unique variable name for a node's output pin
        static std::string MakeVarName(const ShaderGraphNode& node, const ShaderGraphPin& pin);

        /// Get the expression for an input pin (either connected wire or default value)
        std::string ResolveInputExpression(
            const ShaderGraph& graph,
            const ShaderGraphPin& inputPin,
            const std::unordered_map<u64, std::string>& pinVarNames) const;

        /// Generate GLSL code for a single node
        std::string GenerateNodeCode(
            const ShaderGraph& graph,
            const ShaderGraphNode& node,
            std::unordered_map<u64, std::string>& pinVarNames) const;
    };

} // namespace OloEngine
