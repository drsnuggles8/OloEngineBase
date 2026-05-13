#pragma once
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include <glm/glm.hpp>
#include <filesystem>
#include <unordered_set>

namespace OloEngine
{
    class OpenGLShader : public Shader
    {
        using GLenum = unsigned int;

      public:
        OpenGLShader(const std::string& filepath);
        OpenGLShader(std::string name, std::string_view vertexSrc, std::string_view fragmentSrc);
        ~OpenGLShader() override;

        void Bind() const override;
        void Unbind() const override;

        void SetInt(const std::string& name, int value) const override;
        void SetIntArray(const std::string& name, int* values, u32 count) const override;
        void SetFloat(const std::string& name, f32 value) const override;
        void SetFloat2(const std::string& name, const glm::vec2& value) const override;
        void SetFloat3(const std::string& name, const glm::vec3& value) const override;
        void SetFloat4(const std::string& name, const glm::vec4& value) const override;
        void SetMat4(const std::string& name, const glm::mat4& value) const override;

        [[nodiscard]] u32 GetRendererID() const override
        {
            // If async link hasn't completed yet, block-finalize before returning the ID.
            // This protects code paths (e.g. command dispatch) that use the raw program ID
            // without going through Bind().
            if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
            {
                const_cast<OpenGLShader*>(this)->EnsureLinked();
            }
            return m_RendererID;
        }
        [[nodiscard("Store this!")]] const std::string& GetName() const override
        {
            return m_Name;
        }
        [[nodiscard("Store this!")]] const std::string& GetFilePath() const override
        {
            return m_FilePath;
        }

        void Reload() override;

        // --- Async compilation status (override base class) ---
        [[nodiscard]] ShaderCompilationStatus GetCompilationStatus() const override
        {
            return m_CompilationStatus;
        }
        // IsReady opportunistically polls async link completion when called.
        // Without this, shaders that aren't tracked by ShaderLibrary (e.g.
        // owned directly by a render pass) stay in Compiling forever because
        // ShaderLibrary::PollPendingShaders only iterates library-registered
        // shaders. Mirrors the GetRendererID() const_cast → EnsureLinked
        // pattern below; same single-threaded GL context assumption applies.
        [[nodiscard]] bool IsReady() const override
        {
            if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
                const_cast<OpenGLShader*>(this)->PollCompilationStatus();
            return m_CompilationStatus == ShaderCompilationStatus::Ready;
        }
        bool PollCompilationStatus() override;
        void EnsureLinked() override;

        // Populated during SPIR-V reflection (Reflect()) by scanning the
        // fragment stage's declared outputs for G-Buffer MRT markers.
        // Stable once IsReady() returns true — Reflect() runs before the
        // program is marked Ready.
        [[nodiscard]] bool IsDeferredCapable() const override
        {
            return m_IsDeferredCapable;
        }

        // Resource registry access (override base class virtual methods)
        ShaderResourceRegistry* GetResourceRegistry() override
        {
            return &m_ResourceRegistry;
        }
        const ShaderResourceRegistry* GetResourceRegistry() const override
        {
            return &m_ResourceRegistry;
        }

        void UploadUniformInt(const std::string& name, int value) const;
        void UploadUniformIntArray(const std::string& name, int const* values, u32 count) const;
        void UploadUniformFloat(const std::string& name, f32 value) const;
        void UploadUniformFloat2(const std::string& name, const glm::vec2& value) const;
        void UploadUniformFloat3(const std::string& name, const glm::vec3& value) const;
        void UploadUniformFloat4(const std::string& name, const glm::vec4& value) const;

        void UploadUniformMat3(const std::string& name, const glm::mat3& matrix) const;
        void UploadUniformMat4(const std::string& name, const glm::mat4& matrix) const;

        // Initialize resource registry (called after shader is fully constructed)
        void InitializeResourceRegistry(const Ref<Shader>& shaderRef);

        // Convenience methods for setting shader resources
        template<typename T>
        bool SetShaderResource(const std::string& name, const Ref<T>& resource)
        {
            return m_ResourceRegistry.SetResource(name, resource);
        }

        bool SetShaderResource(const std::string& name, const ShaderResourceInput& input)
        {
            return m_ResourceRegistry.SetResource(name, input);
        }

        // SPIR-V data access (for shader pack serialization)
        [[nodiscard]] const std::unordered_map<GLenum, std::vector<u32>>& GetVulkanSPIRV() const
        {
            return m_VulkanSPIRV;
        }
        [[nodiscard]] const std::unordered_map<GLenum, std::vector<u32>>& GetOpenGLSPIRV() const
        {
            return m_OpenGLSPIRV;
        }

        // Include processing — public so compute shaders can reuse it
        static std::string ProcessIncludes(const std::string& source, const std::string& directory = "");
        static std::string ProcessIncludes(const std::string& source, const std::string& directory, std::vector<std::string>& outIncludePaths);

        // Create a shader from pre-compiled SPIR-V data (loaded from a shader pack).
        // Skips file I/O, preprocessing, and SPIR-V compilation entirely.
        static Ref<Shader> CreateFromPackData(
            const std::string& name,
            const std::string& filepath,
            std::unordered_map<GLenum, std::vector<u32>> vulkanSPIRV,
            std::unordered_map<GLenum, std::vector<u32>> openGLSPIRV);

      private:
        // Tag type for the pack-data constructor (internal only)
        struct PackDataTag
        {
        };

        // Private constructor — creates a shader from pre-compiled SPIR-V (no file I/O or compilation)
        OpenGLShader(PackDataTag,
                     const std::string& name,
                     const std::string& filepath,
                     std::unordered_map<GLenum, std::vector<u32>> vulkanSPIRV,
                     std::unordered_map<GLenum, std::vector<u32>> openGLSPIRV);

        static std::string ReadFile(const std::string& filepath);
        static std::string ProcessIncludesInternal(const std::string& source, const std::string& directory, std::unordered_set<std::string>& includedFiles);
        std::unordered_map<GLenum, std::string> PreProcess(std::string_view source);

        void CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources);
        void CompileOrGetOpenGLBinaries();
        void CreateProgram();

        void CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const;
        void CreateProgramForAmd();

        void Reflect(GLenum stage, const std::vector<u32>& shaderData);

        // Helper to finalize a compiled shader program with registration, memory tracking, and SPIR-V decompilation
        void FinalizeProgram(GLenum const& program, const std::unordered_map<GLenum, std::vector<u32>>& spirvMap);

        // Async link helpers — called after glLinkProgram() returns (non-blocking with extension)
        void FinalizeAfterLink();            // Check link status, cache binary, call FinalizeProgram()
        void SaveProgramBinaryCache() const; // Extract & save program binary to disk cache

      private:
        u32 m_RendererID{};
        std::string m_Name;
        std::string m_FilePath;
        std::unordered_map<GLenum, std::vector<u32>> m_VulkanSPIRV;
        std::unordered_map<GLenum, std::vector<u32>> m_OpenGLSPIRV;

        std::unordered_map<GLenum, std::string> m_OpenGLSourceCode;
        std::unordered_map<GLenum, std::string> m_OriginalSourceCode; // Store original preprocessed source

        // Paths resolved during #include expansion — used to invalidate shader
        // cache when any include file is modified (not just the main .glsl).
        std::vector<std::string> m_IncludedFilePaths;

        // Returns true when the cached binary at |cachedPath| is older than the
        // main shader source OR any of its transitive #include dependencies.
        [[nodiscard]] bool IsCacheStale(const std::filesystem::path& cachedPath) const;

        // Resource registry for automatic resource management
        ShaderResourceRegistry m_ResourceRegistry;

        // --- Async compilation state ---
        ShaderCompilationStatus m_CompilationStatus = ShaderCompilationStatus::Ready;
        f64 m_DeferredCompilationTime = 0.0;
        bool m_TrackedAllocation = false;

        // Set during fragment-stage SPIR-V reflection when any declared
        // stage output matches the G-Buffer marker names. Drives
        // `IsDeferredCapable()`; see the base class declaration for the
        // marker set and rationale.
        bool m_IsDeferredCapable = false;

        // Shader stage IDs kept alive until link completes (then detached/deleted)
        std::vector<u32> m_PendingShaderIDs;
    };

} // namespace OloEngine
