#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "Platform/OpenGL/OpenGLContext.h"
#include "Platform/OpenGL/OpenGLDebug.h"
#include "Platform/OpenGL/OpenGLProgramBinaryCache.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderRegistry.h"
#include "OloEngine/Task/ParallelFor.h"

#include <cstring>
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>

#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <chrono>

#include <atomic>

// GL_COMPLETION_STATUS_ARB / KHR — same token value
#ifndef GL_COMPLETION_STATUS_ARB
#define GL_COMPLETION_STATUS_ARB 0x91B1
#endif

namespace OloEngine
{
    namespace Utils
    {
        // Does this fragment source declare a G-BUFFER OUTPUT?
        //
        // The bindless route has no SPIR-V, so `Reflect()` never runs and cannot
        // set `m_IsDeferredCapable` from SPIRV-Cross's `stage_outputs`. This reads
        // the same fact off the source. It must mirror Reflect()'s criteria — a
        // name starting with `o_GBuffer`, or one of the three sentinels — because
        // a shader classified differently by the two routes would render through a
        // different bucket depending only on whether the heap is on (issue #691).
        //
        // It matches a DECLARATION, not a mention: the name must be the declared
        // identifier of an `out` statement. Scanning for the bare name would also
        // fire on a shader that merely SAMPLES a G-Buffer target — DeferredLighting
        // reads `gAlbedo` — and would misclassify a consumer as a producer.
        [[nodiscard]] static bool DeclaresGBufferOutput(std::string_view source, std::string_view prefix,
                                                        std::span<const std::string_view> sentinels)
        {
            for (sizet lineStart = 0; lineStart < source.size();)
            {
                const sizet lineEnd = std::min(source.find('\n', lineStart), source.size());
                const std::string_view line = source.substr(lineStart, lineEnd - lineStart);
                lineStart = lineEnd + 1u;

                // Strip a line comment so `// writes o_GBufferAlbedo` cannot match.
                const std::string_view code = line.substr(0, std::min(line.find("//"), line.size()));

                // An `out` declaration, as a whole word — not `layout`, not `output`.
                sizet outPos = code.find("out ");
                bool isDeclaration = false;
                while (outPos != std::string_view::npos)
                {
                    const bool leftOk = (outPos == 0) ||
                                        !(std::isalnum(static_cast<unsigned char>(code[outPos - 1])) ||
                                          code[outPos - 1] == '_');
                    if (leftOk)
                    {
                        isDeclaration = true;
                        break;
                    }
                    outPos = code.find("out ", outPos + 1u);
                }
                if (!isDeclaration)
                {
                    continue;
                }

                // The declared name is the last identifier before the ';'.
                const sizet semi = code.find(';', outPos);
                if (semi == std::string_view::npos)
                {
                    continue;
                }
                sizet nameEnd = semi;
                while (nameEnd > outPos && std::isspace(static_cast<unsigned char>(code[nameEnd - 1])))
                {
                    --nameEnd;
                }
                sizet nameStart = nameEnd;
                while (nameStart > outPos && (std::isalnum(static_cast<unsigned char>(code[nameStart - 1])) ||
                                              code[nameStart - 1] == '_'))
                {
                    --nameStart;
                }
                const std::string_view name = code.substr(nameStart, nameEnd - nameStart);
                if (name.empty())
                {
                    continue;
                }

                if (name.starts_with(prefix))
                {
                    return true;
                }
                for (const std::string_view sentinel : sentinels)
                {
                    if (name == sentinel)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        static std::atomic<bool> s_DisableShaderCache{ false }; // Debug flag to disable shader caching

        // Debug API to control shader cache (exposed for external use)
        void SetDisableShaderCache(bool disable)
        {
            s_DisableShaderCache.store(disable, std::memory_order_relaxed);
        }

        bool IsShaderCacheDisabled()
        {
            return s_DisableShaderCache.load(std::memory_order_relaxed);
        }

        static GLenum ShaderTypeFromString(std::string_view type)
        {
            if (type == "vertex")
            {
                return GL_VERTEX_SHADER;
            }
            if ((type == "fragment") || (type == "pixel"))
            {
                return GL_FRAGMENT_SHADER;
            }
            if ((type == "tess_control") || (type == "tesscontrol"))
            {
                return GL_TESS_CONTROL_SHADER;
            }
            if ((type == "tess_evaluation") || (type == "tesseval"))
            {
                return GL_TESS_EVALUATION_SHADER;
            }

            OLO_CORE_ERROR("Unknown shader type: '{0}' (length: {1})", std::string(type), type.length());
            OLO_CORE_ASSERT(false, "Unknown shader type!");
            return 0;
        }

        [[nodiscard("Store this!")]] static shaderc_shader_kind GLShaderStageToShaderC(const GLenum stage)
        {
            switch (stage)
            {
                case GL_VERTEX_SHADER:
                {
                    return shaderc_glsl_vertex_shader;
                }
                case GL_FRAGMENT_SHADER:
                {
                    return shaderc_glsl_fragment_shader;
                }
                case GL_TESS_CONTROL_SHADER:
                {
                    return shaderc_glsl_tess_control_shader;
                }
                case GL_TESS_EVALUATION_SHADER:
                {
                    return shaderc_glsl_tess_evaluation_shader;
                }
            }
            OLO_CORE_ASSERT(false);
            return static_cast<shaderc_shader_kind>(0);
        }

        [[nodiscard("Store this!")]] static const char* GLShaderStageToString(const GLenum stage)
        {
            switch (stage)
            {
                case GL_VERTEX_SHADER:
                {
                    return "GL_VERTEX_SHADER";
                }
                case GL_FRAGMENT_SHADER:
                {
                    return "GL_FRAGMENT_SHADER";
                }
                case GL_TESS_CONTROL_SHADER:
                {
                    return "GL_TESS_CONTROL_SHADER";
                }
                case GL_TESS_EVALUATION_SHADER:
                {
                    return "GL_TESS_EVALUATION_SHADER";
                }
            }
            OLO_CORE_ASSERT(false);
            return "Unknown";
        }

        [[nodiscard("Store this!")]] static const char* GetCacheDirectory()
        {
            if (const std::filesystem::path assetsDirectory = "assets"; !std::filesystem::exists(assetsDirectory))
            {
                OLO_CORE_ERROR("The assets directory does not exist.");
                return nullptr;
            }

            return "assets/cache/shader/opengl";
        }

        static void CreateCacheDirectoryIfNeeded()
        {
            const std::filesystem::path cacheDirectory = GetCacheDirectory();
            if (!std::filesystem::exists(cacheDirectory))
            {
                std::filesystem::create_directories(cacheDirectory);
            }
        }

        [[nodiscard("Store this!")]] static const char* GLShaderStageCachedOpenGLFileExtension(const u32 stage)
        {
            switch (stage)
            {
                case GL_VERTEX_SHADER:
                {
                    return ".cached_opengl.vert";
                }
                case GL_FRAGMENT_SHADER:
                {
                    return ".cached_opengl.frag";
                }
                case GL_TESS_CONTROL_SHADER:
                {
                    return ".cached_opengl.tesc";
                }
                case GL_TESS_EVALUATION_SHADER:
                {
                    return ".cached_opengl.tese";
                }
            }
            OLO_CORE_ASSERT(false);
            return "";
        }

        [[nodiscard("Store this!")]] static const char* GLShaderStageCachedVulkanFileExtension(const u32 stage)
        {
            switch (stage)
            {
                case GL_VERTEX_SHADER:
                {
                    return ".cached_vulkan.vert";
                }
                case GL_FRAGMENT_SHADER:
                {
                    return ".cached_vulkan.frag";
                }
                case GL_TESS_CONTROL_SHADER:
                {
                    return ".cached_vulkan.tesc";
                }
                case GL_TESS_EVALUATION_SHADER:
                {
                    return ".cached_vulkan.tese";
                }
            }
            OLO_CORE_ASSERT(false);
            return "";
        }

        static bool IsAmdGpu()
        {
            const auto* const vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            return std::strstr(vendor, "ATI") != nullptr || std::strstr(vendor, "AMD") != nullptr;
        }
    } // namespace Utils

    OpenGLShader::OpenGLShader(const std::string& filepath)
        : m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

        m_CompilationStatus = ShaderCompilationStatus::Pending;

        Utils::CreateCacheDirectoryIfNeeded();
        const std::string source = ReadFile(filepath);

        const auto shaderSources = PreProcess(source);

        // Store original source code for debugging (after we have shader ID)
        // This will be called later after CreateProgram when m_RendererID is available

        // Extract shader name from filepath first
        auto lastSlash = filepath.find_last_of("/\\");
        const auto lastDot = filepath.rfind('.');
        lastSlash = lastSlash == std::string::npos ? 0 : (lastSlash + 1);
        const auto count = lastDot == std::string::npos ? (filepath.size() - lastSlash) : (lastDot - lastSlash);
        m_Name = filepath.substr(lastSlash, count);

        // Make this shader reloadable BY NAME regardless of who owns it — a
        // ShaderLibrary or a render pass member (issue #607). Registered before
        // the compile so a shader whose GLSL is broken at boot can still be
        // fixed on disk and reloaded over MCP instead of needing a restart.
        ShaderRegistry::Get().RegisterShader(m_Name, this);

        OLO_SHADER_COMPILATION_START(m_Name, filepath);
        const Timer timer;

        OLO_CORE_INFO("Compiling shader '{}' from '{}'", m_Name, filepath);

        // The bindless variant is tried FIRST and is allowed to decline. It
        // cannot share the path below — glslang rejects GL_ARB_bindless_texture
        // when generating SPIR-V, so tier 1 would fail on the very shaders this
        // route exists for (issue #691 Phase 3, BindlessShaderPipelineTest).
        // On any failure it falls through to the ordinary path, so a broken
        // bindless branch costs the optimisation and not the shader.
        if (WantsBindlessVariant(shaderSources) && CreateProgramFromRawGLSL(shaderSources))
        {
            const f64 bindlessTime = timer.ElapsedMillis();
            OLO_CORE_INFO("Shader creation took {0} ms (bindless route)", bindlessTime);
            OLO_SHADER_COMPILATION_END(m_RendererID, m_RendererID != 0, "", bindlessTime);
            return;
        }

        if (!CompileOrGetVulkanBinaries(shaderSources))
        {
            // A user-authored GLSL syntax/compile error is not an engineering
            // invariant violation — surface it as a Failed shader instead of
            // crashing the editor (issue #568). Already logged via OLO_CORE_CRITICAL.
            m_CompilationStatus = ShaderCompilationStatus::Failed;
        }
        else if (Utils::IsAmdGpu())
        {
            std::string fullVersion(reinterpret_cast<const char*>(glGetString(GL_VERSION)));

            if (sizet lastSpace = fullVersion.rfind(' '); lastSpace != std::string::npos)
            {
                std::string driverVersion = fullVersion.substr(lastSpace + 1);
                std::istringstream versionStream(driverVersion);
                std::string token;
                std::vector<int> versionNumbers;

                while (std::getline(versionStream, token, '.'))
                {
                    versionNumbers.push_back(std::stoi(token));
                }

                if (versionNumbers[0] < 23 || (versionNumbers[0] == 23 && versionNumbers[1] < 5) || (versionNumbers[0] == 23 && versionNumbers[1] == 5 && versionNumbers[2] < 2))
                {
                    CreateProgramForAmd();
                }
                else if (CompileOrGetOpenGLBinaries())
                {
                    CreateProgram();
                }
                else
                {
                    m_CompilationStatus = ShaderCompilationStatus::Failed;
                }
            }
            else
            {
                OLO_CORE_ERROR("Could not find driver version in string: '{0}'", fullVersion);
                m_CompilationStatus = ShaderCompilationStatus::Failed;
            }
        }
        else if (CompileOrGetOpenGLBinaries())
        {
            CreateProgram();
        }
        else
        {
            m_CompilationStatus = ShaderCompilationStatus::Failed;
        }
        const f64 compilationTime = timer.ElapsedMillis();

        // If the shader is still Compiling (async path), report that; otherwise it's Ready/Failed
        if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
        {
            OLO_CORE_INFO("Shader '{}' link issued asynchronously ({:.1f} ms CPU work)", m_Name, compilationTime);
        }
        else
        {
            OLO_CORE_INFO("Shader creation took {0} ms", compilationTime);
        }

        // Register with shader debugger and report compilation
        // For async shaders (still Compiling), defer COMPILATION_END to FinalizeAfterLink
        // where the shader is registered via FinalizeProgram first.
        if (m_CompilationStatus != ShaderCompilationStatus::Compiling)
        {
            OLO_SHADER_COMPILATION_END(m_RendererID, m_RendererID != 0, "", compilationTime);
        }
        else
        {
            m_DeferredCompilationTime = compilationTime;
        }
    }

    OpenGLShader::OpenGLShader(std::string name, std::string_view vertexSrc, std::string_view fragmentSrc)
        : m_Name(std::move(name)), m_FilePath(m_Name) // Use name as pseudo-path so shaderc message parser has a valid filename
    {
        OLO_PROFILE_FUNCTION();

        m_CompilationStatus = ShaderCompilationStatus::Pending;

        std::unordered_map<GLenum, std::string> sources;
        sources[GL_VERTEX_SHADER] = vertexSrc;
        sources[GL_FRAGMENT_SHADER] = fragmentSrc;

        OLO_SHADER_COMPILATION_START(m_Name, "runtime_source");

        if (Utils::IsAmdGpu())
        {
            // AMD path: compile GLSL source strings directly (no SPIR-V)
            m_OriginalSourceCode = sources;
            if (CompileOrGetVulkanBinaries(sources))
            {
                CreateProgramForAmd();
            }
            else
            {
                m_CompilationStatus = ShaderCompilationStatus::Failed;
            }
        }
        else if (CompileOrGetVulkanBinaries(sources) && CompileOrGetOpenGLBinaries())
        {
            CreateProgram();
        }
        else
        {
            m_CompilationStatus = ShaderCompilationStatus::Failed;
        }

        // Source-string shaders (boot/fallback) must be ready immediately —
        // the boot shader is needed to render the warmup progress bar, and
        // the fallback shader is needed for substitution. Force-complete
        // any async link now (safe: we're not inside any ShaderDebugger lock).
        if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
        {
            EnsureLinked();
        }

        OLO_CORE_INFO("Source-string shader '{}' constructor done, status={}, rendererID={}",
                      m_Name, static_cast<int>(m_CompilationStatus), m_RendererID);

        // Register compilation completion
        OLO_SHADER_COMPILATION_END(m_RendererID, m_RendererID != 0, "", 0.0);
    }

    Ref<Shader> OpenGLShader::CreateFromPackData(
        const std::string& name,
        const std::string& filepath,
        std::unordered_map<GLenum, std::vector<u32>> vulkanSPIRV,
        std::unordered_map<GLenum, std::vector<u32>> openGLSPIRV)
    {
        OLO_PROFILE_FUNCTION();

        auto* raw = new OpenGLShader(PackDataTag{},
                                     name, filepath,
                                     std::move(vulkanSPIRV),
                                     std::move(openGLSPIRV));
        return Ref<Shader>(raw);
    }

    OpenGLShader::OpenGLShader(PackDataTag,
                               const std::string& name,
                               const std::string& filepath,
                               std::unordered_map<GLenum, std::vector<u32>> vulkanSPIRV,
                               std::unordered_map<GLenum, std::vector<u32>> openGLSPIRV)
        : m_Name(name), m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

        m_CompilationStatus = ShaderCompilationStatus::Pending;
        m_VulkanSPIRV = std::move(vulkanSPIRV);
        m_OpenGLSPIRV = std::move(openGLSPIRV);

        // Pack-loaded shaders keep their on-disk source path, so they are
        // reloadable by name too (issue #607).
        ShaderRegistry::Get().RegisterShader(m_Name, this);

        // Run reflection on Vulkan SPIR-V (extracts UBO/texture bindings)
        for (auto&& [stage, data] : m_VulkanSPIRV)
        {
            Reflect(stage, data);
        }

        // Create GL program from OpenGL SPIR-V (may go async)
        CreateProgram();

        // Force synchronous link — pack-loaded shaders should be instantly usable
        if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
        {
            EnsureLinked();
        }

        OLO_CORE_INFO("[ShaderPack] Created shader '{}' from pack data", m_Name);
    }

    OpenGLShader::~OpenGLShader()
    {
        OLO_PROFILE_FUNCTION();

        // Drop the name->shader entry BEFORE this address can be recycled by
        // another allocation — the liveness side-table WeakRef::Lock() consults
        // is keyed by address, so a stale entry could otherwise be locked into a
        // Ref<Shader> pointing at something that is not a shader (issue #607;
        // docs/agent-rules/intrusive-refcount-weakref-races.md).
        ShaderRegistry::Get().UnregisterShader(this);

        // Clean up pending shader objects if never finalized
        // These are intermediate compile artifacts, safe to delete immediately
        for (const auto id : m_PendingShaderIDs)
        {
            glDetachShader(m_RendererID, id);
            glDeleteShader(id);
        }
        m_PendingShaderIDs.clear();

        // Unregister the resource registry from Renderer3D
        // Unregister the resource registry from the shader-system registry map
        if (m_RendererID != 0)
        {
            ShaderResourceRegistry::Unregister(m_RendererID);
        }

        // Shutdown the resource registry
        m_ResourceRegistry.Shutdown();

        // Unregister from shader debugger
        OLO_SHADER_UNREGISTER(m_RendererID);

        // Track GPU memory deallocation — only if FinalizeProgram ran (which tracks the alloc).
        // Async shaders destroyed while still Compiling were never allocated in the tracker.
        if (m_TrackedAllocation)
        {
            OLO_TRACK_DEALLOC(this);
        }

        u32 programId = m_RendererID;
        UnregisterGLProgramLabel(programId);
        FrameResourceManager::Get().SubmitForDeletion([programId]()
                                                      {
                                                          // See Utils::UnbindProgramIfCurrent (issue #625): this
                                                          // program may still be the bound program by the time this
                                                          // deferred deletion runs.
                                                          Utils::UnbindProgramIfCurrent(programId);
                                                          Shader::UnregisterProgram(programId);
                                                          glDeleteProgram(programId); });
    }

    void OpenGLShader::InitializeResourceRegistry(const Ref<Shader>& shaderRef)
    {
        OLO_CORE_TRACE("OpenGLShader: InitializeResourceRegistry called for shader '{0}'", m_Name);
        m_ResourceRegistry.SetShader(shaderRef);
        m_ResourceRegistry.Initialize();
        if (m_RendererID != 0)
        {
            ShaderResourceRegistry::Register(m_RendererID, &m_ResourceRegistry);
        }
        OLO_CORE_TRACE("OpenGLShader: Initialized resource registry for shader '{0}'", m_Name);
    }

    std::string OpenGLShader::ReadFile(const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        std::string result;
        if (std::ifstream in(filepath, std::ios::in | std::ios::binary); in)
        {
            in.seekg(0, std::ios::end);
            sizet const size = in.tellg();
            if (std::cmp_not_equal(size, -1))
            {
                result.resize(size);
                in.seekg(0, std::ios::beg);
                in.read(&result[0], size);
            }
            else
            {
                OLO_CORE_ERROR("Could not read from file '{0}'", filepath);
            }
        }
        else
        {
            OLO_CORE_ERROR("Could not open file '{0}'", filepath);
        }

        return result;
    }

    std::string OpenGLShader::ProcessIncludes(const std::string& source, const std::string& directory)
    {
        std::unordered_set<std::string> includedFiles;
        return ProcessIncludesInternal(source, directory, includedFiles);
    }

    std::string OpenGLShader::ProcessIncludes(const std::string& source, const std::string& directory, std::vector<std::string>& outIncludePaths)
    {
        std::unordered_set<std::string> includedFiles;
        auto result = ProcessIncludesInternal(source, directory, includedFiles);
        outIncludePaths.assign(includedFiles.begin(), includedFiles.end());
        return result;
    }

    std::string OpenGLShader::ProcessIncludesInternal(const std::string& source, const std::string& directory, std::unordered_set<std::string>& includedFiles)
    {
        OLO_PROFILE_FUNCTION();

        std::stringstream result;
        std::istringstream stream(source);
        std::string line;

        while (std::getline(stream, line))
        {
            // Check for #include directive
            const std::string includeToken = "#include";
            const auto pos = line.find(includeToken);

            // Skip #include directives that appear inside line comments
            const auto commentPos = line.find("//");
            const bool isCommented = (commentPos != std::string::npos && pos != std::string::npos && commentPos < pos);

            if (pos != std::string::npos && !isCommented)
            {
                // Extract the include file path
                const auto start = line.find_first_of("\"<", pos + includeToken.length());
                const auto end = line.find_first_of("\">", start + 1);

                if (start != std::string::npos && end != std::string::npos)
                {
                    const std::string includePath = line.substr(start + 1, end - start - 1);

                    // Resolve the full path
                    std::filesystem::path fullPath;
                    if (directory.empty())
                    {
                        // Try relative to shader assets directory
                        fullPath = std::filesystem::path("assets/shaders") / includePath;
                    }
                    else
                    {
                        fullPath = std::filesystem::path(directory) / includePath;
                    }

                    const std::string fullPathStr = fullPath.string();

                    // Check for circular includes
                    if (includedFiles.find(fullPathStr) != includedFiles.end())
                    {
                        OLO_CORE_WARN("Circular include detected for: {0}", fullPathStr);
                        result << "// Circular include: " << includePath << "\n";
                        continue;
                    }

                    // Add to included files set
                    includedFiles.insert(fullPathStr);

                    // Read and recursively process the included file
                    const std::string includeContent = ReadFile(fullPathStr);
                    if (!includeContent.empty())
                    {
                        const std::string includeDir = fullPath.parent_path().string();
                        const std::string processedInclude = ProcessIncludesInternal(includeContent, includeDir, includedFiles);
                        result << processedInclude << "\n";
                    }
                    else
                    {
                        OLO_CORE_ERROR("Failed to read include file: {0}", fullPathStr);
                        result << "// Failed to include: " << includePath << "\n";
                    }
                }
                else
                {
                    OLO_CORE_WARN("Invalid include syntax: {0}", line);
                    result << line << "\n";
                }
            }
            else
            {
                result << line << "\n";
            }
        }

        return result.str();
    }

    bool OpenGLShader::IsCacheStale(const std::filesystem::path& cachedPath) const
    {
        std::error_code ec;
        auto const cacheTime = std::filesystem::last_write_time(cachedPath, ec);
        if (ec)
            return true; // Cannot stat cache file → treat as stale

        // Check the main shader file
        if (auto const shaderTime = std::filesystem::last_write_time(m_FilePath, ec); ec || shaderTime > cacheTime)
            return true;

        // Check every transitively included file
        for (const auto& includePath : m_IncludedFilePaths)
        {
            auto const includeTime = std::filesystem::last_write_time(includePath, ec);
            if (ec)
                continue; // Include might have been removed — skip
            if (includeTime > cacheTime)
                return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // The heap-bindless compile route (issue #691 Phase 3).
    // -------------------------------------------------------------------------

    bool OpenGLShader::WantsBindlessVariant(const std::unordered_map<GLenum, std::string>& sources)
    {
        // The heap has to be live before the decision, and it is: the backend
        // installs itself in OpenGLRendererAPI::Init, which runs before any
        // shader is compiled. A shader compiled while the toggle was off keeps
        // its slot-based program until it is reloaded — flipping SetEnabled at
        // runtime does not retro-actively rebuild programs, and pretending
        // otherwise would give a half-converted frame.
        if (!RHI::DescriptorHeap::Get().IsEnabled())
        {
            return false;
        }

        // Opt-in is the token itself. A shader that never mentions OLO_BINDLESS
        // has no bindless branch to build, so building one would just compile
        // the same slot-based code down a route with fewer safety nets.
        return std::ranges::any_of(sources,
                                   [](const auto& entry)
                                   { return entry.second.find("OLO_BINDLESS") != std::string::npos; });
    }

    bool OpenGLShader::CreateProgramFromRawGLSL(const std::unordered_map<GLenum, std::string>& sources)
    {
        OLO_PROFILE_FUNCTION();

        m_IsBindlessVariant = true;

        // SET HERE, BESIDE m_IsBindlessVariant, AND FOR THE SAME REASON: this
        // function returns early on a linked-program cache hit, so anything
        // derived further down is simply absent on a warm run. The flag would then
        // disagree with the shader actually executing — it reads material offsets
        // while CommandDispatch believes it does not, so the offsets are never
        // written and it samples the reserved null.
        //
        // AND THE MARKER IS AN EXPLICIT OPT-IN, because the obvious ones do not
        // work: `sources` is POST-INCLUDE-RESOLUTION, and include/BindlessHeap.glsl
        // both defines the material accessor macros and names the UBO field inside
        // them. Keying on either token marks every shader that merely INCLUDES the
        // header as a reader, which makes BindPBRTextures skip the material binds
        // engine-wide and renders meshes unlit with no error (issue #691 Phase 3).
        m_ReadsMaterialHeapOffsets =
            std::ranges::any_of(sources,
                                [](const auto& entry)
                                { return entry.second.find("OLO_MATERIAL_HEAP_READER") != std::string::npos; });

        const GLuint program = glCreateProgram();

        // The variant-keyed cache. Same reasoning as the SPIR-V tiers: a linked
        // program binary is the expensive artefact, and it is reusable here even
        // though nothing else about this route is — glGetProgramBinary does not
        // care how the program was built.
        if (LoadProgramBinaryCache(program))
        {
            FinalizeProgram(program, {});
            m_CompilationStatus = ShaderCompilationStatus::Ready;
            OLO_CORE_TRACE("[Bindless] Loaded bindless program from binary cache: {}", m_FilePath);
            return true;
        }

        Shader::UnregisterProgram(program);
        glDeleteProgram(program);
        const GLuint freshProgram = glCreateProgram();

        std::vector<GLuint> attached;
        attached.reserve(sources.size());

        bool ok = true;
        for (const auto& [stage, stageSource] : sources)
        {
            // Inject the define AND the extension directive immediately after
            // `#version`. Both have to be here rather than in
            // include/BindlessHeap.glsl, and the reason is a GLSL rule that bites
            // silently: an `#extension` directive must precede every
            // non-preprocessor token in the unit. A shader that includes the heap
            // header below its first `layout(...)` declaration — which is the
            // natural place to put it, right next to the samplers it replaces —
            // would be ill-formed, and whether that is diagnosed depends on the
            // driver. Injecting here makes include placement a non-issue across
            // every converted shader instead of a per-file rule nobody can see.
            //
            // GL_ARB_shader_draw_parameters is DELIBERATELY NOT ENABLED here — see
            // the shim below. Enabling it changed what `gl_InstanceIndex` resolves
            // to and silently desynchronised every instanced draw.
            static constexpr std::string_view kPrologue =
                "#extension GL_ARB_bindless_texture : require\n"
                "#define OLO_BINDLESS 1\n";

            std::string patched = stageSource;

            // THE VULKAN-BUILTIN SHIM, and it is the one transform that makes the
            // two variants MORE alike rather than less.
            //
            // The default route is shaderc(vulkan) -> SPIR-V -> SPIRV-Cross -> GLSL,
            // and SPIRV-Cross is what rewrites Vulkan's vertex builtins into their
            // GL spellings. This route bypasses SPIRV-Cross entirely, so without a
            // shim the Vulkan names reach the GL compiler verbatim:
            //
            //     error C7531: global variable gl_InstanceIndex requires
            //                  "#extension GL_KHR_vulkan_glsl : enable" before use
            //
            // and the shader degrades to the slot path with only a
            // `.bindless.failed.glsl` dump to show for it. Renaming in the .glsl is
            // NOT an option: the default path targets Vulkan, where the GL spellings
            // do not exist. The shader genuinely needs both, and something has to
            // bridge them.
            //
            // The replacements are SPIRV-Cross's own, taken from its output for
            // these very shaders rather than derived from the spec:
            //     gl_InstanceIndex -> (gl_InstanceID + gl_BaseInstanceARB)
            //     gl_VertexIndex   -> gl_VertexID
            // (SPIRV-Cross emits the base-instance term behind a
            // `SPIRV_Cross_BaseInstance` macro defined to `gl_BaseInstanceARB`; the
            // expression is inlined here because this route has no macro preamble
            // of its own to hang it on.)
            //
            // Substituting the TEXT rather than `#define`-ing the names is required,
            // not stylistic: GLSL reserves identifiers beginning with `gl_`, so
            // `#define gl_InstanceIndex ...` is ill-formed.
            struct VulkanBuiltinShim
            {
                std::string_view Vulkan;
                std::string_view OpenGL;
            };
            static constexpr std::array<VulkanBuiltinShim, 2> kVulkanBuiltinShims{ {
                // gl_InstanceID ALONE, deliberately — NOT `gl_InstanceID +
                // gl_BaseInstanceARB`, which is what SPIRV-Cross literally prints
                // and what an earlier version of this shim copied.
                //
                // SPIRV-Cross guards that term:
                //     #ifdef GL_ARB_shader_draw_parameters
                //     #define SPIRV_Cross_BaseInstance gl_BaseInstanceARB
                //     #else
                //     uniform int SPIRV_Cross_BaseInstance;   // never set
                //     #endif
                // and the engine's SPIR-V -> GL path does not enable that
                // extension, so every existing draw effectively indexes with
                // `gl_InstanceID + 0`. Enabling the extension here and using the
                // real base instance made this route disagree with the slot path
                // about which instance a vertex belongs to — every `u_Model`,
                // `u_Normal` and `u_PrevModel` in InstanceBlock_Vertex.glsl reads
                // `instances[gl_InstanceIndex]`, so the transforms and normals
                // came from the wrong entry.
                //
                // Measured on WorldOriginRebaseVisualEvidence: 10.097 RMSE with
                // the base-instance term, 5.861 without (the slot path is 1.413;
                // the remainder is the raw-GLSL compile path itself, not this).
                //
                // The rule: match what the engine's OTHER path actually COMPILES
                // TO, not what the translator prints in the abstract.
                { "gl_InstanceIndex", "gl_InstanceID" },
                { "gl_VertexIndex", "gl_VertexID" },
            } };
            for (const auto& [vulkanName, openglName] : kVulkanBuiltinShims)
            {
                for (sizet pos = patched.find(vulkanName); pos != std::string::npos;
                     pos = patched.find(vulkanName, pos + openglName.size()))
                {
                    // Identifier-boundary guard on both ends, so a longer name that
                    // merely CONTAINS one of these is left alone.
                    const bool leftOk = (pos == 0) || !(std::isalnum(static_cast<unsigned char>(patched[pos - 1])) ||
                                                        patched[pos - 1] == '_');
                    const sizet after = pos + vulkanName.size();
                    const bool rightOk = (after >= patched.size()) ||
                                         !(std::isalnum(static_cast<unsigned char>(patched[after])) ||
                                           patched[after] == '_');
                    if (!leftOk || !rightOk)
                    {
                        pos = after;
                        continue;
                    }
                    patched.replace(pos, vulkanName.size(), openglName);
                }
            }
            if (const sizet versionPos = patched.find("#version"); versionPos != std::string::npos)
            {
                const sizet eol = patched.find('\n', versionPos);
                const sizet insertAt = (eol == std::string::npos) ? patched.size() : eol + 1u;
                patched.insert(insertAt, kPrologue);
            }
            else
            {
                patched.insert(0, std::string("#version 460 core\n").append(kPrologue));
            }

            const GLuint shader = glCreateShader(stage);
            const GLchar* const cstr = patched.c_str();
            glShaderSource(shader, 1, &cstr, nullptr);
            glCompileShader(shader);

            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE)
            {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<sizet>(length > 0 ? length : 1));
                glGetShaderInfoLog(shader, length, nullptr, log.data());
                OLO_CORE_ERROR("[Bindless] '{}' stage {} failed to compile: {}",
                               m_FilePath, Utils::GLShaderStageToString(stage), log.data());

                // Dump the exact text the driver saw. There are no #line
                // directives on this route (PreProcess splices includes in
                // verbatim), so a reported line number refers to the FLATTENED
                // source and is unusable without it.
                const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
                const std::filesystem::path dumpPath =
                    cacheDirectory / (std::filesystem::path(m_FilePath).filename().string() +
                                      Utils::GLShaderStageCachedOpenGLFileExtension(stage) + ".bindless.failed.glsl");
                if (std::ofstream dump(dumpPath); dump.is_open())
                {
                    dump << patched;
                    OLO_CORE_ERROR("  Flattened bindless GLSL dumped to: {}", dumpPath.string());
                }

                glDeleteShader(shader);
                ok = false;
                break;
            }

            glAttachShader(freshProgram, shader);
            attached.push_back(shader);
            m_OpenGLSourceCode[stage] = std::move(patched);
        }

        if (ok)
        {
            glProgramParameteri(freshProgram, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
            glLinkProgram(freshProgram);

            GLint linked = GL_FALSE;
            glGetProgramiv(freshProgram, GL_LINK_STATUS, &linked);
            if (linked == GL_FALSE)
            {
                GLint length = 0;
                glGetProgramiv(freshProgram, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<sizet>(length > 0 ? length : 1));
                glGetProgramInfoLog(freshProgram, length, nullptr, log.data());
                OLO_CORE_ERROR("[Bindless] '{}' failed to link: {}", m_FilePath, log.data());
                ok = false;
            }
        }

        for (const GLuint shader : attached)
        {
            glDetachShader(freshProgram, shader);
            glDeleteShader(shader);
        }

        if (!ok)
        {
            Shader::UnregisterProgram(freshProgram);
            glDeleteProgram(freshProgram);
            // Degrade, do not fail. A broken bindless branch must cost the frame
            // its optimisation, never its shader — the caller retries on the
            // ordinary path, which is the one every device without the extension
            // uses anyway.
            m_IsBindlessVariant = false;
            m_OpenGLSourceCode.clear();
            return false;
        }

        FinalizeProgram(freshProgram, {});
        SaveProgramBinaryCache();
        m_CompilationStatus = ShaderCompilationStatus::Ready;

        // NO SPIR-V MEANS NO Reflect(), so two pieces of state the ordinary route
        // sets would be absent here. This recovers the one that matters.
        //
        // `m_IsDeferredCapable` decides whether Renderer3DMeshSubmission routes a
        // mesh into the deferred producer bucket or the forward-overlay fallback,
        // so leaving it false silently misroutes a G-Buffer shader — a whole-frame
        // behaviour change with no error anywhere. That is why this route used to
        // REFUSE such shaders outright.
        //
        // It does not have to. Reflect() sets the flag from SPIRV-Cross's
        // `stage_outputs`, and the same fact is legible in the source we already
        // hold: a fragment `out` declaration whose name starts with `o_GBuffer` or
        // is one of the three sentinels. Deriving it here costs one scan and
        // unblocks every deferred shader for the heap.
        //
        // The OTHER absent piece — `m_ResourceRegistry`'s discovered resources —
        // was checked rather than assumed: `SetResource`, the only thing that
        // consumes them, has ZERO callers in the engine or the editor. Nothing
        // reads that state, so its absence cannot misroute anything.
        //
        // Criteria mirror Reflect()'s exactly; if that list grows, grow this one
        // (ShaderDeferredCapabilityTest pins the two against each other).
        static constexpr std::string_view kGBufferPrefix = "o_GBuffer";
        static constexpr std::array<std::string_view, 3> kGBufferSentinels{ "gAlbedo", "gNormalRoughAO",
                                                                            "gEmissive" };
        for (const auto& [stage, stageSource] : m_OpenGLSourceCode)
        {
            if (stage != GL_FRAGMENT_SHADER)
            {
                continue;
            }
            // NOTE m_ReadsMaterialHeapOffsets is NOT derived here. It is set at the
            // top of this function, above the cache-hit early return, because
            // anything computed at this point is absent on a warm run.
            if (Utils::DeclaresGBufferOutput(stageSource, kGBufferPrefix, kGBufferSentinels))
            {
                m_IsDeferredCapable = true;
                OLO_CORE_TRACE("    [Bindless] detected G-Buffer fragment outputs by source scan -> "
                               "IsDeferredCapable=true");
                break;
            }
        }

        Shader::RegisterProgramMaterialOffsets(m_RendererID, m_ReadsMaterialHeapOffsets);

        OLO_CORE_INFO("[Bindless] '{}' built through the raw-GLSL route (no SPIR-V).", m_Name);
        return true;
    }

    std::unordered_map<GLenum, std::string> OpenGLShader::PreProcess(std::string_view source)
    {
        OLO_PROFILE_FUNCTION();

        // Split by #type FIRST, then process includes per stage independently.
        // This avoids false "circular include" warnings when multiple stages
        // include the same file (e.g., CameraMatrices.glsl in both vertex and fragment).
        std::unordered_map<GLenum, std::string> shaderSources;
        const std::string sourceStr(source);

        const char* const typeToken = "#type";
        const sizet typeTokenLength = std::strlen(typeToken);
        sizet pos = sourceStr.find(typeToken, 0);
        while (pos != std::string::npos)
        {
            const sizet eol = sourceStr.find_first_of("\r\n", pos);
            OLO_CORE_ASSERT(eol != std::string::npos, "Syntax error");

            sizet typeStart = pos + typeTokenLength;
            while (typeStart < eol && (sourceStr[typeStart] == ' ' || sourceStr[typeStart] == '\t'))
            {
                ++typeStart;
            }

            sizet typeEnd = typeStart;
            while (typeEnd < eol && sourceStr[typeEnd] != ' ' && sourceStr[typeEnd] != '\t' && sourceStr[typeEnd] != '\r' && sourceStr[typeEnd] != '\n')
            {
                ++typeEnd;
            }

            std::string typeStr = sourceStr.substr(typeStart, typeEnd - typeStart);
            GLenum shaderType = Utils::ShaderTypeFromString(typeStr);
            OLO_CORE_ASSERT(shaderType != 0, "Invalid shader type specified");

            const sizet nextLinePos = sourceStr.find_first_not_of("\r\n", eol);
            OLO_CORE_ASSERT(nextLinePos != std::string::npos, "Syntax error");
            pos = sourceStr.find(typeToken, nextLinePos);

            shaderSources[shaderType] = (pos == std::string::npos) ? sourceStr.substr(nextLinePos) : sourceStr.substr(nextLinePos, pos - nextLinePos);
        }

        // Process includes per stage independently — each stage gets a fresh includedFiles set.
        // Also collect all included file paths for cache invalidation.
        m_IncludedFilePaths.clear();
        for (auto& [type, stageSource] : shaderSources)
        {
            std::vector<std::string> stageIncludes;
            stageSource = ProcessIncludes(stageSource, "", stageIncludes);
            m_IncludedFilePaths.insert(m_IncludedFilePaths.end(), stageIncludes.begin(), stageIncludes.end());
        }
        // Deduplicate (order doesn't matter for cache invalidation)
        std::ranges::sort(m_IncludedFilePaths);
        m_IncludedFilePaths.erase(std::unique(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end()), m_IncludedFilePaths.end());

        return shaderSources;
    }

    bool OpenGLShader::CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources)
    {
        // Store original preprocessed source code for debugging
        m_OriginalSourceCode = shaderSources;

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        bool disableCache = Utils::IsShaderCacheDisabled();

        auto& shaderData = m_VulkanSPIRV;
        shaderData.clear();

        // Convert map to vector for parallel processing
        std::vector<std::pair<GLenum, std::string>> stageSourcePairs;
        stageSourcePairs.reserve(shaderSources.size());
        for (const auto& [stage, source] : shaderSources)
        {
            stageSourcePairs.emplace_back(stage, source);
        }

        // Thread-safe storage for compilation results
        struct CompilationResult
        {
            GLenum Stage = 0;
            std::vector<u32> SpirvData;
            bool Success = false;
            std::string ErrorMessage;
            bool NeedsCache = false;
            std::filesystem::path CachePath;
        };

        std::vector<CompilationResult> results(stageSourcePairs.size());
        std::atomic<bool> hasError{ false };

        // Parallel compile all shader stages using Task System
        // shaderc::Compiler is thread-safe and can be used from multiple threads
        ParallelFor(
            "ShaderCompileVulkan",
            static_cast<i32>(stageSourcePairs.size()),
            [this, &hasError, &stageSourcePairs, &results, &cacheDirectory, &disableCache](i32 index)
            {
                // Record the stage before the early-out below so the collect loop can
                // still identify (and log) this result even if this task never runs —
                // the default-constructed CompilationResult::Stage is 0, which isn't a
                // valid GLenum and would otherwise hit GLShaderStageToString's own
                // switch-default assert when logging the failure.
                const auto& [stage, source] = stageSourcePairs[index];
                CompilationResult& result = results[index];
                result.Stage = stage;

                if (hasError.load(std::memory_order_relaxed))
                    return; // Early exit if another stage failed

                const std::filesystem::path shaderFilePath = m_FilePath;
                const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedVulkanFileExtension(stage));
                result.CachePath = cachedPath;

                // Try to load from cache first
                if (std::ifstream in(cachedPath, std::ios::in | std::ios::binary); in.is_open() && !disableCache)
                {
                    in.seekg(0, std::ios::end);
                    const auto size = in.tellg();
                    in.seekg(0, std::ios::beg);

                    if (!IsCacheStale(cachedPath))
                    {
                        result.SpirvData.resize(size / sizeof(u32));
                        in.read(reinterpret_cast<char*>(result.SpirvData.data()), size);
                        result.Success = true;
                        result.NeedsCache = false;
                        return;
                    }
                }

                // Log before compilation so crashes leave a breadcrumb
                OLO_CORE_TRACE("[Vulkan SPIR-V] Compiling '{}' stage {}",
                               m_FilePath, Utils::GLShaderStageToString(stage));

                // Compile the shader - each thread creates its own compiler and options
                // (shaderc is thread-safe but options are not shared)
                shaderc::Compiler compiler;
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                options.SetPreserveBindings(true);
                options.SetAutoBindUniforms(false);
                options.SetGenerateDebugInfo();
                options.SetOptimizationLevel(shaderc_optimization_level_performance);
                // Suppress warnings: shaderc's message parser asserts on malformed
                // glslang warning strings (message.cc line 240).  Warnings are
                // informational and must not crash the engine.
                options.SetSuppressWarnings();

                shaderc::SpvCompilationResult spirvModule = compiler.CompileGlslToSpv(
                    source, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str(), options);

                if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    result.Success = false;
                    result.ErrorMessage = spirvModule.GetErrorMessage();
                    OLO_CORE_ERROR("[Vulkan SPIR-V] Compilation FAILED for '{}' stage {}: {}",
                                   m_FilePath, Utils::GLShaderStageToString(stage), result.ErrorMessage);
                    hasError.store(true, std::memory_order_relaxed);
                    return;
                }

                result.SpirvData = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());
                result.Success = true;
                result.NeedsCache = !disableCache;
            });

        // Collect results and write cache (sequential to avoid map race conditions).
        // This loop itself is single-threaded (ParallelFor above already joined all
        // workers), so accumulating into a plain local bool is race-free — the
        // subtlety is only that a later *successful* stage must not make the overall
        // result look like a success after an earlier stage failed.
        bool anyStageFailed = false;
        for (const auto& result : results)
        {
            if (!result.Success)
            {
                // A stage that never ran because an earlier stage already tripped
                // |hasError| carries an empty ErrorMessage — still a real failure.
                OLO_CORE_CRITICAL("[OpenGL] SPIR-V compilation failed for '{}' (stage {}): {}",
                                  m_FilePath, Utils::GLShaderStageToString(result.Stage), result.ErrorMessage);
                anyStageFailed = true;
                continue;
            }

            shaderData[result.Stage] = result.SpirvData;

            // Write to cache if needed
            if (result.NeedsCache)
            {
                std::ofstream out(result.CachePath, std::ios::out | std::ios::binary);
                if (out.is_open())
                {
                    out.write(reinterpret_cast<const char*>(result.SpirvData.data()),
                              result.SpirvData.size() * sizeof(u32));
                    out.flush();
                    out.close();
                }
            }
        }

        if (anyStageFailed)
            return false;

        for (auto&& [stage, data] : shaderData)
        {
            Reflect(stage, data);
        }

        return true;
    }

    bool OpenGLShader::CompileOrGetOpenGLBinaries()
    {
        auto& shaderData = m_OpenGLSPIRV;

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        bool disableCache = Utils::IsShaderCacheDisabled();

        shaderData.clear();
        m_OpenGLSourceCode.clear();

        // Convert map to vector for parallel processing
        std::vector<std::pair<GLenum, std::vector<u32>>> stageSpirvPairs;
        stageSpirvPairs.reserve(m_VulkanSPIRV.size());
        for (const auto& [stage, spirv] : m_VulkanSPIRV)
        {
            stageSpirvPairs.emplace_back(stage, spirv);
        }

        // Thread-safe storage for compilation results
        struct OpenGLCompilationResult
        {
            GLenum Stage = 0;
            std::vector<u32> SpirvData;
            std::string GlslSource;
            bool Success = false;
            std::string ErrorMessage;
            bool NeedsCache = false;
            std::filesystem::path CachePath;
        };

        std::vector<OpenGLCompilationResult> results(stageSpirvPairs.size());
        std::atomic<bool> hasError{ false };

        // Parallel compile/cross-compile all shader stages using Task System
        ParallelFor(
            "ShaderCompileOpenGL",
            static_cast<i32>(stageSpirvPairs.size()),
            [this, &hasError, &stageSpirvPairs, &results, &cacheDirectory, &disableCache](i32 index)
            {
                // Record the stage before the early-out below — see the matching
                // comment in CompileOrGetVulkanBinaries's ParallelFor lambda.
                const auto& [stage, vulkanSpirv] = stageSpirvPairs[index];
                OpenGLCompilationResult& result = results[index];
                result.Stage = stage;

                if (hasError.load(std::memory_order_relaxed))
                    return;

                const std::filesystem::path shaderFilePath = m_FilePath;
                const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedOpenGLFileExtension(stage));
                result.CachePath = cachedPath;

                // Try to load from cache first
                if (std::ifstream in(cachedPath, std::ios::in | std::ios::binary); in.is_open() && !disableCache)
                {
                    if (!IsCacheStale(cachedPath))
                    {
                        in.seekg(0, std::ios::end);
                        const auto size = in.tellg();
                        in.seekg(0, std::ios::beg);

                        result.SpirvData.resize(size / sizeof(u32));
                        in.read(reinterpret_cast<char*>(result.SpirvData.data()), size);
                        result.Success = true;
                        result.NeedsCache = false;
                        return;
                    }
                }

                // Cross-compile Vulkan SPIR-V to GLSL using spirv-cross
                spirv_cross::CompilerGLSL glslCompiler(vulkanSpirv);

                spirv_cross::CompilerGLSL::Options glslOptions;
                glslOptions.version = 450;
                glslOptions.es = false;
                glslOptions.vulkan_semantics = false;
                glslOptions.separate_shader_objects = false;
                glslOptions.enable_420pack_extension = true;
                glslOptions.emit_uniform_buffer_as_plain_uniforms = false;
                glslCompiler.set_common_options(glslOptions);

                glslCompiler.require_extension("GL_ARB_separate_shader_objects");

                // Preserve resource names
                auto resources = glslCompiler.get_shader_resources();
                auto preserveResourceNames = [&glslCompiler](const auto& resourceList)
                {
                    for (const auto& resource : resourceList)
                    {
                        std::string originalName = resource.name;
                        if (!originalName.empty() && originalName.find("_") != 0)
                        {
                            glslCompiler.set_name(resource.id, originalName);
                        }
                    }
                };

                preserveResourceNames(resources.uniform_buffers);
                preserveResourceNames(resources.stage_inputs);
                preserveResourceNames(resources.stage_outputs);
                preserveResourceNames(resources.storage_buffers);

                result.GlslSource = glslCompiler.compile();

                // Log before compilation so crashes leave a breadcrumb
                OLO_CORE_TRACE("[OpenGL SPIR-V] Compiling '{}' stage {} ({} lines of cross-compiled GLSL)",
                               m_FilePath, Utils::GLShaderStageToString(stage),
                               std::ranges::count(result.GlslSource, '\n'));

                // Compile GLSL to OpenGL SPIR-V
                shaderc::Compiler compiler;
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
                options.SetPreserveBindings(true);
                // Suppress warnings: cross-compiled GLSL from spirv-cross can
                // trigger glslang warnings whose format crashes shaderc's
                // message parser (assertion in message.cc).
                options.SetSuppressWarnings();

                shaderc::SpvCompilationResult spirvModule = compiler.CompileGlslToSpv(
                    result.GlslSource, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str(), options);

                if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    result.Success = false;
                    result.ErrorMessage = spirvModule.GetErrorMessage();
                    OLO_CORE_ERROR("[OpenGL SPIR-V] Cross-compilation FAILED for '{}' stage {}: {}",
                                   m_FilePath, Utils::GLShaderStageToString(stage), result.ErrorMessage);

                    // Dump the generated GLSL to a temp file for post-mortem debugging
                    auto const dumpPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedOpenGLFileExtension(stage) + ".failed.glsl");
                    if (std::ofstream dump(dumpPath); dump.is_open())
                    {
                        dump << result.GlslSource;
                        OLO_CORE_ERROR("  Cross-compiled GLSL source dumped to: {}", dumpPath.string());
                    }

                    hasError.store(true, std::memory_order_relaxed);
                    return;
                }

                result.SpirvData = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());
                result.Success = true;
                result.NeedsCache = !disableCache;
            });

        // Collect results and write cache (sequential). Single-threaded here too
        // (ParallelFor above already joined), so this bool accumulation is race-free.
        bool anyStageFailed = false;
        for (const auto& result : results)
        {
            if (!result.Success)
            {
                OLO_CORE_CRITICAL("[OpenGL] SPIR-V cross-compilation failed for '{}' (stage {}): {}",
                                  m_FilePath, Utils::GLShaderStageToString(result.Stage), result.ErrorMessage);
                anyStageFailed = true;
                continue;
            }

            shaderData[result.Stage] = result.SpirvData;
            if (!result.GlslSource.empty())
            {
                m_OpenGLSourceCode[result.Stage] = result.GlslSource;
            }

            // Write to cache if needed
            if (result.NeedsCache)
            {
                std::ofstream out(result.CachePath, std::ios::out | std::ios::binary);
                if (out.is_open())
                {
                    out.write(reinterpret_cast<const char*>(result.SpirvData.data()),
                              result.SpirvData.size() * sizeof(u32));
                    out.flush();
                    out.close();
                }
            }
        }

        return !anyStageFailed;
    }

    void OpenGLShader::FinalizeProgram(GLenum const& program, const std::unordered_map<GLenum, std::vector<u32>>& spirvMap)
    {
        OLO_CORE_TRACE("FinalizeProgram: '{}' program={}, spirvMap stages={}", m_Name, program, spirvMap.size());
        m_RendererID = program;
        m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);

        // Record the variant against the NATIVE id so a bind that never reaches
        // Bind() — CommandDispatch binds by handle through
        // RendererAPI::BindShaderProgram — can still publish it. Registered
        // unconditionally (true AND false) because GL reissues freed program
        // names: a slot-based program inheriting a retired bindless id would
        // otherwise be told it reads the heap (issue #691 Phase 3).
        Shader::RegisterProgramBindless(m_RendererID, m_IsBindlessVariant);

        // Name the program for GPU debuggers (RenderDoc/NSight) and register it
        // in the CPU-side label registry so the GL debug callback can resolve
        // driver perf messages that reference a raw program id (e.g. NVIDIA
        // id 131218 shader-recompile warnings) to this shader's name.
        if (!m_Name.empty())
        {
            glObjectLabel(GL_PROGRAM, program, -1, m_Name.c_str());
            RegisterGLProgramLabel(program, m_Name);
        }

        // Compute estimated memory from the appropriate SPIR-V map
        sizet estimatedMemory = 0;
        for (const auto& [stage, spirv] : spirvMap)
        {
            estimatedMemory += spirv.size() * sizeof(u32);
        }
        estimatedMemory += 1024; // Additional overhead for program linking, uniforms, etc.

        // On a hot-reload FinalizeProgram runs again for the same 'this'. The tracker is
        // keyed by pointer, so re-tracking without first releasing the prior allocation
        // leaks the old size into m_TypeUsage on every reload — balance it here so
        // FinalizeProgram is self-consistent across re-invocations.
        if (m_TrackedAllocation)
        {
            OLO_TRACK_DEALLOC(this);
        }

        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            estimatedMemory,
                            RendererMemoryTracker::ResourceType::Shader,
                            m_Name.empty() ? "OpenGL Shader" : m_Name);
        m_TrackedAllocation = true;

        OLO_CORE_TRACE("FinalizeProgram: '{}' decompiling SPIR-V for debugger...", m_Name);
        // Store shader source code in debugger
        for (const auto& [stage, spirv] : spirvMap)
        {
            OLO_CORE_TRACE("FinalizeProgram: '{}' decompiling stage {} ({} words)...",
                           m_Name, Utils::GLShaderStageToString(stage), spirv.size());
            spirv_cross::CompilerGLSL glslCompiler(spirv);
            const std::string generatedGLSL = glslCompiler.compile();

            std::string originalSource;
            if (auto originalIt = m_OriginalSourceCode.find(stage); originalIt != m_OriginalSourceCode.end())
            {
                originalSource = originalIt->second;
            }

            std::vector<u8> spirvBytes;
            spirvBytes.reserve(spirv.size() * sizeof(u32));
            const u8* spirvData = reinterpret_cast<const u8*>(spirv.data());
            spirvBytes.assign(spirvData, spirvData + spirv.size() * sizeof(u32));

            OLO_SHADER_SET_SOURCE(m_RendererID, GLStageToShaderStage(stage),
                                  originalSource, generatedGLSL, spirvBytes);
        }
        OLO_CORE_TRACE("FinalizeProgram: '{}' complete", m_Name);
    }

    void OpenGLShader::CreateProgram()
    {
        GLuint program = glCreateProgram();

        // Try to load from the program-binary cache first. The shared helper parses
        // the on-disk framing, calls glProgramBinary, and soft-checks GL_LINK_STATUS;
        // on any miss/failure it returns false and we fall through to a full compile.
        if (LoadProgramBinaryCache(program))
        {
            FinalizeProgram(program, m_OpenGLSPIRV);
            m_CompilationStatus = ShaderCompilationStatus::Ready;
            OLO_CORE_TRACE("Loaded shader program from binary cache: {0}", m_FilePath);
            return;
        }

        // Cache miss or invalid binary: discard the (possibly dirtied) program object
        // and recreate a clean one for compilation from SPIR-V.
        Shader::UnregisterProgram(program);
        glDeleteProgram(program);
        program = glCreateProgram();

        // Compile from SPIR-V if cache miss or invalid
        std::vector<GLuint> shaderIDs;
        for (auto&& [stage, spirv] : m_OpenGLSPIRV)
        {
            const GLuint shaderID = shaderIDs.emplace_back(glCreateShader(stage));
            glShaderBinary(1, &shaderID, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.data(), static_cast<GLsizei>(spirv.size() * sizeof(u32)));
            glSpecializeShader(shaderID, "main", 0, nullptr, nullptr);
            glAttachShader(program, shaderID);
        }

        // Tell the driver we may retrieve the program binary later (required by spec
        // for glGetProgramBinary; without this, Mesa/radeonsi can crash in SaveProgramBinaryCache).
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(program);

        // ---- Async path: if GL_ARB/KHR_parallel_shader_compile is available,
        //      do NOT check GL_LINK_STATUS now. The driver links in background.
        //      We'll poll GL_COMPLETION_STATUS_ARB later via PollCompilationStatus().
        if (OpenGLContext::HasParallelShaderCompile())
        {
            // Store the program handle and keep shader objects alive until finalization
            m_RendererID = program;
            m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
            m_PendingShaderIDs = std::move(shaderIDs);
            m_CompilationStatus = ShaderCompilationStatus::Compiling;
            return;
        }

        // ---- Synchronous fallback: no parallel compile extension ----------
        GLint isLinked{};
        glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
        if (GL_FALSE == isLinked)
        {
            GLint maxLength{};
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

            std::vector<GLchar> infoLog(maxLength);
            glGetProgramInfoLog(program, maxLength, &maxLength, infoLog.data());
            OLO_CORE_CRITICAL("[OpenGL] Shader linking failed for '{}':\n{}", m_FilePath, infoLog.data());

            Shader::UnregisterProgram(program);
            glDeleteProgram(program);

            for (const auto id : shaderIDs)
            {
                glDeleteShader(id);
            }
            m_CompilationStatus = ShaderCompilationStatus::Failed;
            return;
        }

        // Clean up shader objects
        for (const auto id : shaderIDs)
        {
            glDetachShader(program, id);
            glDeleteShader(id);
        }

        FinalizeProgram(program, m_OpenGLSPIRV);

        // Save program binary to cache (after FinalizeProgram so m_RendererID is set)
        SaveProgramBinaryCache();

        m_CompilationStatus = ShaderCompilationStatus::Ready;
    }

    // ========================================================================
    // Async link helpers
    // ========================================================================

    // A driver update invalidates every previously saved GL program binary at
    // once — see SyncProgramBinaryCacheDriverStamp (OpenGLProgramBinaryCache.h)
    // for the full rationale; the wipe/stamp logic lives there, GL-free, so it
    // is unit-tested (ShaderBinaryCacheRoundTripTest). This wrapper only builds
    // the driver identity string, which needs a current GL context (both call
    // sites are shader compile/link paths, which guarantee that).
    static void EnsureProgramBinaryCacheMatchesDriver()
    {
        static std::once_flag s_Once;
        std::call_once(s_Once, []()
                       {
            const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            const std::string stamp = std::string(vendor ? vendor : "?") + "|" +
                                      (renderer ? renderer : "?") + "|" +
                                      (version ? version : "?");

            const DriverStampSyncResult sync =
                SyncProgramBinaryCacheDriverStamp(Utils::GetCacheDirectory(), stamp);

            // First run ever (no stamp, no binaries) is not worth a log line.
            if (sync.Mismatched && (!sync.PreviousStamp.empty() || sync.RemovedBinaries > 0))
            {
                OLO_CORE_INFO("Program-binary cache invalidated by driver change ({0} stale binaries dropped; '{1}' -> '{2}'). "
                              "Shaders recompile once this launch and re-cache.",
                              sync.RemovedBinaries,
                              sync.PreviousStamp.empty() ? "<no stamp>" : sync.PreviousStamp, stamp);
            } });
    }

    bool OpenGLShader::LoadProgramBinaryCache(GLenum program) const
    {
        OLO_PROFILE_FUNCTION();

        if (Utils::IsShaderCacheDisabled())
            return false;

        EnsureProgramBinaryCacheMatchesDriver();

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        const std::filesystem::path shaderFilePath = m_FilePath;
        const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ProgramBinaryCacheSuffix());

        std::ifstream in(cachedPath, std::ios::binary);
        if (!in.is_open())
            return false;

        if (IsCacheStale(cachedPath))
        {
            OLO_CORE_TRACE("Shader source or include newer than cache, recompiling: {0}", shaderFilePath.string());
            return false;
        }

        const std::optional<ProgramBinary> binary = ReadProgramBinary(in);
        in.close();
        if (!binary)
        {
            OLO_CORE_WARN("Shader program binary cache corrupt or truncated, recompiling: {0}", cachedPath.string());
            return false;
        }

        glProgramBinary(program, binary->Format, binary->Data.data(), static_cast<GLsizei>(binary->Data.size()));

        // Soft link-status check: loading a cached binary is a best-effort optimisation, so
        // a failure here must fall back to recompilation rather than abort. (The old AMD path
        // used the fatal VerifyProgramLink() here, which OLO_DEBUGBREAK()s on every launch.)
        // A failure here is legal per the GL spec — the driver may reject a binary whose
        // source has changed or that was produced by a different driver version — so we log
        // the program info log to disambiguate that expected case from a genuine framing bug.
        GLint isLinked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
        if (isLinked != GL_TRUE)
        {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            std::string infoLog;
            if (logLength > 0)
            {
                infoLog.resize(static_cast<sizet>(logLength));
                glGetProgramInfoLog(program, logLength, nullptr, infoLog.data());
            }
            // A rejected binary is a LEGAL cache miss, not an engine bug: the
            // GL spec allows the driver to reject a stored binary at any time,
            // and NVIDIA does so for inputs beyond the driver build we stamp
            // against (observed: some — not all — first launches after an
            // engine rebuild reject every startup binary with "not compatible
            // with current driver/hardware combination", then the very next
            // launch loads the rewritten set cleanly). The recompile-and-resave
            // below self-heals it, so one launch-level WARN carries all the
            // signal; per-shader WARNs turned an expected miss into a ~105-line
            // flood. Individual rejections stay visible at trace.
            static std::atomic<bool> s_RejectionWarned{ false };
            if (!s_RejectionWarned.exchange(true))
            {
                OLO_CORE_WARN("Cached program binary rejected by the driver, recompiling from SPIR-V: {0}{1}. "
                              "This is an expected cache miss (drivers may reject stored binaries after driver, "
                              "hardware, or application changes; the cache rewrites itself this launch). "
                              "Further rejections this session log at trace.",
                              shaderFilePath.string(), infoLog.empty() ? std::string{} : " (" + infoLog + ")");
            }
            else
            {
                OLO_CORE_TRACE("Cached program binary rejected, recompiling: {0}", shaderFilePath.string());
            }
            return false;
        }

        return true;
    }

    void OpenGLShader::SaveProgramBinaryCache() const
    {
        OLO_PROFILE_FUNCTION();

        if (Utils::IsShaderCacheDisabled() || m_RendererID == 0)
            return;

        EnsureProgramBinaryCacheMatchesDriver();

        // Mesa radeonsi crashes in glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) for
        // programs compiled from SPIR-V binary (glShaderBinary + glSpecializeShader).
        // The AMD-specific CreateProgramForAmd() path has its own cache save that
        // works correctly (it compiles from cross-compiled GLSL text instead).
        if (Utils::IsAmdGpu() && !m_OpenGLSPIRV.empty())
            return;

        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        if (formats <= 0)
            return;

        Utils::CreateCacheDirectoryIfNeeded();
        GLint length = 0;
        glGetProgramiv(m_RendererID, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length <= 0)
            return;

        auto shaderData = std::vector<char>(length);
        u32 format = 0;
        glGetProgramBinary(m_RendererID, length, nullptr, &format, shaderData.data());

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        const std::filesystem::path shaderFilePath = m_FilePath;
        const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ProgramBinaryCacheSuffix());

        std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
        if (out.is_open())
        {
            WriteProgramBinary(out, format, shaderData.data(), shaderData.size());
            out.flush();
            out.close();
            OLO_CORE_TRACE("Saved shader program binary to cache: {0}", cachedPath.string());
        }
    }

    void OpenGLShader::FinalizeAfterLink()
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_TRACE("FinalizeAfterLink: Checking link status for '{}' (ID {})", m_Name, m_RendererID);
        // Check link status (driver should be done by now)
        GLint isLinked = 0;
        glGetProgramiv(m_RendererID, GL_LINK_STATUS, &isLinked);
        OLO_CORE_TRACE("FinalizeAfterLink: '{}' link status = {}", m_Name, isLinked);

        // Clean up individual shader objects regardless of success
        for (const auto id : m_PendingShaderIDs)
        {
            glDetachShader(m_RendererID, id);
            glDeleteShader(id);
        }
        m_PendingShaderIDs.clear();

        if (GL_FALSE == isLinked)
        {
            GLint maxLength = 0;
            glGetProgramiv(m_RendererID, GL_INFO_LOG_LENGTH, &maxLength);

            std::vector<GLchar> infoLog(maxLength);
            glGetProgramInfoLog(m_RendererID, maxLength, &maxLength, infoLog.data());
            OLO_CORE_CRITICAL("[OpenGL] Async shader linking failed for '{}':\n{}", m_FilePath, infoLog.data());

            Shader::UnregisterProgram(m_RendererID);
            glDeleteProgram(m_RendererID);
            m_RendererID = 0;
            m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
            m_CompilationStatus = ShaderCompilationStatus::Failed;
            OLO_SHADER_COMPILATION_END(0, false, infoLog.data(), m_DeferredCompilationTime);
            m_DeferredCompilationTime = 0.0;
            return;
        }

        OLO_CORE_TRACE("FinalizeAfterLink: Calling FinalizeProgram for '{}'...", m_Name);
        FinalizeProgram(m_RendererID, m_OpenGLSPIRV);
        OLO_CORE_TRACE("FinalizeAfterLink: Saving cache for '{}'...", m_Name);
        SaveProgramBinaryCache();
        m_CompilationStatus = ShaderCompilationStatus::Ready;

        // Now that the shader is registered via FinalizeProgram, report deferred compilation end
        OLO_SHADER_COMPILATION_END(m_RendererID, true, "", m_DeferredCompilationTime);
        m_DeferredCompilationTime = 0.0;

        OLO_CORE_TRACE("FinalizeAfterLink: Shader '{}' is Ready", m_Name);
    }

    bool OpenGLShader::PollCompilationStatus()
    {
        OLO_PROFILE_FUNCTION();

        if (m_CompilationStatus != ShaderCompilationStatus::Compiling)
            return m_CompilationStatus == ShaderCompilationStatus::Ready || m_CompilationStatus == ShaderCompilationStatus::Failed;

        // Poll the driver — GL_COMPLETION_STATUS_ARB is non-blocking
        GLint complete = GL_FALSE;
        glGetProgramiv(m_RendererID, GL_COMPLETION_STATUS_ARB, &complete);

        if (complete == GL_TRUE)
        {
            FinalizeAfterLink();
            return true;
        }

        return false; // Still compiling
    }

    void OpenGLShader::EnsureLinked()
    {
        OLO_PROFILE_FUNCTION();

        if (m_CompilationStatus == ShaderCompilationStatus::Failed)
        {
            glUseProgram(0);
            return;
        }

        if (m_CompilationStatus != ShaderCompilationStatus::Compiling)
            return;

        OLO_CORE_INFO("EnsureLinked: Force-completing shader '{}' (ID {})", m_Name, m_RendererID);
        // Force-complete: check link status (this blocks until the driver finishes)
        FinalizeAfterLink();
        OLO_CORE_INFO("EnsureLinked: Completed shader '{}', status={}", m_Name, static_cast<int>(m_CompilationStatus));
    }

    static bool VerifyProgramLink(GLenum const& program, const std::string& filePath)
    {
        int isLinked = 0;
        glGetError();
        glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
        if (GL_FALSE == isLinked)
        {
            GLint maxLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

            std::vector<GLchar> infoLog(maxLength);
            glGetProgramInfoLog(program, maxLength, &maxLength, &infoLog[0]);

            OLO_CORE_CRITICAL("[OpenGL] Shader link failure for '{}': {}", filePath, infoLog.data());
            return false;
        }
        return true;
    }

    void OpenGLShader::CreateProgramForAmd()
    {
        GLuint program = glCreateProgram();

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        const std::filesystem::path shaderFilePath = m_FilePath;
        const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ProgramBinaryCacheSuffix());
        bool disableCache = Utils::IsShaderCacheDisabled();

        // Shared with the non-AMD path: parse the framing, call glProgramBinary, and
        // soft-check GL_LINK_STATUS. This previously had its own hand-copied loader that
        // read the wrong byte count (issue #267) and used the fatal VerifyProgramLink for
        // a recoverable cache miss; routing through the shared helper fixes both.
        if (LoadProgramBinaryCache(program))
        {
            FinalizeProgram(program, m_VulkanSPIRV);
            m_CompilationStatus = ShaderCompilationStatus::Ready;
            OLO_CORE_TRACE("Loaded shader program from binary cache: {0}", m_FilePath);
            return;
        }

        // Cache miss or invalid binary: recreate a clean program object before compiling.
        Shader::UnregisterProgram(program);
        glDeleteProgram(program);
        program = glCreateProgram();

        std::array<u32, 2> glShadersIDs{};
        if (!CompileOpenGLBinariesForAmd(program, glShadersIDs))
        {
            // CompileOpenGLBinariesForAmd already cleaned up any shaders it attached
            // before failing; |program| itself is still ours to delete.
            Shader::UnregisterProgram(program);
            glDeleteProgram(program);
            m_CompilationStatus = ShaderCompilationStatus::Failed;
            return;
        }
        // Required by spec for glGetProgramBinary; prevents Mesa/radeonsi crash.
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(program);

        if (!VerifyProgramLink(program, m_FilePath))
        {
            for (auto const& id : glShadersIDs)
            {
                glDetachShader(program, id);
            }
            Shader::UnregisterProgram(program);
            glDeleteProgram(program);
            m_CompilationStatus = ShaderCompilationStatus::Failed;
            return;
        }

        {
            GLint formats = 0;
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
            OLO_CORE_ASSERT(formats > 0, "Driver does not support binary format");
            Utils::CreateCacheDirectoryIfNeeded();
            GLint length = 0;
            glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
            auto shaderData = std::vector<char>(length);
            u32 format = 0;
            glGetProgramBinary(program, length, nullptr, &format, shaderData.data());

            if (!disableCache)
            {
                std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
                if (out.is_open())
                {
                    WriteProgramBinary(out, format, shaderData.data(), shaderData.size());
                    out.flush();
                    out.close();
                }
            }
        }

        for (auto const& id : glShadersIDs)
        {
            glDetachShader(program, id);
        }

        FinalizeProgram(program, m_VulkanSPIRV);
        m_CompilationStatus = ShaderCompilationStatus::Ready;
    }

    bool OpenGLShader::CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const
    {
        int glShaderIDIndex = 0;
        for (auto&& [stage, spirv] : m_VulkanSPIRV)
        {
            spirv_cross::CompilerGLSL glslCompiler(spirv);

            // Configure compiler options to preserve names and bindings
            spirv_cross::CompilerGLSL::Options options;
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            options.separate_shader_objects = false;
            options.enable_420pack_extension = true;
            glslCompiler.set_common_options(options);

            // Try to preserve variable names by setting them explicitly
            auto resources = glslCompiler.get_shader_resources();
            for (const auto& ubo : resources.uniform_buffers)
            {
                // Try to preserve the original name if it exists
                std::string originalName = ubo.name;
                if (!originalName.empty() && originalName.find("_") != 0)
                {
                    glslCompiler.set_name(ubo.id, originalName);
                }
            }

            const auto source = glslCompiler.compile();

            u32 shader = glCreateShader(stage);

            const GLchar* const sourceCStr = source.c_str();
            glShaderSource(shader, 1, &sourceCStr, nullptr);

            glCompileShader(shader);

            int isCompiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
            if (GL_FALSE == isCompiled)
            {
                int maxLength = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);

                std::vector<char> infoLog(maxLength);
                glGetShaderInfoLog(shader, maxLength, &maxLength, &infoLog[0]);

                glDeleteShader(shader);

                OLO_CORE_CRITICAL("[OpenGL] Shader compilation failed for '{}' (stage {}): {}",
                                  m_FilePath, Utils::GLShaderStageToString(stage), infoLog.data());

                // Clean up any stages already compiled+attached during this call so the
                // caller is left with a clean, empty |program| rather than a partially
                // attached one it doesn't know about.
                for (int i = 0; i < glShaderIDIndex; ++i)
                {
                    glDetachShader(program, glShadersIDs[i]);
                    glDeleteShader(glShadersIDs[i]);
                }
                glShadersIDs.fill(0);
                return false;
            }
            glAttachShader(program, shader);
            glShadersIDs[glShaderIDIndex++] = shader;
        }
        return true;
    }

    void OpenGLShader::Reflect(const GLenum stage, const std::vector<u32>& shaderData)
    {
        const spirv_cross::Compiler compiler(shaderData);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        OLO_CORE_TRACE("OpenGLShader::Reflect - {0} {1}", Utils::GLShaderStageToString(stage), m_FilePath);
        OLO_CORE_TRACE("    {0} uniform buffers", resources.uniform_buffers.size());
        OLO_CORE_TRACE("    {0} resources", resources.sampled_images.size());

        // Integrate with the resource registry for automatic resource discovery
        m_ResourceRegistry.DiscoverResources(stage, shaderData, m_FilePath);

        // Deferred-capability detection: scan fragment stage outputs for the
        // engine's G-Buffer MRT marker names. Any single match promotes the
        // shader to deferred-capable so Renderer3D routes it into ScenePass's
        // G-Buffer producer bucket instead of the forward-overlay fallback.
        // Markers are case-sensitive and match the opt-in prefix/sentinels
        // used across the current shader set (PBR_GBuffer*, Decal_GBuffer*,
        // Foliage_Instance_GBuffer, skybox / grid / light cube / terrain /
        // voxel GBuffer variants).
        if (stage == GL_FRAGMENT_SHADER)
        {
            // Reset before scanning so a Reload()/recompile that drops all
            // G-Buffer outputs correctly downgrades the shader from
            // deferred-capable to forward-only. Without this, a stale `true`
            // from a prior compile would persist for the lifetime of the
            // OpenGLShader instance.
            m_IsDeferredCapable = false;

            // Marker prefixes / full names. Using string_view to avoid
            // per-shader heap allocations; inputs from SPIR-V reflection
            // are plain std::string.
            constexpr std::string_view kGBufferPrefix = "o_GBuffer";
            static constexpr std::string_view kGBufferSentinels[] = {
                "gAlbedo", "gNormalRoughAO", "gEmissive"
            };
            for (const auto& out : resources.stage_outputs)
            {
                const std::string_view name = out.name;
                if (name.rfind(kGBufferPrefix, 0) == 0)
                {
                    m_IsDeferredCapable = true;
                    break;
                }
                bool matched = false;
                for (const auto& sentinel : kGBufferSentinels)
                {
                    if (name == sentinel)
                    {
                        matched = true;
                        break;
                    }
                }
                if (matched)
                {
                    m_IsDeferredCapable = true;
                    break;
                }
            }
            if (m_IsDeferredCapable)
            {
                OLO_CORE_TRACE("    detected G-Buffer fragment outputs -> IsDeferredCapable=true");
            }
        }

        // Keep existing debug logging for compatibility
        OLO_CORE_TRACE("Uniform buffers:");
        for (const auto& resource : resources.uniform_buffers)
        {
            const auto& bufferType = compiler.get_type(resource.base_type_id);
            sizet bufferSize = compiler.get_declared_struct_size(bufferType);
            u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            sizet memberCount = bufferType.member_types.size();

            OLO_CORE_TRACE("  {0}", resource.name);
            OLO_CORE_TRACE("    Size = {0}", bufferSize);
            OLO_CORE_TRACE("    Binding = {0}", binding);
            OLO_CORE_TRACE("    Members = {0}", memberCount);
        }
    }

    void OpenGLShader::Reload()
    {
        // Capture the currently-live program up front. Every recompile path funnels
        // through FinalizeProgram, which reassigns m_RendererID to a fresh
        // glCreateProgram() handle — the old handle would otherwise leak. Keeping the
        // old id in a local also lets us key the debugger reload start/end on a single,
        // consistent id (the pre-reload one), instead of START(old)/END(new) which
        // orphaned the old entry stuck m_IsReloading=true forever.
        const GLuint oldProgram = m_RendererID;
        OLO_SHADER_RELOAD_START(oldProgram);

        m_CompilationStatus = ShaderCompilationStatus::Pending;

        std::string source = ReadFile(m_FilePath);
        auto shaderSources = PreProcess(source);

        try
        {
            // Re-decide the variant rather than reusing the old one. This is the
            // one place the runtime toggle CAN take effect, which makes an A/B
            // capture possible without a restart: flip the heap on, reload the
            // shader, and the same file comes back bindless.
            m_IsBindlessVariant = false;
            if (WantsBindlessVariant(shaderSources) && CreateProgramFromRawGLSL(shaderSources))
            {
                EnsureLinked();
            }
            else if (!CompileOrGetVulkanBinaries(shaderSources))
            {
                // Broken GLSL in the edited file — already logged via
                // OLO_CORE_CRITICAL. Fall through to Failed; the restore-old-program
                // block below keeps the previously-working shader bound (issue #568).
                m_CompilationStatus = ShaderCompilationStatus::Failed;
            }
            else if (Utils::IsAmdGpu())
            {
                CreateProgramForAmd();
            }
            else if (CompileOrGetOpenGLBinaries())
            {
                CreateProgram();
            }
            else
            {
                m_CompilationStatus = ShaderCompilationStatus::Failed;
            }

            // For hot-reload we want synchronous completion so the new shader
            // is immediately usable — force-finish any async link.
            EnsureLinked();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Shader reload failed for '{}': {}", m_Name, e.what());
            m_CompilationStatus = ShaderCompilationStatus::Failed;
        }
        catch (...)
        {
            OLO_CORE_ERROR("Shader reload failed for '{}': unknown error", m_Name);
            m_CompilationStatus = ShaderCompilationStatus::Failed;
        }

        const bool success = (m_CompilationStatus == ShaderCompilationStatus::Ready);

        // Async link failure: the parallel-compile path (CreateProgram) commits the fresh
        // program to m_RendererID before the link resolves, and FinalizeAfterLink then
        // deletes it and zeroes m_RendererID on failure — unlike the sync/AMD paths, which
        // leave m_RendererID on the old program. Restore the previously-working program so
        // a failed reload keeps the shader usable and doesn't orphan the old handle. The
        // retire block below is success-gated, so oldProgram is never deleted on this path.
        if (!success && oldProgram != 0 && m_RendererID != oldProgram)
        {
            m_RendererID = oldProgram;
            m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
            m_CompilationStatus = ShaderCompilationStatus::Ready;
        }

        // Notify the debugger of the reload outcome BEFORE unregistering the old id, so
        // ShaderDebugger::OnReloadEnd can still resolve the entry by its (old) RendererID
        // and record the reload event. `success` reflects the reload result, independent
        // of any restore above.
        OLO_SHADER_RELOAD_END(oldProgram, success);

        // Retire the previous GL program (and its debugger entry) once the new one is
        // live. Guarding on success keeps the old program as a fallback when a reload
        // fails. Guarding on the id actually changing avoids deleting a handle a cache-hit
        // path legitimately reused. Deletion is deferred through the FrameResourceManager
        // (matching the destructor) because the old program may still be referenced by the
        // in-flight frame's commands.
        if (success && oldProgram != 0 && m_RendererID != oldProgram)
        {
            OLO_SHADER_UNREGISTER(oldProgram);

            // Re-point the shader-system resource-registry map from the old program id
            // to the new one. ShaderLibrary::ReloadShaders() only calls Reload() and
            // never re-runs InitializeResourceRegistry, so without this the global map
            // keeps mapping the (about-to-be-deleted) old id — Find(newId) misses and
            // the stale old-id entry leaks until the shader is destroyed (the destructor
            // only unregisters the current id). A same-file recompile keeps the resource
            // layout, so the existing m_ResourceRegistry contents stay valid; we only
            // re-point when the old id was actually registered.
            if (ShaderResourceRegistry::Find(oldProgram) != nullptr)
            {
                ShaderResourceRegistry::Unregister(oldProgram);
                ShaderResourceRegistry::Register(m_RendererID, &m_ResourceRegistry);
            }

            UnregisterGLProgramLabel(oldProgram);
            FrameResourceManager::Get().SubmitForDeletion([oldProgram]()
                                                          {
                                                              // See Utils::UnbindProgramIfCurrent (issue #625): the
                                                              // reloaded-away program may still be bound by the time
                                                              // this deferred deletion runs.
                                                              Utils::UnbindProgramIfCurrent(oldProgram);
                                                              Shader::UnregisterProgram(oldProgram);
                                                              glDeleteProgram(oldProgram); });
        }
    }

    void OpenGLShader::Bind() const
    {
        OLO_PROFILE_FUNCTION();

        // Lazy finalization: if the shader is still being linked asynchronously,
        // force-complete it now (one-time micro-stall on first bind).
        if (m_CompilationStatus == ShaderCompilationStatus::Compiling)
        {
            const_cast<OpenGLShader*>(this)->EnsureLinked();
        }

        if (m_CompilationStatus != ShaderCompilationStatus::Ready)
            return;

        glUseProgram(m_RendererID);

        // Record whether the program now in flight actually reads the descriptor
        // heap. The heap's enabled flag is global; this is per shader, because
        // the bindless route may have declined and fallen back. Without it,
        // RGCommandContext::BindTextureOrHeapOffset would skip the bind for a
        // program that reads sampler binding points (issue #691 Phase 3).
        Shader::SetBoundProgramBindless(m_IsBindlessVariant);
        Shader::SetBoundProgramMaterialOffsets(m_ReadsMaterialHeapOffsets);

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::ShaderBinds, 1);

        // Track shader binding
        OLO_SHADER_BIND(m_RendererID);
    }

    void OpenGLShader::Unbind() const
    {
        OLO_PROFILE_FUNCTION();

        glUseProgram(0);

        // No program is bound, so no program reads the heap. Bind() publishes
        // this flag and Unbind() has to retract it, or a bind issued between an
        // Unbind and the next Bind takes the heap path on the strength of a
        // program that is no longer in flight — recording an offset and skipping
        // the bind for nobody (issue #691 Phase 3).
        Shader::SetBoundProgramBindless(false);
        Shader::SetBoundProgramMaterialOffsets(false);
    }
    void OpenGLShader::SetInt(const std::string& name, const int value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformInt(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Int);
    }

    void OpenGLShader::SetIntArray(const std::string& name, int* const values, const u32 count) const
    {
        UploadUniformIntArray(name, values, count);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::IntArray);
    }

    void OpenGLShader::SetFloat(const std::string& name, const f32 value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformFloat(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float);
    }

    void OpenGLShader::SetFloat2(const std::string& name, const glm::vec2& value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformFloat2(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float2);
    }

    void OpenGLShader::SetFloat3(const std::string& name, const glm::vec3& value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformFloat3(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float3);
    }

    void OpenGLShader::SetFloat4(const std::string& name, const glm::vec4& value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformFloat4(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float4);
    }

    void OpenGLShader::SetMat4(const std::string& name, const glm::mat4& value) const
    {
        OLO_PROFILE_FUNCTION();

        UploadUniformMat4(name, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Mat4);
    }

    void OpenGLShader::UploadUniformInt(const std::string& name, const int value) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform1i(location, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Int);
    }

    void OpenGLShader::UploadUniformIntArray(const std::string& name, int const* const values, const u32 count) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform1iv(location, count, values);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::IntArray);
    }

    void OpenGLShader::UploadUniformFloat(const std::string& name, const f32 value) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform1f(location, value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float);
    }

    void OpenGLShader::UploadUniformFloat2(const std::string& name, const glm::vec2& value) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform2f(location, value.x, value.y);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float2);
    }

    void OpenGLShader::UploadUniformFloat3(const std::string& name, const glm::vec3& value) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform3f(location, value.x, value.y, value.z);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float3);
    }

    void OpenGLShader::UploadUniformFloat4(const std::string& name, const glm::vec4& value) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniform4f(location, value.x, value.y, value.z, value.w);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float4);
    }

    void OpenGLShader::UploadUniformMat3(const std::string& name, const glm::mat3& matrix) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniformMatrix3fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Mat3);
    }

    void OpenGLShader::UploadUniformMat4(const std::string& name, const glm::mat4& matrix) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Mat4);
    }

} // namespace OloEngine
