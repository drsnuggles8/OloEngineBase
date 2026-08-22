#pragma once
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
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

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            // Same block-finalize as GetRendererID above: the handle is only
            // minted once EnsureLinked has produced a program to mint it for, so
            // an async-compiling shader must be forced to completion first or
            // this hands back the null handle.
            if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
            {
                const_cast<OpenGLShader*>(this)->EnsureLinked();
            }
            return m_RHIHandle.Get();
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

        // Returns false if any shader stage failed to compile (already logged via
        // OLO_CORE_CRITICAL). Callers must not proceed to link/finalize on failure —
        // set m_CompilationStatus to Failed and bail instead (see issue #568).
        [[nodiscard]] bool CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources);
        [[nodiscard]] bool CompileOrGetOpenGLBinaries();
        void CreateProgram();

        // Returns false if any stage failed to compile; any shader objects already
        // attached to |program| during this call are detached/deleted before
        // returning so the caller is left with a clean, empty program.
        [[nodiscard]] bool CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const;
        void CreateProgramForAmd();

        void Reflect(GLenum stage, const std::vector<u32>& shaderData);

        // Helper to finalize a compiled shader program with registration, memory tracking, and SPIR-V decompilation
        void FinalizeProgram(GLenum const& program, const std::unordered_map<GLenum, std::vector<u32>>& spirvMap);

        // Async link helpers — called after glLinkProgram() returns (non-blocking with extension)
        void FinalizeAfterLink();            // Check link status, cache binary, call FinalizeProgram()
        void SaveProgramBinaryCache() const; // Extract & save program binary to disk cache

        // Loads the cached program binary into |program| (a freshly created, empty program
        // object) and verifies it links. Returns true only on a fresh, well-framed cache that
        // links cleanly; returns false — without asserting — on a missing/disabled/stale cache,
        // a corrupt or truncated file, or a soft link failure, leaving the caller to recompile.
        // Shared by CreateProgram() and CreateProgramForAmd() so the on-disk framing is parsed
        // in exactly one place (see issue #267).
        [[nodiscard]] bool LoadProgramBinaryCache(GLenum program) const;

        // Which on-disk program-binary cache this shader's CURRENT variant owns.
        // The two variants must never share a file: the driver stamps the binary
        // with its own version, not with which GLSL branch produced it, so a
        // bindless binary loaded into a slot-based run would link cleanly and
        // sample nothing.
        [[nodiscard]] auto ProgramBinaryCacheSuffix() const -> std::string
        {
            return m_IsBindlessVariant ? ".cached_opengl.bindless.pgr" : ".cached_opengl.pgr";
        }

        // ---------------------------------------------------------------------
        // The heap-bindless compile route (issue #691).
        //
        // A shader written against include/BindlessHeap.glsl cannot travel the
        // normal path at ALL: tier 1 targets Vulkan SPIR-V and glslang rejects
        // GL_ARB_bindless_texture outright ("not allowed when using generating
        // SPIR-V codes" — pinned by BindlessShaderPipelineTest). So the bindless
        // variant is compiled from the ORIGINAL, include-resolved GLSL straight
        // through glShaderSource, skipping shaderc and SPIRV-Cross entirely.
        //
        // Two consequences a reader should expect rather than discover:
        //
        //   * NO SPIR-V, therefore no Reflect(). The five SPIR-V-reading shader
        //     tests still cover this file's SHARED declarations through the
        //     default variant (one source file, two builds), so what goes
        //     unvalidated is only the bindless branch's own declarations.
        //   * THE PROGRAM-BINARY CACHE MUST BE KEYED ON THE VARIANT. Loading a
        //     bindless binary into a slot-based run would be silently wrong, and
        //     the driver cannot tell them apart — same shader file, same driver
        //     stamp. This is the "encode the target env in the cache filename"
        //     lesson from ADR 0011 section 3, one variant axis over.
        // ---------------------------------------------------------------------

        // True when the source opts in (`OLO_BINDLESS` appears) AND the heap is
        // actually enabled. Checked once at compile time: flipping the runtime
        // toggle afterwards needs a shader reload to take effect.
        [[nodiscard]] static bool WantsBindlessVariant(const std::unordered_map<GLenum, std::string>& sources);

        // Compiles + links the bindless variant. Returns false on any failure,
        // and the caller then falls back to the ordinary path — a broken
        // bindless branch must cost the frame its optimisation, never its shader.
        [[nodiscard]] bool CreateProgramFromRawGLSL(const std::unordered_map<GLenum, std::string>& sources);

      private:
        u32 m_RendererID{};
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        std::string m_Name;
        std::string m_FilePath;
        std::unordered_map<GLenum, std::vector<u32>> m_VulkanSPIRV;
        std::unordered_map<GLenum, std::vector<u32>> m_OpenGLSPIRV;

        std::unordered_map<GLenum, std::string> m_OpenGLSourceCode;
        std::unordered_map<GLenum, std::string> m_OriginalSourceCode; // Store original preprocessed source

        // True when this program was built by CreateProgramFromRawGLSL, i.e. it
        // indexes the descriptor heap rather than sampler binding points. Read
        // by the program-binary cache (which must not mix the two variants) and
        // exposed so a pass can assert it is not writing heap offsets at a
        // program that has no heap in it.
        bool m_IsBindlessVariant = false;
        // True when this program declares u_MaterialHeapOffsets, i.e. reads its
        // MATERIAL textures from the material UBO rather than from sampler
        // bindings. NOT the same as m_IsBindlessVariant — see
        // Shader::ReadsMaterialHeapOffsets (issue #691).
        bool m_ReadsMaterialHeapOffsets = false;

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
