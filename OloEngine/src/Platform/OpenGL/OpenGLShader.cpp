#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Task/ParallelFor.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>

#include <fstream>
#include <utility>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <chrono>

#include <atomic>

namespace OloEngine
{
    namespace Utils
    {
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
            }
            OLO_CORE_ASSERT(false);
            return nullptr;
        }

        [[nodiscard("Store this!")]] static const char* GetCacheDirectory()
        {
            const std::filesystem::path assetsDirectory = "assets";
            if (!std::filesystem::exists(assetsDirectory))
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
            }
            OLO_CORE_ASSERT(false);
            return "";
        }

        static bool IsAmdGpu()
        {
            const auto* const vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            return std::strstr(vendor, "ATI") != nullptr;
        }
    } // namespace Utils

    OpenGLShader::OpenGLShader(const std::string& filepath)
        : m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

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

        OLO_SHADER_COMPILATION_START(m_Name, filepath);
        const Timer timer;

        CompileOrGetVulkanBinaries(shaderSources);

        if (Utils::IsAmdGpu())
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
                else
                {
                    CompileOrGetOpenGLBinaries();
                    CreateProgram();
                }
            }
            else
            {
                OLO_CORE_ERROR("Could not find driver version in string: '{0}'", fullVersion);
            }
        }
        else
        {
            CompileOrGetOpenGLBinaries();
            CreateProgram();
        }
        const f64 compilationTime = timer.ElapsedMillis();
        OLO_CORE_INFO("Shader creation took {0} ms", compilationTime);

        // Register with shader debugger and report compilation
        OLO_SHADER_COMPILATION_END(m_RendererID, m_RendererID != 0, "", compilationTime);
    }

    OpenGLShader::OpenGLShader(std::string name, std::string_view vertexSrc, std::string_view fragmentSrc)
        : m_Name(std::move(name))
    {
        OLO_PROFILE_FUNCTION();

        std::unordered_map<GLenum, std::string> sources;
        sources[GL_VERTEX_SHADER] = vertexSrc;
        sources[GL_FRAGMENT_SHADER] = fragmentSrc;

        OLO_SHADER_COMPILATION_START(m_Name, "runtime_source");

        CompileOrGetVulkanBinaries(sources);
        if (Utils::IsAmdGpu())
        {
            CreateProgramForAmd();
        }
        else
        {
            CompileOrGetOpenGLBinaries();
            CreateProgram();
        }

        // Register compilation completion
        OLO_SHADER_COMPILATION_END(m_RendererID, m_RendererID != 0, "", 0.0);
    }

    OpenGLShader::~OpenGLShader()
    {
        OLO_PROFILE_FUNCTION();

        // Unregister the resource registry from Renderer3D
        if (m_RendererID != 0)
        {
            OloEngine::Renderer3D::UnregisterShaderRegistry(m_RendererID);
        }

        // Shutdown the resource registry
        m_ResourceRegistry.Shutdown();

        // Unregister from shader debugger
        OLO_SHADER_UNREGISTER(m_RendererID);

        // Track GPU memory deallocation
        OLO_TRACK_DEALLOC(this);

        glDeleteProgram(m_RendererID);
    }

    void OpenGLShader::InitializeResourceRegistry(const Ref<Shader>& shaderRef)
    {
        OLO_CORE_TRACE("OpenGLShader: InitializeResourceRegistry called for shader '{0}'", m_Name);
        m_ResourceRegistry.SetShader(shaderRef);
        m_ResourceRegistry.Initialize();
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

            if (pos != std::string::npos)
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

    std::unordered_map<GLenum, std::string> OpenGLShader::PreProcess(std::string_view source)
    {
        OLO_PROFILE_FUNCTION();

        std::string processedSource = ProcessIncludes(std::string(source));

        std::unordered_map<GLenum, std::string> shaderSources;

        const char* const typeToken = "#type";
        const sizet typeTokenLength = std::strlen(typeToken);
        sizet pos = processedSource.find(typeToken, 0);
        while (pos != std::string::npos)
        {
            const sizet eol = processedSource.find_first_of("\r\n", pos);
            OLO_CORE_ASSERT(eol != std::string::npos, "Syntax error");

            sizet typeStart = pos + typeTokenLength;
            while (typeStart < eol && (processedSource[typeStart] == ' ' || processedSource[typeStart] == '\t'))
            {
                typeStart++;
            }

            sizet typeEnd = typeStart;
            while (typeEnd < eol && processedSource[typeEnd] != ' ' && processedSource[typeEnd] != '\t' && processedSource[typeEnd] != '\r' && processedSource[typeEnd] != '\n')
            {
                typeEnd++;
            }

            std::string typeStr = processedSource.substr(typeStart, typeEnd - typeStart);
            GLenum shaderType = Utils::ShaderTypeFromString(typeStr);
            OLO_CORE_ASSERT(shaderType != 0, "Invalid shader type specified");

            const sizet nextLinePos = processedSource.find_first_not_of("\r\n", eol);
            OLO_CORE_ASSERT(nextLinePos != std::string::npos, "Syntax error");
            pos = processedSource.find(typeToken, nextLinePos);

            shaderSources[shaderType] = (pos == std::string::npos) ? processedSource.substr(nextLinePos) : processedSource.substr(nextLinePos, pos - nextLinePos);
        }

        return shaderSources;
    }

    void OpenGLShader::CompileOrGetVulkanBinaries(const std::unordered_map<GLenum, std::string>& shaderSources)
    {
        glCreateProgram();

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
            GLenum Stage;
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
            [&](i32 index)
            {
                if (hasError.load(std::memory_order_relaxed))
                    return; // Early exit if another stage failed

                const auto& [stage, source] = stageSourcePairs[index];
                CompilationResult& result = results[index];
                result.Stage = stage;

                const std::filesystem::path shaderFilePath = m_FilePath;
                const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedVulkanFileExtension(stage));
                result.CachePath = cachedPath;

                // Try to load from cache first
                std::ifstream in(cachedPath, std::ios::in | std::ios::binary);
                if (in.is_open() && !disableCache)
                {
                    in.seekg(0, std::ios::end);
                    const auto size = in.tellg();
                    in.seekg(0, std::ios::beg);

                    std::error_code ec;
                    std::filesystem::file_time_type cacheLastWriteTime = std::filesystem::last_write_time(cachedPath, ec);
                    std::filesystem::file_time_type shaderLastWriteTime = std::filesystem::last_write_time(shaderFilePath, ec);

                    if (!ec && shaderLastWriteTime <= cacheLastWriteTime)
                    {
                        result.SpirvData.resize(size / sizeof(u32));
                        in.read(reinterpret_cast<char*>(result.SpirvData.data()), size);
                        result.Success = true;
                        result.NeedsCache = false;
                        return;
                    }
                }

                // Compile the shader - each thread creates its own compiler and options
                // (shaderc is thread-safe but options are not shared)
                shaderc::Compiler compiler;
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                options.SetPreserveBindings(true);
                options.SetAutoBindUniforms(false);
                options.SetGenerateDebugInfo();
                options.SetOptimizationLevel(shaderc_optimization_level_performance);

                shaderc::SpvCompilationResult spirvModule = compiler.CompileGlslToSpv(
                    source, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str(), options);

                if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    result.Success = false;
                    result.ErrorMessage = spirvModule.GetErrorMessage();
                    hasError.store(true, std::memory_order_relaxed);
                    return;
                }

                result.SpirvData = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());
                result.Success = true;
                result.NeedsCache = !disableCache;
            });

        // Collect results and write cache (sequential to avoid map race conditions)
        for (const auto& result : results)
        {
            if (!result.Success)
            {
                OLO_CORE_ERROR(result.ErrorMessage);
                OLO_CORE_ASSERT(false);
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

        for (auto&& [stage, data] : shaderData)
        {
            Reflect(stage, data);
        }
    }

    void OpenGLShader::CompileOrGetOpenGLBinaries()
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
            GLenum Stage;
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
            [&](i32 index)
            {
                if (hasError.load(std::memory_order_relaxed))
                    return;

                const auto& [stage, vulkanSpirv] = stageSpirvPairs[index];
                OpenGLCompilationResult& result = results[index];
                result.Stage = stage;

                const std::filesystem::path shaderFilePath = m_FilePath;
                const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + Utils::GLShaderStageCachedOpenGLFileExtension(stage));
                result.CachePath = cachedPath;

                // Try to load from cache first
                std::ifstream in(cachedPath, std::ios::in | std::ios::binary);
                if (in.is_open() && !disableCache)
                {
                    std::error_code ec;
                    std::filesystem::file_time_type cacheLastWriteTime = std::filesystem::last_write_time(cachedPath, ec);
                    std::filesystem::file_time_type shaderLastWriteTime = std::filesystem::last_write_time(shaderFilePath, ec);

                    if (!ec && shaderLastWriteTime <= cacheLastWriteTime)
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

                // Compile GLSL to OpenGL SPIR-V
                shaderc::Compiler compiler;
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
                options.SetPreserveBindings(true);

                shaderc::SpvCompilationResult spirvModule = compiler.CompileGlslToSpv(
                    result.GlslSource, Utils::GLShaderStageToShaderC(stage), m_FilePath.c_str(), options);

                if (spirvModule.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    result.Success = false;
                    result.ErrorMessage = spirvModule.GetErrorMessage();
                    hasError.store(true, std::memory_order_relaxed);
                    return;
                }

                result.SpirvData = std::vector<u32>(spirvModule.cbegin(), spirvModule.cend());
                result.Success = true;
                result.NeedsCache = !disableCache;
            });

        // Collect results and write cache (sequential)
        for (const auto& result : results)
        {
            if (!result.Success)
            {
                OLO_CORE_ERROR(result.ErrorMessage);
                OLO_CORE_ASSERT(false);
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
    }

    void OpenGLShader::FinalizeProgram(GLenum const& program, const std::unordered_map<GLenum, std::vector<u32>>& spirvMap)
    {
        m_RendererID = program;

        // Compute estimated memory from the appropriate SPIR-V map
        sizet estimatedMemory = 0;
        for (const auto& [stage, spirv] : spirvMap)
        {
            estimatedMemory += spirv.size() * sizeof(u32);
        }
        estimatedMemory += 1024; // Additional overhead for program linking, uniforms, etc.

        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            estimatedMemory,
                            RendererMemoryTracker::ResourceType::Shader,
                            m_Name.empty() ? "OpenGL Shader" : m_Name);

        // Register shader
        OLO_SHADER_REGISTER_MANUAL(m_RendererID, m_Name, m_FilePath);
        OloEngine::Renderer3D::RegisterShaderRegistry(m_RendererID, &m_ResourceRegistry);

        // Store shader source code in debugger
        for (const auto& [stage, spirv] : spirvMap)
        {
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
    }

    void OpenGLShader::CreateProgram()
    {
        GLuint program = glCreateProgram();

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        const std::filesystem::path shaderFilePath = m_FilePath;
        const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ".cached_opengl.pgr");
        const bool disableCache = Utils::IsShaderCacheDisabled();

        // Try to load from program binary cache first
        if (std::ifstream in(cachedPath, std::ios::ate | std::ios::binary); in.is_open() && !disableCache)
        {
            std::error_code ec;
            std::filesystem::file_time_type cacheLastWriteTime = std::filesystem::last_write_time(cachedPath, ec);
            std::filesystem::file_time_type shaderLastWriteTime = std::filesystem::last_write_time(shaderFilePath, ec);

            if (!ec && shaderLastWriteTime <= cacheLastWriteTime)
            {
                const auto size = in.tellg();

                // Validate file size - must contain at least format u32
                if (size < static_cast<std::streamoff>(sizeof(u32)))
                {
                    OLO_CORE_WARN("Shader program binary cache file too small, recompiling: {}", cachedPath.string());
                    in.close();
                }
                else
                {
                    in.seekg(0);

                    u32 format = 0;
                    in.read(reinterpret_cast<char*>(&format), sizeof(u32));

                    if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(u32)))
                    {
                        OLO_CORE_WARN("Shader program binary cache: Failed to read format, recompiling: {}", cachedPath.string());
                        in.close();
                        glDeleteProgram(program);
                        program = glCreateProgram();
                        // Fall through to recompilation
                    }
                    else
                    {
                        const auto dataSize = size - static_cast<std::streamoff>(sizeof(u32));
                        auto data = std::vector<char>(dataSize);
                        in.read(data.data(), dataSize);

                        if (!in || in.gcount() != static_cast<std::streamsize>(dataSize))
                        {
                            OLO_CORE_WARN("Shader program binary cache: Failed to read data, recompiling: {}", cachedPath.string());
                            in.close();
                            glDeleteProgram(program);
                            program = glCreateProgram();
                            // Fall through to recompilation
                        }
                        else
                        {
                            in.close();

                            glProgramBinary(program, format, data.data(), static_cast<GLsizei>(data.size()));

                            GLint isLinked = 0;
                            glGetProgramiv(program, GL_LINK_STATUS, &isLinked);

                            if (isLinked == GL_TRUE)
                            {
                                FinalizeProgram(program, m_OpenGLSPIRV);
                                OLO_CORE_TRACE("Loaded shader program from binary cache: {0}", m_FilePath);
                                return;
                            }
                            else
                            {
                                OLO_CORE_WARN("Cached program binary failed to link, recompiling: {0}", shaderFilePath.string());
                                glDeleteProgram(program);
                                program = glCreateProgram();
                            }
                        }
                    }
                }
            }
            else if (ec)
            {
                OLO_CORE_TRACE("Could not check file timestamps for shader cache: {0}", m_FilePath);
            }
            else
            {
                OLO_CORE_TRACE("Shader source newer than cache, recompiling: {0}", shaderFilePath.string());
            }
        }

        // Compile from SPIR-V if cache miss or invalid
        std::vector<GLuint> shaderIDs;
        for (auto&& [stage, spirv] : m_OpenGLSPIRV)
        {
            const GLuint shaderID = shaderIDs.emplace_back(glCreateShader(stage));
            glShaderBinary(1, &shaderID, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.data(), static_cast<GLsizei>(spirv.size() * sizeof(u32)));
            glSpecializeShader(shaderID, "main", 0, nullptr, nullptr);
            glAttachShader(program, shaderID);
        }

        glLinkProgram(program);

        GLint isLinked{};
        glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
        if (GL_FALSE == isLinked)
        {
            GLint maxLength{};
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

            std::vector<GLchar> infoLog(maxLength);
            glGetProgramInfoLog(program, maxLength, &maxLength, infoLog.data());
            OLO_CORE_ERROR("Shader linking failed ({0}):\n{1}", m_FilePath, infoLog.data());

            glDeleteProgram(program);

            for (const auto id : shaderIDs)
            {
                glDeleteShader(id);
            }
            return;
        }

        // Clean up shader objects
        for (const auto id : shaderIDs)
        {
            glDetachShader(program, id);
            glDeleteShader(id);
        }

        // Save program binary to cache
        if (!disableCache)
        {
            GLint formats = 0;
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
            if (formats > 0)
            {
                Utils::CreateCacheDirectoryIfNeeded();
                GLint length = 0;
                glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
                if (length > 0)
                {
                    auto shaderData = std::vector<char>(length);
                    u32 format = 0;
                    glGetProgramBinary(program, length, nullptr, &format, shaderData.data());

                    std::ofstream out(cachedPath, std::ios::out | std::ios::binary);
                    if (out.is_open())
                    {
                        out.write(reinterpret_cast<char*>(&format), sizeof(u32));
                        out.write(shaderData.data(), static_cast<std::streamsize>(shaderData.size()));
                        out.flush();
                        out.close();
                        OLO_CORE_TRACE("Saved shader program binary to cache: {0}", cachedPath.string());
                    }
                }
            }
        }

        FinalizeProgram(program, m_OpenGLSPIRV);
    }

    static bool VerifyProgramLink(GLenum const& program)
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

            glDeleteProgram(program);

            OLO_CORE_ERROR("{0}", infoLog.data());
            OLO_CORE_ASSERT(false, "[OpenGL] Shader link failure!");
            return false;
        }
        return true;
    }

    void OpenGLShader::CreateProgramForAmd()
    {
        GLuint program = glCreateProgram();

        const std::filesystem::path cacheDirectory = Utils::GetCacheDirectory();
        const std::filesystem::path shaderFilePath = m_FilePath;
        const std::filesystem::path cachedPath = cacheDirectory / (shaderFilePath.filename().string() + ".cached_opengl.pgr");
        bool disableCache = Utils::IsShaderCacheDisabled();

        if (std::ifstream in(cachedPath, std::ios::ate | std::ios::binary); in.is_open() && !disableCache)
        {
            std::filesystem::file_time_type cacheLastWriteTime = std::filesystem::last_write_time(cachedPath);
            std::filesystem::file_time_type shaderLastWriteTime = std::filesystem::last_write_time(shaderFilePath);

            if (shaderLastWriteTime <= cacheLastWriteTime)
            {
                const auto size = in.tellg();
                in.seekg(0);

                auto data = std::vector<char>(size);
                u32 format = 0;
                in.read(reinterpret_cast<char*>(&format), sizeof(u32));
                in.read(data.data(), size);
                glProgramBinary(program, format, data.data(), static_cast<GLsizei>(data.size()));

                const bool linked = VerifyProgramLink(program);

                if (!linked)
                {
                    OLO_CORE_WARN("Cached program binary failed to link, recompiling: {0}", shaderFilePath.string());
                }
                else
                {
                    FinalizeProgram(program, m_VulkanSPIRV);
                    return;
                }
            }
            else
            {
                OLO_CORE_INFO("Shader source newer than cache, recompiling: {0}", shaderFilePath.string());
            }
        }

        std::array<u32, 2> glShadersIDs{};
        CompileOpenGLBinariesForAmd(program, glShadersIDs);
        glLinkProgram(program);

        if (const bool linked = VerifyProgramLink(program))
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
                    out.write(reinterpret_cast<char*>(&format), sizeof(u32));
                    out.write(shaderData.data(), shaderData.size());
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
    }

    void OpenGLShader::CompileOpenGLBinariesForAmd(GLenum const& program, std::array<u32, 2>& glShadersIDs) const
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

                OLO_CORE_ERROR("{0}", infoLog.data());
                OLO_CORE_ASSERT(false, "[OpenGL] Shader compilation failure!");
                return;
            }
            glAttachShader(program, shader);
            glShadersIDs[glShaderIDIndex++] = shader;
        }
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
        OLO_SHADER_RELOAD_START(m_RendererID);

        std::string source = ReadFile(m_FilePath);
        auto shaderSources = PreProcess(source);

        bool success = true;
        try
        {
            CompileOrGetVulkanBinaries(shaderSources);
            if (Utils::IsAmdGpu())
            {
                CreateProgramForAmd();
            }
            else
            {
                CompileOrGetOpenGLBinaries();
                CreateProgram();
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Shader reload failed for '{}': {}", m_Name, e.what());
            success = false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("Shader reload failed for '{}': unknown error", m_Name);
            success = false;
        }
        OLO_SHADER_RELOAD_END(m_RendererID, success);
    }

    void OpenGLShader::Bind() const
    {
        OLO_PROFILE_FUNCTION();

        glUseProgram(m_RendererID);

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::ShaderBinds, 1);

        // Track shader binding
        OLO_SHADER_BIND(m_RendererID);
    }

    void OpenGLShader::Unbind() const
    {
        OLO_PROFILE_FUNCTION();

        glUseProgram(0);
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
