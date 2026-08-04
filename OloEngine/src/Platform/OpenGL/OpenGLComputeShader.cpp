#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLDebug.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderRegistry.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    OpenGLComputeShader::OpenGLComputeShader(const std::string& filepath)
        : m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

        // Extract name from filepath
        auto lastSlash = filepath.find_last_of("/\\");
        const auto lastDot = filepath.rfind('.');
        lastSlash = lastSlash == std::string::npos ? 0 : (lastSlash + 1);
        const auto count = lastDot == std::string::npos ? (filepath.size() - lastSlash) : (lastDot - lastSlash);
        m_Name = filepath.substr(lastSlash, count);

        // Reloadable by name like every other file-backed shader (issue #607) —
        // registered before the compile so a compute shader that fails to build
        // at boot can still be fixed on disk and reloaded without a restart.
        ShaderRegistry::Get().RegisterComputeShader(m_Name, this);

        const std::string rawSource = FileSystem::ReadFileText(filepath);
        if (rawSource.empty())
        {
            OLO_CORE_ERROR("Compute shader '{0}': failed to read source file '{1}'", m_Name, filepath);
            m_IsValid = false;
            return;
        }

        // Extract directory for resolving #include paths
        auto dirEnd = filepath.find_last_of("/\\");
        std::string directory = (dirEnd != std::string::npos) ? filepath.substr(0, dirEnd) : "";

        // Resolve #include directives (reuse the regular shader include processor)
        const std::string source = OpenGLShader::ProcessIncludes(rawSource, directory);
        if (source.empty())
        {
            OLO_CORE_ERROR("Compute shader '{0}': include processing returned empty source", m_Name);
            return;
        }

        OLO_SHADER_COMPILATION_START(m_Name, filepath);
        Compile(source);
        OLO_SHADER_COMPILATION_END(m_RendererID, m_IsValid, "", 0.0);
    }

    OpenGLComputeShader::OpenGLComputeShader(const std::string& name, const std::string& source)
        : m_Name(name)
    {
        OLO_PROFILE_FUNCTION();

        OLO_SHADER_COMPILATION_START(m_Name, "<from_source>");
        Compile(source);
        OLO_SHADER_COMPILATION_END(m_RendererID, m_IsValid, "", 0.0);
    }

    OpenGLComputeShader::~OpenGLComputeShader()
    {
        OLO_PROFILE_FUNCTION();

        // Drop the name->shader entry before this address can be recycled (see
        // the matching comment in ~OpenGLShader).
        ShaderRegistry::Get().UnregisterComputeShader(this);

        if (m_IsValid)
        {
            OLO_TRACK_DEALLOC(this);
        }
        OLO_SHADER_UNREGISTER(m_RendererID);

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

    void OpenGLComputeShader::Compile(const std::string& source)
    {
        OLO_PROFILE_FUNCTION();

        // The heap-bindless branch (issue #691 Phase 3, bucket 3).
        //
        // A compute shader needs NO second compile route: this function has
        // always handed include-resolved GLSL straight to glShaderSource, so it
        // never travelled the shaderc(vulkan) hop that rejects
        // GL_ARB_bindless_texture in the first place. All that is missing is the
        // prologue the graphics route injects, and for the identical reason —
        // GLSL requires every `#extension` directive to precede all
        // non-preprocessor tokens, so it cannot live in
        // include/BindlessHeap.glsl without imposing an invisible
        // "put the #include above your first declaration" rule on every file.
        //
        // Opt-in is the token itself, and the heap must be live: a shader
        // compiled while the toggle was off keeps its slot-based program until it
        // is reloaded, exactly as on the graphics route.
        std::string patched = source;
        m_IsBindlessVariant =
            RHI::DescriptorHeap::Get().IsEnabled() && source.find("OLO_BINDLESS") != std::string::npos;
        if (m_IsBindlessVariant)
        {
            static constexpr std::string_view kPrologue =
                "#extension GL_ARB_bindless_texture : require\n#define OLO_BINDLESS 1\n";
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
        }

        const u32 shader = glCreateShader(GL_COMPUTE_SHADER);
        const char* src = patched.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE)
        {
            GLint length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            std::string infoLog(static_cast<sizet>(length), '\0');
            glGetShaderInfoLog(shader, length, &length, infoLog.data());
            glDeleteShader(shader);
            OLO_CORE_ERROR("Compute shader compilation failed ({0}):\n{1}", m_Name, infoLog);

            // A broken bindless BRANCH must cost the dispatch its optimisation,
            // never its shader — same degradation policy as the graphics route,
            // and it matters more here because a compute pass that fails to
            // compile takes a whole system offline (no snow, no wind, no HZB)
            // rather than one draw. Retry the slot-based source once.
            if (m_IsBindlessVariant)
            {
                OLO_CORE_WARN("[Bindless] Compute shader '{0}' failed with the bindless branch; "
                              "falling back to the slot-based build.",
                              m_Name);
                m_IsBindlessVariant = false;
                const u32 retry = glCreateShader(GL_COMPUTE_SHADER);
                const char* plain = source.c_str();
                glShaderSource(retry, 1, &plain, nullptr);
                glCompileShader(retry);

                GLint retryCompiled = 0;
                glGetShaderiv(retry, GL_COMPILE_STATUS, &retryCompiled);
                if (retryCompiled != GL_FALSE)
                {
                    Link(retry, source);
                    return;
                }
                glDeleteShader(retry);
            }

            OLO_CORE_ASSERT(false, "Compute shader compilation failure!");
            return;
        }

        Link(shader, source);
    }

    void OpenGLComputeShader::Link(u32 shader, const std::string& source)
    {
        OLO_PROFILE_FUNCTION();

        m_RendererID = glCreateProgram();
        m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
        // Same registration as the graphics route — see OpenGLShader::FinalizeProgram.
        Shader::RegisterProgramBindless(m_RendererID, m_IsBindlessVariant);
        glAttachShader(m_RendererID, shader);
        glLinkProgram(m_RendererID);

        GLint linked = 0;
        glGetProgramiv(m_RendererID, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE)
        {
            GLint length = 0;
            glGetProgramiv(m_RendererID, GL_INFO_LOG_LENGTH, &length);
            std::string infoLog(static_cast<sizet>(length), '\0');
            glGetProgramInfoLog(m_RendererID, length, &length, infoLog.data());
            Shader::UnregisterProgram(m_RendererID);
            glDeleteProgram(m_RendererID);
            glDeleteShader(shader);
            m_RendererID = 0;
            m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
            OLO_CORE_ERROR("Compute shader link failed ({0}):\n{1}", m_Name, infoLog);
            OLO_CORE_ASSERT(false, "Compute shader link failure!");
            return;
        }

        glDetachShader(m_RendererID, shader);
        glDeleteShader(shader);

        // Name the program for GPU debuggers and the debug-callback label
        // registry (see OpenGLShader::FinalizeProgram for rationale).
        if (!m_Name.empty())
        {
            glObjectLabel(GL_PROGRAM, m_RendererID, -1, m_Name.c_str());
            RegisterGLProgramLabel(m_RendererID, m_Name);
        }

        // Estimate GPU memory: source size + driver overhead for compiled program
        const sizet estimatedMemory = source.size() + 1024;
        OLO_TRACK_GPU_ALLOC(this, estimatedMemory, RendererMemoryTracker::ResourceType::Shader, "OpenGL Compute Shader");

        OLO_SHADER_REGISTER_MANUAL(m_RendererID, m_Name, m_FilePath);
        m_IsValid = true;
        OLO_CORE_INFO("Compiled compute shader '{0}'{1}", m_Name, m_IsBindlessVariant ? " (bindless)" : "");
    }

    void OpenGLComputeShader::Bind() const
    {
        glUseProgram(m_RendererID);

        // PUBLISH WHETHER THIS PROGRAM READS THE HEAP, and this line is load-
        // bearing rather than bookkeeping. `Shader::SetBoundProgramBindless` is a
        // process-wide flag that the binding seam consults to decide between
        // writing an offset and issuing a bind. Before this, only
        // OpenGLShader::Bind() ever set it — so binding a bindless GRAPHICS
        // program and then a compute program left the flag TRUE, and the first
        // converted compute pass would have recorded offsets into a table its
        // program never declares while binding nothing at all.
        //
        // That failure has no diagnostic: the dispatch runs, reads an unwritten
        // image unit, and writes plausible garbage.
        Shader::SetBoundProgramBindless(m_IsBindlessVariant);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::ShaderBinds, 1);
        OLO_SHADER_BIND(m_RendererID);
    }

    void OpenGLComputeShader::Unbind() const
    {
        glUseProgram(0);

        // No program is bound, so no program reads the heap. Leaving the flag set
        // would let a bind issued between an Unbind() and the next Bind() take the
        // heap path on the strength of a program that is no longer in flight.
        Shader::SetBoundProgramBindless(false);
    }

    GLint OpenGLComputeShader::GetUniformLocation(const std::string& name) const
    {
        if (const auto it = m_UniformLocationCache.find(name); it != m_UniformLocationCache.end())
        {
            return it->second;
        }

        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        if (location == -1)
        {
            OLO_CORE_WARN("Compute shader '{0}': uniform '{1}' not found", m_Name, name);
        }
        m_UniformLocationCache[name] = location;
        return location;
    }

    void OpenGLComputeShader::SetInt(const std::string& name, int value) const
    {
        glProgramUniform1i(m_RendererID, GetUniformLocation(name), value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Int);
    }

    void OpenGLComputeShader::SetUint(const std::string& name, u32 value) const
    {
        glProgramUniform1ui(m_RendererID, GetUniformLocation(name), value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::UInt);
    }

    void OpenGLComputeShader::SetIntArray(const std::string& name, int* values, u32 count) const
    {
        glProgramUniform1iv(m_RendererID, GetUniformLocation(name), static_cast<GLsizei>(count), values);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::IntArray);
    }

    void OpenGLComputeShader::SetFloat(const std::string& name, f32 value) const
    {
        glProgramUniform1f(m_RendererID, GetUniformLocation(name), value);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float);
    }

    void OpenGLComputeShader::SetFloat2(const std::string& name, const glm::vec2& value) const
    {
        glProgramUniform2f(m_RendererID, GetUniformLocation(name), value.x, value.y);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float2);
    }

    void OpenGLComputeShader::SetFloat3(const std::string& name, const glm::vec3& value) const
    {
        glProgramUniform3f(m_RendererID, GetUniformLocation(name), value.x, value.y, value.z);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float3);
    }

    void OpenGLComputeShader::SetFloat4(const std::string& name, const glm::vec4& value) const
    {
        glProgramUniform4f(m_RendererID, GetUniformLocation(name), value.x, value.y, value.z, value.w);
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Float4);
    }

    void OpenGLComputeShader::SetMat4(const std::string& name, const glm::mat4& value) const
    {
        glProgramUniformMatrix4fv(m_RendererID, GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
        OLO_SHADER_UNIFORM_SET(m_RendererID, name, ShaderDebugger::UniformType::Mat4);
    }

    void OpenGLComputeShader::Reload()
    {
        OLO_PROFILE_FUNCTION();

        OLO_SHADER_RELOAD_START(m_RendererID);

        const std::string rawSource = FileSystem::ReadFileText(m_FilePath);
        if (rawSource.empty())
        {
            OLO_CORE_ERROR("Failed to reload compute shader '{0}': empty source", m_Name);
            OLO_SHADER_RELOAD_END(m_RendererID, false);
            return;
        }

        // Extract directory for resolving #include paths
        auto dirEnd = m_FilePath.find_last_of("/\\");
        std::string directory = (dirEnd != std::string::npos) ? m_FilePath.substr(0, dirEnd) : "";

        const std::string source = OpenGLShader::ProcessIncludes(rawSource, directory);
        if (source.empty())
        {
            OLO_CORE_ERROR("Compute shader '{0}': include processing returned empty source during reload", m_Name);
            OLO_SHADER_RELOAD_END(m_RendererID, false);
            return;
        }

        // Clean up old program
        if (m_IsValid)
        {
            OLO_TRACK_DEALLOC(this);
        }
        OLO_SHADER_UNREGISTER(m_RendererID);

        u32 oldProgramId = m_RendererID;
        UnregisterGLProgramLabel(oldProgramId);
        FrameResourceManager::Get().SubmitForDeletion([oldProgramId]()
                                                      {
                                                          // See Utils::UnbindProgramIfCurrent (issue #625): the
                                                          // reloaded-away program may still be bound by the time
                                                          // this deferred deletion runs.
                                                          Utils::UnbindProgramIfCurrent(oldProgramId);
                                                          Shader::UnregisterProgram(oldProgramId);
                                                          glDeleteProgram(oldProgramId); });

        m_RendererID = 0;
        m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, m_RendererID, RHI::Backend::OpenGL);
        m_IsValid = false;
        m_UniformLocationCache.clear();

        Compile(source);
        OLO_SHADER_RELOAD_END(m_RendererID, m_IsValid);
    }
} // namespace OloEngine
