#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ComputeShader.h"

namespace OloEngine
{
    class ShaderGraphAsset : public Asset
    {
      public:
        ShaderGraphAsset() = default;
        ~ShaderGraphAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::ShaderGraph;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // ── Graph access ─────────────────────────────────────

        ShaderGraph& GetGraph()
        {
            return m_Graph;
        }
        const ShaderGraph& GetGraph() const
        {
            return m_Graph;
        }

        // ── Compilation ──────────────────────────────────────

        /// Compile the graph to GLSL. Caches the result until the graph changes.
        const ShaderGraphCompileResult& Compile()
        {
            OLO_PROFILE_FUNCTION();

            if (m_Dirty || !m_CachedResult.Success)
            {
                ShaderGraphCompiler compiler;
                m_CachedResult = compiler.Compile(m_Graph);
                m_Dirty = false;
            }
            return m_CachedResult;
        }

        /// Mark the graph as dirty so it will be recompiled on next Compile() call
        void MarkDirty()
        {
            m_Dirty = true;
        }

        bool IsDirty() const
        {
            return m_Dirty;
        }

        const ShaderGraphCompileResult& GetCachedCompileResult() const
        {
            return m_CachedResult;
        }

        /// Compile the graph and create a Ref<Shader> from the result.
        /// Returns nullptr if compilation fails.
        Ref<Shader> CompileToShader(const std::string& name)
        {
            const auto& result = Compile();
            if (!result.Success)
                return nullptr;

            // Split the combined source at "#type fragment" to get vertex and fragment parts
            const std::string marker = "#type fragment";
            auto fragPos = result.ShaderSource.find(marker);
            if (fragPos == std::string::npos)
                return nullptr;

            // Vertex source: everything before #type fragment, strip the "#type vertex\n" prefix
            std::string vertexSrc = result.ShaderSource.substr(0, fragPos);
            const std::string vtxMarker = "#type vertex\n";
            if (auto pos = vertexSrc.find(vtxMarker); pos != std::string::npos)
                vertexSrc.erase(pos, vtxMarker.size());

            // Fragment source: everything from #type fragment onward, strip the marker
            std::string fragmentSrc = result.ShaderSource.substr(fragPos + marker.size());
            // Remove leading newline if present
            if (!fragmentSrc.empty() && fragmentSrc[0] == '\n')
                fragmentSrc.erase(0, 1);

            return Shader::Create(name, vertexSrc, fragmentSrc);
        }

        /// Compile the graph and create a Ref<ComputeShader> from the result.
        /// Returns nullptr if compilation fails or graph is not a compute graph.
        Ref<ComputeShader> CompileToComputeShader(const std::string& name)
        {
            const auto& result = Compile();
            if (!result.Success || !result.IsCompute)
                return nullptr;

            return ComputeShader::CreateFromSource(name, result.ShaderSource);
        }

      private:
        ShaderGraph m_Graph;
        ShaderGraphCompileResult m_CachedResult;
        bool m_Dirty = true;

        friend class ShaderGraphSerializer;
    };

} // namespace OloEngine
