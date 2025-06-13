#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Shader.h"
#include "DebugUtils.h"

#include <imgui.h>
#include <glad/gl.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <chrono>

// Convenience macros for shader debugging (only in debug builds)
#ifdef OLO_DEBUG
    #define OLO_SHADER_REGISTER(shader) \
        OloEngine::ShaderDebugger::GetInstance().RegisterShader(shader)
    #define OLO_SHADER_REGISTER_MANUAL(rendererID, name, filePath) \
        OloEngine::ShaderDebugger::GetInstance().RegisterShader(rendererID, name, filePath)
    #define OLO_SHADER_UNREGISTER(rendererID) \
        OloEngine::ShaderDebugger::GetInstance().UnregisterShader(rendererID)
    #define OLO_SHADER_COMPILATION_START(name, filepath) \
        OloEngine::ShaderDebugger::GetInstance().OnCompilationStart(name, filepath)
    #define OLO_SHADER_COMPILATION_END(rendererID, success, errorMsg, compileTime) \
        OloEngine::ShaderDebugger::GetInstance().OnCompilationEnd(rendererID, success, errorMsg, compileTime)
    #define OLO_SHADER_RELOAD_START(rendererID) \
        OloEngine::ShaderDebugger::GetInstance().OnReloadStart(rendererID)
    #define OLO_SHADER_RELOAD_END(rendererID, success) \
        OloEngine::ShaderDebugger::GetInstance().OnReloadEnd(rendererID, success)
    #define OLO_SHADER_BIND(rendererID) \
        OloEngine::ShaderDebugger::GetInstance().OnShaderBind(rendererID)
    #define OLO_SHADER_UNIFORM_SET(rendererID, name, type) \
        OloEngine::ShaderDebugger::GetInstance().OnUniformSet(rendererID, name, type)
    #define OLO_SHADER_SET_SOURCE(rendererID, stage, original, generated, spirv) \
        OloEngine::ShaderDebugger::GetInstance().SetShaderSource(rendererID, stage, original, generated, spirv)
#else
    #define OLO_SHADER_REGISTER(shader)
    #define OLO_SHADER_REGISTER_MANUAL(rendererID, name, filePath)
    #define OLO_SHADER_UNREGISTER(rendererID)
    #define OLO_SHADER_COMPILATION_START(name, filepath)
    #define OLO_SHADER_COMPILATION_END(rendererID, success, errorMsg, compileTime)
    #define OLO_SHADER_RELOAD_START(rendererID)
    #define OLO_SHADER_RELOAD_END(rendererID, success)
    #define OLO_SHADER_BIND(rendererID)
    #define OLO_SHADER_UNIFORM_SET(rendererID, name, type)
    #define OLO_SHADER_SET_SOURCE(rendererID, stage, original, generated, spirv)
#endif

namespace OloEngine
{
    /**
     * @brief Comprehensive shader debugging and analysis tool
     * 
     * Provides detailed inspection of shader compilation, uniforms, performance,
     * source code viewing, hot-reload tracking, and SPIR-V analysis.
     */
    class ShaderDebugger
    {
    public:
        enum class UniformType : u8
        {
            Int, IntArray, Float, Float2, Float3, Float4, Mat3, Mat4, Sampler2D, SamplerCube
        };

        enum class ShaderStage : u8
        {
            Vertex = 0, Fragment = 1, Geometry = 2, Compute = 3
        };

        struct UniformInfo
        {
            std::string m_Name;
            UniformType m_Type;
            u32 m_Location = 0;
            u32 m_Size = 1; // Array size or 1 for non-arrays
            std::string m_LastValue; // String representation of last set value
            u32 m_SetCount = 0; // How many times this uniform has been set
            std::chrono::steady_clock::time_point m_LastSetTime;
        };

        struct UniformBufferInfo
        {
            std::string m_Name;
            u32 m_Binding = 0;
            u32 m_Size = 0;
            std::vector<std::string> m_Members;
        };

        struct SamplerInfo
        {
            std::string m_Name;
            u32 m_Binding = 0;
            u32 m_TextureUnit = 0;
            std::string m_Type; // "sampler2D", "samplerCube", etc.
        };

        struct CompilationResult
        {
            bool m_Success = false;
            std::string m_ErrorMessage;
            f64 m_CompileTimeMs = 0.0;
            std::chrono::steady_clock::time_point m_Timestamp;
            sizet m_VulkanSPIRVSize = 0;
            sizet m_OpenGLSPIRVSize = 0;
            u32 m_InstructionCount = 0; // Estimated from SPIR-V
        };

        struct ReloadEvent
        {
            std::chrono::steady_clock::time_point m_Timestamp;
            bool m_Success = false;
            std::string m_Reason; // Why reload was triggered
        };

        struct ShaderInfo
        {
            u32 m_RendererID = 0;
            std::string m_Name;
            std::string m_FilePath;
            
            // Source code
            std::unordered_map<ShaderStage, std::string> m_OriginalSource;
            std::unordered_map<ShaderStage, std::string> m_GeneratedGLSL;
            std::unordered_map<ShaderStage, std::vector<u8>> m_SPIRVBinary;
            
            // Reflection data
            std::vector<UniformInfo> m_Uniforms;
            std::vector<UniformBufferInfo> m_UniformBuffers;
            std::vector<SamplerInfo> m_Samplers;
            
            // Performance and usage tracking
            CompilationResult m_LastCompilation;
            std::vector<ReloadEvent> m_ReloadHistory;
            u32 m_BindCount = 0;
            std::chrono::steady_clock::time_point m_LastBindTime;
            f64 m_TotalActiveTimeMs = 0.0; // Time spent bound
            std::chrono::steady_clock::time_point m_LastActivationTime;
            
            // Status
            bool m_IsActive = false;
            bool m_HasErrors = false;
            bool m_IsReloading = false;
            std::chrono::steady_clock::time_point m_CreationTime;
        };

        /**
         * @brief Get the singleton instance
         */
        static ShaderDebugger& GetInstance();

        /**
         * @brief Initialize the shader debugger
         */
        void Initialize();

        /**
         * @brief Shutdown and cleanup
         */
        void Shutdown();

        /**
         * @brief Register a shader for debugging
         * @param shader Shared pointer to the shader
         */
        void RegisterShader(const Ref<Shader>& shader);
        void RegisterShader(u32 rendererID, const std::string& name, const std::string& filePath = "");
        /**
         * @brief Unregister a shader when it's destroyed
         * @param rendererID OpenGL shader program ID
         */
        void UnregisterShader(u32 rendererID);

        /**
         * @brief Called when shader compilation starts
         * @param name Shader name
         * @param filepath Shader file path
         */
        void OnCompilationStart(const std::string& name, const std::string& filepath);

        /**
         * @brief Called when shader compilation ends
         * @param rendererID OpenGL shader program ID
         * @param success Whether compilation succeeded
         * @param errorMsg Error message if compilation failed
         * @param compileTimeMs Compilation time in milliseconds
         */
        void OnCompilationEnd(u32 rendererID, bool success, const std::string& errorMsg, f64 compileTimeMs);

        /**
         * @brief Called when shader reload starts
         * @param rendererID OpenGL shader program ID
         */
        void OnReloadStart(u32 rendererID);

        /**
         * @brief Called when shader reload ends
         * @param rendererID OpenGL shader program ID
         * @param success Whether reload succeeded
         */
        void OnReloadEnd(u32 rendererID, bool success);

        /**
         * @brief Called when a shader is bound
         * @param rendererID OpenGL shader program ID
         */
        void OnShaderBind(u32 rendererID);

        /**
         * @brief Called when a uniform is set
         * @param rendererID OpenGL shader program ID
         * @param name Uniform name
         * @param type Uniform type
         */
        void OnUniformSet(u32 rendererID, const std::string& name, UniformType type);

        /**
         * @brief Update shader reflection data
         * @param rendererID OpenGL shader program ID
         * @param stage Shader stage
         * @param spirvData SPIR-V binary data
         */
        void UpdateReflectionData(u32 rendererID, ShaderStage stage, const std::vector<u32>& spirvData);

        /**
         * @brief Set shader source code
         * @param rendererID OpenGL shader program ID
         * @param stage Shader stage
         * @param originalSource Original GLSL source
         * @param generatedGLSL Generated OpenGL GLSL (if different)
         * @param spirvBinary SPIR-V binary data
         */
        void SetShaderSource(u32 rendererID, ShaderStage stage, 
                           const std::string& originalSource, 
                           const std::string& generatedGLSL = "",
                           const std::vector<u8>& spirvBinary = {});

        /**
         * @brief Render the debug UI
         * @param open Pointer to boolean controlling window visibility
         * @param title Window title
         */
        void RenderDebugView(bool* open = nullptr, const char* title = "Shader Debugger");

        /**
         * @brief Get shader information by renderer ID
         * @param rendererID OpenGL shader program ID
         * @return Pointer to shader info or nullptr if not found
         */
        const ShaderInfo* GetShaderInfo(u32 rendererID) const;

        /**
         * @brief Get all tracked shaders
         * @return Map of renderer ID to shader info
         */
        const std::unordered_map<u32, ShaderInfo>& GetAllShaders() const { return m_Shaders; }

        /**
         * @brief Export shader debugging report
         * @param filePath Output file path
         * @return True if export succeeded
         */
        bool ExportReport(const std::string& filePath) const;

    private:
        ShaderDebugger() = default;
        ~ShaderDebugger() = default;

        // Non-copyable
        ShaderDebugger(const ShaderDebugger&) = delete;
        ShaderDebugger& operator=(const ShaderDebugger&) = delete;        // UI rendering methods
        void RenderShaderList();
        void RenderShaderDetails(const ShaderInfo& shaderInfo);
        void RenderSourceCode(const ShaderInfo& shaderInfo);
        void RenderUniforms(const ShaderInfo& shaderInfo);
        void RenderPerformanceMetrics(const ShaderInfo& shaderInfo);
        void RenderReloadHistory(const ShaderInfo& shaderInfo);
        void RenderSPIRVAnalysis(const ShaderInfo& shaderInfo);
        void RenderCompilationErrors(const ShaderInfo& shaderInfo);

        // Helper methods
        void UpdateActiveTime(ShaderInfo& shaderInfo);        std::string GetUniformTypeString(UniformType type) const;
        std::string GetShaderStageString(ShaderStage stage) const;
        ImVec4 GetShaderStageColor(ShaderStage stage) const;
        void AnalyzeSPIRV(const std::vector<u8>& spirvData, u32& instructionCount) const;
        void AnalyzeSPIRVFromWords(const std::vector<u32>& spirvWords, u32& instructionCount) const;
        
        // Advanced SPIR-V analysis methods
        std::string GenerateSPIRVDisassembly(const std::vector<u8>& spirvData) const;
        void PerformOptimizationAnalysis(const std::vector<u8>& spirvData) const;

        // Data members
        bool m_IsInitialized = false;
        mutable std::mutex m_ShaderMutex;
        std::unordered_map<u32, ShaderInfo> m_Shaders;
        
        // Compilation tracking
        struct PendingCompilation
        {
            std::string m_Name;
            std::string m_FilePath;
            std::chrono::steady_clock::time_point m_StartTime;
        };
        std::unordered_map<std::string, PendingCompilation> m_PendingCompilations;

        // UI state
        u32 m_SelectedShaderID = 0;
        i32 m_SelectedTab = 0;
        bool m_ShowOnlyActiveShaders = false;
        bool m_ShowOnlyErrorShaders = false;
        bool m_AutoSelectNewShaders = true;
        char m_SearchFilter[256] = "";

        // Performance tracking
        f64 m_TotalCompilationTime = 0.0;
        u32 m_TotalCompilations = 0;
        u32 m_FailedCompilations = 0;
        u32 m_TotalReloads = 0;
    };

    // Helper function to convert OpenGL shader stage to our enum
    inline ShaderDebugger::ShaderStage GLStageToShaderStage(GLenum stage)
    {
        switch (stage)
        {
            case GL_VERTEX_SHADER:   return ShaderDebugger::ShaderStage::Vertex;
            case GL_FRAGMENT_SHADER: return ShaderDebugger::ShaderStage::Fragment;
            case GL_GEOMETRY_SHADER: return ShaderDebugger::ShaderStage::Geometry;
            case GL_COMPUTE_SHADER:  return ShaderDebugger::ShaderStage::Compute;
            default: return ShaderDebugger::ShaderStage::Vertex;
        }
    }
}
