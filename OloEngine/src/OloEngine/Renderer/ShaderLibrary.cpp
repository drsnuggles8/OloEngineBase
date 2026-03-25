#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
    Ref<Shader> ShaderLibrary::s_FallbackShader = nullptr;

    void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
    {
        OLO_CORE_ASSERT(!Exists(name), "Shader '{}' already exists!", name);
        m_Shaders[name] = shader;

        // Register with shader debugger — but only if the shader is already
        // ready.  For async shaders (Compiling), registration happens later
        // inside FinalizeProgram.  Registering now would call GetRendererID()
        // while the ShaderDebugger mutex is held, which triggers EnsureLinked
        // → FinalizeProgram → OLO_SHADER_REGISTER_MANUAL → deadlock on the
        // same non-recursive mutex.
        if (shader->IsReady())
        {
            OLO_SHADER_REGISTER(shader);
        }
    }

    void ShaderLibrary::Add(const Ref<Shader>& shader)
    {
        auto& name = shader->GetName();
        Add(name, shader);
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
    {
        auto shader = Shader::Create(filepath);
        Add(shader);
        return shader;
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
    {
        auto shader = Shader::Create(filepath);
        Add(name, shader);
        return shader;
    }

    Ref<Shader> ShaderLibrary::Get(const std::string& name)
    {
        OLO_CORE_ASSERT(Exists(name), "Shader '{}' not found!", name);
        return m_Shaders[name];
    }

    void ShaderLibrary::ReloadShaders()
    {
        for (auto& [name, shader] : m_Shaders)
        {
            shader->Reload();
        }
    }

    bool ShaderLibrary::Exists(const std::string& name) const
    {
        return m_Shaders.contains(name);
    }

    std::vector<std::string> ShaderLibrary::GetAllShaderNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Shaders.size());
        for (const auto& [name, shader] : m_Shaders)
        {
            names.push_back(name);
        }
        return names;
    }

    // ====================================================================
    // Async shader compilation support
    // ====================================================================

    u32 ShaderLibrary::PollPendingShaders()
    {
        OLO_PROFILE_FUNCTION();

        u32 completed = 0;
        for (auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling)
            {
                if (shader->PollCompilationStatus())
                {
                    ++completed;
                    // Now that the shader is finalized, initialize its resource registry
                    if (shader->IsReady())
                    {
                        auto* glShader = static_cast<OpenGLShader*>(shader.get());
                        glShader->InitializeResourceRegistry(shader);
                    }
                }
            }
            // Catch shaders that became Ready outside the library (e.g. via EnsureLinked/Bind)
            // but whose registry was never initialized.
            else if (shader->IsReady())
            {
                if (auto* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                    ++completed;
                }
            }
        }
        return completed;
    }

    void ShaderLibrary::FlushPendingShaders()
    {
        OLO_PROFILE_FUNCTION();

        for (auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling)
            {
                shader->EnsureLinked();
                if (shader->IsReady())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                }
            }
            // Same guard as PollPendingShaders — catch ready-but-uninitialized.
            else if (shader->IsReady())
            {
                if (auto* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                }
            }
        }
    }

    u32 ShaderLibrary::GetPendingCount() const
    {
        u32 count = 0;
        for (const auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling || shader->GetCompilationStatus() == ShaderCompilationStatus::Pending)
            {
                ++count;
            }
            else if (shader->IsReady())
            {
                if (auto const* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    ++count;
                }
            }
        }
        return count;
    }

    bool ShaderLibrary::HasPendingShaders() const
    {
        for (const auto& [name, shader] : m_Shaders)
        {
            auto status = shader->GetCompilationStatus();
            if (status == ShaderCompilationStatus::Compiling || status == ShaderCompilationStatus::Pending)
                return true;
            if (shader->IsReady())
            {
                if (auto const* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                    return true;
            }
        }
        return false;
    }

    // ====================================================================
    // Fallback shader — solid magenta, compiled synchronously
    // ====================================================================

    static constexpr const char* s_FallbackVertexSrc = R"glsl(
#version 450 core
layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjectionMatrix;
    mat4 u_ViewMatrix;
    mat4 u_ProjectionMatrix;
    vec4 u_CameraPosition;
};

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

void main()
{
    gl_Position = u_ViewProjectionMatrix * u_Model * vec4(a_Position, 1.0);
}
)glsl";

    static constexpr const char* s_FallbackFragmentSrc = R"glsl(
#version 450 core
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Octahedral encoding: maps a unit normal to [-1,1]^2
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return n.xy;
}

void main()
{
    // Magenta — instantly recognizable as "shader not ready"
    o_Color = vec4(1.0, 0.0, 1.0, 1.0);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(vec3(0.0, 0.0, 1.0));
}
)glsl";

    void ShaderLibrary::InitFallbackShader()
    {
        if (s_FallbackShader)
            return;

        s_FallbackShader = Shader::Create("__Fallback", s_FallbackVertexSrc, s_FallbackFragmentSrc);
        OLO_CORE_INFO("Fallback shader initialized (magenta)");
    }

    void ShaderLibrary::ShutdownFallbackShader()
    {
        s_FallbackShader.Reset();
    }

    Ref<Shader> ShaderLibrary::GetFallbackShader()
    {
        return s_FallbackShader;
    }
} // namespace OloEngine
