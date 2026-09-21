#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"
#include "DebugUtils.h"

#include <imgui.h>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <chrono>

#include "OloEngine/Threading/Mutex.h"

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
    enum class ShaderDebugUniformType : u8
    {
        Int,
        UInt,
        IntArray,
        Float,
        Float2,
        Float3,
        Float4,
        Mat3,
        Mat4,
        Sampler2D,
        SamplerCube
    };

    enum class ShaderDebugStage : u8
    {
        Vertex = 0,
        Fragment = 1,
        Geometry = 2,
        Compute = 3
    };

    struct ShaderDebugUniformInfo
    {
        FString m_Name;
        ShaderDebugUniformType m_Type;
        u32 m_Location = 0;
        u32 m_Size = 1;      // Array size or 1 for non-arrays
        FString m_LastValue; // String representation of last set value
        u32 m_SetCount = 0;  // How many times this uniform has been set
        std::chrono::steady_clock::time_point m_LastSetTime;
    };

    struct ShaderDebugUniformBufferInfo
    {
        FString m_Name;
        u32 m_Binding = 0;
        u32 m_Size = 0;
        TArray<FString> m_Members;
    };

    struct ShaderDebugSamplerInfo
    {
        FString m_Name;
        u32 m_Binding = 0;
        u32 m_TextureUnit = 0;
        FString m_Type; // "sampler2D", "samplerCube", etc.
    };

    struct ShaderDebugCompilationResult
    {
        bool m_Success = false;
        FString m_ErrorMessage;
        f64 m_CompileTimeMs = 0.0;
        std::chrono::steady_clock::time_point m_Timestamp;
        sizet m_VertexGeometrySPIRVSize = 0;  // Vertex + Geometry stages
        sizet m_FragmentComputeSPIRVSize = 0; // Fragment + Compute stages
        u32 m_InstructionCount = 0;           // Estimated from SPIR-V
    };

    struct ShaderDebugResourceBindingInfo
    {
        FString m_Name;
        ShaderResourceType m_Type = ShaderResourceType::None;
        u32 m_BindingPoint = 0;
        bool m_IsBound = false;
    };

    struct ShaderDebugReloadEvent
    {
        std::chrono::steady_clock::time_point m_Timestamp;
        bool m_Success = false;
        FString m_Reason; // Why reload was triggered
    };

    // Owns FString/TArray storage and value metadata; clock values retain their own trait.
    template<>
    struct TIsTriviallyRelocatable<ShaderDebugUniformInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_Type)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_Location)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_Size)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_LastValue)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_SetCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformInfo::m_LastSetTime)>::Value;
    };

    // Owns FString/TArray storage and value metadata; clock values retain their own trait.
    template<>
    struct TIsTriviallyRelocatable<ShaderDebugUniformBufferInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ShaderDebugUniformBufferInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformBufferInfo::m_Binding)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformBufferInfo::m_Size)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugUniformBufferInfo::m_Members)>::Value;
    };

    // Owns FString/TArray storage and value metadata; clock values retain their own trait.
    template<>
    struct TIsTriviallyRelocatable<ShaderDebugSamplerInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ShaderDebugSamplerInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugSamplerInfo::m_Binding)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugSamplerInfo::m_TextureUnit)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugSamplerInfo::m_Type)>::Value;
    };

    // Owns FString/TArray storage and value metadata; clock values retain their own trait.
    template<>
    struct TIsTriviallyRelocatable<ShaderDebugResourceBindingInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ShaderDebugResourceBindingInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugResourceBindingInfo::m_Type)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugResourceBindingInfo::m_BindingPoint)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugResourceBindingInfo::m_IsBound)>::Value;
    };

    // Owns FString/TArray storage and value metadata; clock values retain their own trait.
    template<>
    struct TIsTriviallyRelocatable<ShaderDebugReloadEvent>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(ShaderDebugReloadEvent::m_Timestamp)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugReloadEvent::m_Success)>::Value &&
                                      TIsTriviallyRelocatable<decltype(ShaderDebugReloadEvent::m_Reason)>::Value;
    };

    // @brief Comprehensive shader debugging and analysis tool
    //
    // Provides detailed inspection of shader compilation, uniforms, performance,
    // source code viewing, hot-reload tracking, and SPIR-V analysis.
    class ShaderDebugger
    {
      public:
        using UniformType = ShaderDebugUniformType;
        using ShaderStage = ShaderDebugStage;
        using UniformInfo = ShaderDebugUniformInfo;
        using UniformBufferInfo = ShaderDebugUniformBufferInfo;
        using SamplerInfo = ShaderDebugSamplerInfo;
        using CompilationResult = ShaderDebugCompilationResult;
        using ResourceBindingInfo = ShaderDebugResourceBindingInfo;
        using ReloadEvent = ShaderDebugReloadEvent;

        struct ShaderInfo
        {
            u32 m_RendererID = 0;
            FString m_Name;
            FString m_FilePath;

            // Source code
            std::unordered_map<ShaderStage, FString> m_OriginalSource;
            std::unordered_map<ShaderStage, FString> m_GeneratedGLSL;
            std::unordered_map<ShaderStage, TArray<u8>> m_SPIRVBinary;

            // Reflection data
            TArray<UniformInfo> m_Uniforms;
            TArray<UniformBufferInfo> m_UniformBuffers;
            TArray<SamplerInfo> m_Samplers;

            // Resource binding information from UniformBufferRegistry
            TArray<ResourceBindingInfo> m_ResourceBindings;

            // Performance and usage tracking
            CompilationResult m_LastCompilation;
            TArray<ReloadEvent> m_ReloadHistory;
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

        // @brief Get the singleton instance
        static ShaderDebugger& GetInstance();

        // @brief Initialize the shader debugger
        void Initialize();

        // @brief Shutdown and cleanup
        void Shutdown();

        // @brief Register a shader for debugging
        // @param shader Asset reference to the shader
        void RegisterShader(const Ref<Shader>& shader);
        void RegisterShader(u32 rendererID, const std::string& name, const std::string& filePath = "");
        // @brief Unregister a shader when it's destroyed
        // @param rendererID OpenGL shader program ID
        void UnregisterShader(u32 rendererID);

        // @brief Called when shader compilation starts
        // @param name Shader name
        // @param filepath Shader file path
        void OnCompilationStart(const std::string& name, const std::string& filepath);

        // @brief Called when shader compilation ends
        // @param rendererID OpenGL shader program ID
        // @param success Whether compilation succeeded
        // @param errorMsg Error message if compilation failed
        // @param compileTimeMs Compilation time in milliseconds
        void OnCompilationEnd(u32 rendererID, bool success, const std::string& errorMsg, f64 compileTimeMs);

        // @brief Called when shader reload starts
        // @param rendererID OpenGL shader program ID
        void OnReloadStart(u32 rendererID);

        // @brief Called when shader reload ends
        // @param rendererID OpenGL shader program ID
        // @param success Whether reload succeeded
        void OnReloadEnd(u32 rendererID, bool success);

        // @brief Called when a shader is bound
        // @param rendererID OpenGL shader program ID
        void OnShaderBind(u32 rendererID);

        // @brief Called when a uniform is set
        // @param rendererID OpenGL shader program ID
        // @param name Uniform name
        // @param type Uniform type
        void OnUniformSet(u32 rendererID, const std::string& name, UniformType type);

        // @brief Update shader reflection data
        // @param rendererID OpenGL shader program ID
        // @param spirvData SPIR-V binary data
        void UpdateReflectionData(u32 rendererID, std::span<const u32> spirvData);

        // @brief Set shader source code
        // @param rendererID OpenGL shader program ID
        // @param stage Shader stage
        // @param originalSource Original GLSL source
        // @param generatedGLSL Generated OpenGL GLSL (if different)
        // @param spirvBinary SPIR-V binary data
        void SetShaderSource(u32 rendererID, ShaderStage stage,
                             const std::string& originalSource,
                             const std::string& generatedGLSL = "",
                             std::span<const u8> spirvBinary = {});

        // @brief Render the debug UI
        // @param open Pointer to boolean controlling window visibility
        // @param title Window title
        void RenderDebugView(bool* open = nullptr, const char* title = "Shader Debugger");

        // @brief Get shader information by renderer ID
        // @param rendererID OpenGL shader program ID
        // @return Pointer to shader info or nullptr if not found
        const ShaderInfo* GetShaderInfo(u32 rendererID) const;

        // @brief Get all tracked shaders
        // @return Map of renderer ID to shader info
        const std::unordered_map<u32, ShaderInfo>& GetAllShaders() const
        {
            return m_Shaders;
        }

        // @brief Whether this BUILD tracks shaders at all.
        //
        // The OLO_SHADER_REGISTER macros compile to nothing outside OLO_DEBUG, so
        // in a Release or Dist build the tracked set is permanently empty. Callers
        // MUST consult this before reading a count: an empty map means "this build
        // does not track shaders", not "there are no shaders" -- and for the error
        // list it means "unknown", not "clean". Reporting the second as though it
        // were the first is how a Release editor came to be cited as evidence of
        // zero shader errors.
        [[nodiscard]] static constexpr bool IsTrackingCompiledIn() noexcept
        {
#ifdef OLO_DEBUG
            return true;
#else
            return false;
#endif
        }

        // @brief Whether tracking is compiled in AND Initialize() has run.
        [[nodiscard]] bool IsTracking() const noexcept
        {
            return IsTrackingCompiledIn() && m_IsInitialized;
        }

        // @brief Export shader debugging report
        // @param filePath Output file path
        // @return True if export succeeded
        bool ExportReport(const std::string& filePath) const;

        // @brief Update resource binding information for a shader
        // @param rendererID OpenGL shader program ID
        // @param resourceName Name of the resource
        // @param bindingInfo String description of the binding
        void UpdateResourceBinding(u32 rendererID, const std::string& resourceName, ShaderResourceType type, u32 bindingPoint, bool isBound);

        // @brief Clear all resource bindings for a shader
        // @param rendererID OpenGL shader program ID
        void ClearResourceBindings(u32 rendererID);

      private:
        ShaderDebugger() = default;
        ~ShaderDebugger() = default;

        // Non-copyable
        ShaderDebugger(const ShaderDebugger&) = delete;
        ShaderDebugger& operator=(const ShaderDebugger&) = delete; // UI rendering methods
        void RenderShaderList();
        void RenderShaderDetails(const ShaderInfo& shaderInfo);
        void RenderSourceCode(const ShaderInfo& shaderInfo) const;
        void RenderUniforms(const ShaderInfo& shaderInfo) const;
        void RenderResourceBindings(const ShaderInfo& shaderInfo) const;
        void RenderPerformanceMetrics(const ShaderInfo& shaderInfo) const;
        void RenderReloadHistory(const ShaderInfo& shaderInfo) const;
        void RenderSPIRVAnalysis(const ShaderInfo& shaderInfo) const;
        void RenderCompilationErrors(const ShaderInfo& shaderInfo) const;

        // Helper methods
        void UpdateActiveTime(ShaderInfo& shaderInfo) const;
        std::string GetUniformTypeString(UniformType type) const;
        std::string GetShaderStageString(ShaderStage stage) const;
        ImVec4 GetShaderStageColor(ShaderStage stage) const;
        void AnalyzeSPIRV(std::span<const u8> spirvData, u32& instructionCount) const;
        void AnalyzeSPIRVFromWords(std::span<const u32> spirvWords, u32& instructionCount) const;

        // Advanced SPIR-V analysis methods
        std::string GenerateSPIRVDisassembly(std::span<const u8> spirvData) const;
        void PerformOptimizationAnalysis(std::span<const u8> spirvData) const;

        // Data members
        bool m_IsInitialized = false;
        mutable FMutex m_ShaderMutex;
        std::unordered_map<u32, ShaderInfo> m_Shaders;

        // Compilation tracking
        struct PendingCompilation
        {
            FString m_Name;
            FString m_FilePath;
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

    // NOTE (#691): the GLenum-taking GLStageToShaderStage helper that
    // used to live here moved to its one caller, Platform/OpenGL/OpenGLShader.cpp
    // — this header is graphics-API-neutral and includes no <glad/gl.h>.
} // namespace OloEngine
