#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderWarmup.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

namespace OloEngine
{
    Scope<Renderer::SceneData> Renderer::s_SceneData = CreateScope<Renderer::SceneData>();
    RendererType Renderer::s_RendererType;

    void Renderer::Init(RendererType type)
    {
        OLO_PROFILE_FUNCTION();

        RenderCommand::Init();
        s_RendererType = type;

        // Initialize boot + fallback shaders BEFORE any renderer loads shaders.
        // This ensures the warmup progress bar is available during all shader
        // compilation — 2D and 3D alike.
        ShaderWarmup::Init();
        ShaderLibrary::InitFallbackShader();

        switch (type)
        {
            case RendererType::Renderer2D:
                Renderer2D::Init();
                break;
            case RendererType::Renderer3D:
                // Scene always uses Renderer2D for 2D sprite/text overlays even
                // in 3D mode, so both renderers must be available.
                Renderer2D::Init();
                Renderer3D::Init();
                break;
        }
    }
    void Renderer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        // Renderer3D may have been lazily initialized (e.g. EditorLayer 3D mode)
        // regardless of the preferred renderer type — always shut it down if active.
        if (Renderer3D::IsInitialized())
            Renderer3D::Shutdown();

        // Renderer2D is always initialized (either as the preferred renderer, or
        // alongside Renderer3D for 2D overlay support). Shut it down unconditionally.
        Renderer2D::Shutdown();

        // Shutdown shared framebuffer resources (post-process shader)
        OpenGLFramebuffer::ShutdownSharedResources();

        // Boot + fallback shaders were initialized in Renderer::Init() before
        // any sub-renderer. Shut them down after all renderers are gone.
        // (Idempotent — safe even if Renderer3D::Shutdown already called these.)
        ShaderWarmup::Shutdown();
        ShaderLibrary::ShutdownFallbackShader();

        // Shutdown memory tracker after all renderers are shut down
        RendererMemoryTracker::GetInstance().Shutdown();
    }

    void Renderer::OnWindowResize(const u32 width, const u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Renderer::OnWindowResize called: {}x{}", width, height);

        RenderCommand::SetViewport(0, 0, width, height);

        // Update the active renderer's framebuffers
        switch (s_RendererType)
        {
            case RendererType::Renderer2D:
                // When implementing Renderer2D render graph, add call here
                break;
            case RendererType::Renderer3D:
                Renderer3D::OnWindowResize(width, height);
                break;
        }
    }

    void Renderer::BeginScene(OrthographicCamera const& camera)
    {
        s_SceneData->ViewProjectionMatrix = camera.GetViewProjectionMatrix();
    }

    void Renderer::EndScene()
    {
    }

    void Renderer::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
    {
        shader->Bind();
        shader->SetMat4("u_ViewProjection", s_SceneData->ViewProjectionMatrix);
        shader->SetMat4("u_Transform", transform);

        vertexArray->Bind();
        RenderCommand::DrawIndexed(vertexArray);
    }
} // namespace OloEngine
