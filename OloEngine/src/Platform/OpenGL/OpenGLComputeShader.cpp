#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <fstream>

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

        const std::string source = ReadFile(filepath);
        Compile(source);
    }

    OpenGLComputeShader::~OpenGLComputeShader()
    {
        OLO_PROFILE_FUNCTION();

        OLO_TRACK_DEALLOC(this);
        glDeleteProgram(m_RendererID);
    }

    std::string OpenGLComputeShader::ReadFile(const std::string& filepath)
    {
        std::string result;
        if (std::ifstream in(filepath, std::ios::in | std::ios::binary); in)
        {
            in.seekg(0, std::ios::end);
            const auto size = in.tellg();
            if (std::cmp_not_equal(size, -1))
            {
                result.resize(static_cast<sizet>(size));
                in.seekg(0, std::ios::beg);
                in.read(result.data(), size);
            }
            else
            {
                OLO_CORE_ERROR("Could not read compute shader file '{0}'", filepath);
            }
        }
        else
        {
            OLO_CORE_ERROR("Could not open compute shader file '{0}'", filepath);
        }
        return result;
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

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::ShaderBinds, 1);
        OLO_TRACK_GPU_ALLOC(this, 0, RendererMemoryTracker::ResourceType::Shader, "OpenGL Compute Shader");

        OLO_CORE_INFO("Compiled compute shader '{0}'", m_Name);
    }

    void OpenGLComputeShader::Bind() const
    {
        glUseProgram(m_RendererID);
    }

    void OpenGLComputeShader::Unbind() const
    {
        glUseProgram(0);
    }

    GLint OpenGLComputeShader::GetUniformLocation(const std::string& name) const
    {
        const GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        if (location == -1)
        {
            OLO_CORE_WARN("Compute shader '{0}': uniform '{1}' not found", m_Name, name);
        }
        return location;
    }

    void OpenGLComputeShader::SetInt(const std::string& name, int value) const
    {
        glProgramUniform1i(m_RendererID, GetUniformLocation(name), value);
    }

    void OpenGLComputeShader::SetFloat(const std::string& name, f32 value) const
    {
        glProgramUniform1f(m_RendererID, GetUniformLocation(name), value);
    }

    void OpenGLComputeShader::SetFloat2(const std::string& name, const glm::vec2& value) const
    {
        glProgramUniform2f(m_RendererID, GetUniformLocation(name), value.x, value.y);
    }

    void OpenGLComputeShader::SetFloat3(const std::string& name, const glm::vec3& value) const
    {
        glProgramUniform3f(m_RendererID, GetUniformLocation(name), value.x, value.y, value.z);
    }

    void OpenGLComputeShader::SetFloat4(const std::string& name, const glm::vec4& value) const
    {
        glProgramUniform4f(m_RendererID, GetUniformLocation(name), value.x, value.y, value.z, value.w);
    }

    void OpenGLComputeShader::SetMat4(const std::string& name, const glm::mat4& value) const
    {
        glProgramUniformMatrix4fv(m_RendererID, GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
    }
} // namespace OloEngine
