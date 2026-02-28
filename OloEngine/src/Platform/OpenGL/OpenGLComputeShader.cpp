#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLShader.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"

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

        const std::string rawSource = FileSystem::ReadFileText(filepath);
        if (!rawSource.empty())
        {
            // Resolve #include directives (reuse the regular shader include processor)
            const std::string source = OpenGLShader::ProcessIncludes(rawSource);

            OLO_SHADER_COMPILATION_START(m_Name, filepath);
            Compile(source);
            OLO_SHADER_COMPILATION_END(m_RendererID, m_IsValid, "", 0.0);
        }
    }

    OpenGLComputeShader::~OpenGLComputeShader()
    {
        OLO_PROFILE_FUNCTION();

        if (m_IsValid)
        {
            OLO_TRACK_DEALLOC(this);
        }
        OLO_SHADER_UNREGISTER(m_RendererID);
        glDeleteProgram(m_RendererID);
    }

    void OpenGLComputeShader::Compile(const std::string& source)
    {
        OLO_PROFILE_FUNCTION();

        const u32 shader = glCreateShader(GL_COMPUTE_SHADER);
        const char* src = source.c_str();
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
            OLO_CORE_ASSERT(false, "Compute shader compilation failure!");
            return;
        }

        m_RendererID = glCreateProgram();
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
            glDeleteProgram(m_RendererID);
            glDeleteShader(shader);
            m_RendererID = 0;
            OLO_CORE_ERROR("Compute shader link failed ({0}):\n{1}", m_Name, infoLog);
            OLO_CORE_ASSERT(false, "Compute shader link failure!");
            return;
        }

        glDetachShader(m_RendererID, shader);
        glDeleteShader(shader);

        OLO_TRACK_GPU_ALLOC(this, 0, RendererMemoryTracker::ResourceType::Shader, "OpenGL Compute Shader");

        OLO_SHADER_REGISTER_MANUAL(m_RendererID, m_Name, m_FilePath);
        m_IsValid = true;
        OLO_CORE_INFO("Compiled compute shader '{0}'", m_Name);
    }

    void OpenGLComputeShader::Bind() const
    {
        glUseProgram(m_RendererID);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::ShaderBinds, 1);
        OLO_SHADER_BIND(m_RendererID);
    }

    void OpenGLComputeShader::Unbind() const
    {
        glUseProgram(0);
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

        const std::string source = OpenGLShader::ProcessIncludes(rawSource);

        // Clean up old program
        if (m_IsValid)
        {
            OLO_TRACK_DEALLOC(this);
        }
        OLO_SHADER_UNREGISTER(m_RendererID);
        glDeleteProgram(m_RendererID);
        m_RendererID = 0;
        m_IsValid = false;
        m_UniformLocationCache.clear();

        Compile(source);
        OLO_SHADER_RELOAD_END(m_RendererID, m_IsValid);
    }
} // namespace OloEngine
