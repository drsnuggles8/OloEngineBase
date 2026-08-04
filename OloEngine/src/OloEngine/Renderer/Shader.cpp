#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shader.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <unordered_set>

namespace OloEngine
{
    namespace
    {
        // Not atomic: written and read only from the render thread, once per
        // program bind. See Shader.h.
        bool s_BoundProgramIsBindless = false;

        // Native program ids whose program was built through the bindless route.
        //
        // WHY A REGISTRY AND NOT JUST THE FLAG ABOVE. `OpenGLShader::Bind()`
        // publishes the flag from its own `m_IsBindlessVariant`, but the command
        // layer does not go through it: `CommandDispatch` binds programs by
        // handle through `RendererAPI::BindShaderProgram`, which reaches
        // `glUseProgram` without any Shader object in sight. The flag was
        // therefore STALE for every command-dispatched draw — carrying whatever
        // the last post-process shader happened to set.
        //
        // That is not a cosmetic staleness; it renders wrong in both directions:
        //   * stale TRUE for an unconverted program -> the binding seam records an
        //     offset and SKIPS the bind, so the shader's sampler unit is empty.
        //   * stale FALSE for a converted program -> the seam binds the texture,
        //     but the shader reads an OFFSET that was never written.
        // The second is what turned the editor's sky black with the heap on.
        //
        // Keyed on the NATIVE id because that is what both BindShaderProgram
        // overloads funnel into, and it is the only identity available at the
        // point the publication has to happen.
        std::unordered_set<u32> s_BindlessPrograms;
    } // namespace

    auto Shader::IsBoundProgramBindless() -> bool
    {
        return s_BoundProgramIsBindless;
    }

    void Shader::SetBoundProgramBindless(const bool bindless)
    {
        s_BoundProgramIsBindless = bindless;
    }

    void Shader::RegisterProgramBindless(const u32 programID, const bool bindless)
    {
        if (programID == 0u)
        {
            return;
        }
        if (bindless)
        {
            s_BindlessPrograms.insert(programID);
        }
        else
        {
            // Erase rather than skip: a reload can turn a bindless program back
            // into a slot-based one, and GL reissues freed program names — so a
            // stale entry would mark an unrelated future program bindless.
            s_BindlessPrograms.erase(programID);
        }
    }

    void Shader::UnregisterProgram(const u32 programID)
    {
        s_BindlessPrograms.erase(programID);
    }

    auto Shader::IsProgramBindless(const u32 programID) -> bool
    {
        return s_BindlessPrograms.contains(programID);
    }

    auto Shader::AnyBindlessProgramsExist() -> bool
    {
        return !s_BindlessPrograms.empty();
    }

    Ref<Shader> Shader::Create(const std::string& filepath)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                auto shader = Ref<OpenGLShader>::Create(filepath);
                // Initialize the resource registry after construction — only if
                // the shader is already fully linked (sync path or cache hit).
                // For async shaders, this is deferred to PollPendingShaders().
                if (shader->IsReady())
                    static_cast<OpenGLShader*>(shader.get())->InitializeResourceRegistry(shader);
                return shader;
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<Shader> Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                auto shader = Ref<OpenGLShader>::Create(name, vertexSrc, fragmentSrc);
                // Initialize the resource registry after construction
                if (shader->IsReady())
                    static_cast<OpenGLShader*>(shader.get())->InitializeResourceRegistry(shader);
                return shader;
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
