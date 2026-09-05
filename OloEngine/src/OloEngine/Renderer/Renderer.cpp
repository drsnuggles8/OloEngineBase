#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/BC6HGpuEncoder.h"

#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
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

    void Renderer::Init(RendererType type, Window* loadingWindow)
    {
        OLO_PROFILE_FUNCTION();

        RenderCommand::Init();
        s_RendererType = type;

        // This thread owns the graphics context, and is therefore the only one the GPU
        // BC6H encoder may issue GL from (#624). Recorded here rather than at first use
        // because the cook that reaches for it runs on a worker and has to know where to
        // marshal to. No GL work and no hook installed by this — see BC6HGpuEncoder.h.
        BC6HGpu::SetContextThreadToCurrent();

        // Symmetric with the Shutdown() call at the bottom of Renderer::Shutdown(),
        // which is the only half that existed: Initialize() had NO callers, so the
        // tracker never sized its history buffers and — worse — never cleared the
        // m_IsShutdown latch its own comment says it exists to clear. After the first
        // Renderer::Shutdown() in a process, TrackDeallocation() early-returned while
        // TrackAllocation() kept recording, so every resource type read as a phantom
        // leaker across an Init/Shutdown/Init cycle (the test binary and the editor's
        // project reload both do exactly that). Needed here because Shutdown() now
        // REPORTS what is still tracked (#839) rather than silently clearing it.
        RendererMemoryTracker::GetInstance().Initialize();

        // Initialize boot + fallback shaders BEFORE any renderer loads shaders.
        // This ensures the warmup progress bar is available during all shader
        // compilation — 2D and 3D alike.
        ShaderWarmup::Init();
        ShaderLibrary::InitFallbackShader();

        switch (type)
        {
            case RendererType::Renderer2D:
                Renderer2D::Init(loadingWindow);
                break;
            case RendererType::Renderer3D:
                // Scene always uses Renderer2D for 2D sprite/text overlays even
                // in 3D mode, so both renderers must be available.
                Renderer2D::Init(loadingWindow);
                Renderer3D::Init(loadingWindow);
                break;
        }
    }
    void Renderer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        // Renderer3D may have been lazily initialized (e.g. EditorLayer 3D mode)
        // regardless of the preferred renderer type — always shut it down if init
        // ran. Guard on HasInitialized() ("Init ran"), not IsInitialized()
        // ("render graph ready"): a renderer that initialized at a 0x0 size (never
        // completed its graph build) still allocated one-shot singletons that must
        // be torn down, or FrameDataBufferManager leaks and asserts on next launch.
        if (Renderer3D::HasInitialized())
            Renderer3D::Shutdown();

        // Renderer2D is always initialized (either as the preferred renderer, or
        // alongside Renderer3D for 2D overlay support). Shut it down unconditionally.
        Renderer2D::Shutdown();

        // Shutdown shared framebuffer resources (post-process shader). GL-only
        // statics behind a GL-only entry point — the Vulkan framebuffer has no
        // shared-resource pool of its own (#691).
        if (RendererAPI::GetAPI() == RendererAPI::API::OpenGL)
            OpenGLFramebuffer::ShutdownSharedResources();

        // The GPU BC6H encoder's cached compute shaders and scratch textures (#624).
        // It is registered explicitly by whoever cooks, and may never have been used
        // at all — Shutdown is a no-op then. It has to happen HERE rather than at
        // static destruction, because that runs after the context is gone and the
        // memory tracker has reported.
        BC6HGpu::Shutdown();

        // Boot + fallback shaders were initialized in Renderer::Init() before
        // any sub-renderer. Shut them down after all renderers are gone.
        // (Idempotent — safe even if Renderer3D::Shutdown already called these.)
        ShaderWarmup::Shutdown();
        ShaderLibrary::ShutdownFallbackShader();

        // The shared fullscreen-triangle VAO. MeshPrimitives is NOT a 3D-only
        // facility — whoever draws a fullscreen pass first creates it, and on any
        // launch that is ShaderWarmup's progress frame, from Renderer2D::Init,
        // before (and sometimes instead of) Renderer3D::Init. Releasing it from
        // Renderer3D::Shutdown therefore leaked it in every session that never
        // brought 3D up — OloRuntime exits before 3D init when it finds no start
        // scene, and Vulkan answered with a surviving VertexArray plus two live
        // VMA allocations at vmaDestroyAllocator (#814); GL leaked the same
        // objects silently, having no allocator-teardown assertion.
        //
        // Position: after ShaderWarmup::Shutdown, the last consumer. It must stay
        // ahead of ShutdownGpuResources() below only on GL, where that call is
        // what releases the descriptor heap while the context is current
        // (OpenGLRendererAPI overrides it; on Vulkan it is the empty
        // RendererAPI base default, and the device is not destroyed until
        // VulkanContext::Shutdown runs from m_Window.reset()).
        //
        // Releasing here rather than inside Renderer3D::Shutdown is safe on both
        // backends — on GL, FrameResourceManager::SubmitForDeletion runs the
        // delete lambda immediately once the manager is down, so the VAO is
        // deleted with the context still current; on Vulkan the destructors
        // enqueue into VulkanDeferredReclaim, which VulkanContext::Shutdown
        // drains a final time just before VulkanDevice::Shutdown.
        MeshPrimitives::Shutdown();

        // The process-wide default font's two Slug atlas textures. Same class as the
        // triangle above and the same reason it belongs here rather than in
        // Renderer3D::Shutdown(): Font::GetDefault() is reached from an ECS component's
        // default member initializer (`Ref<Font> m_FontAsset = Font::GetDefault()` on
        // UITextComponent / TextComponent), so merely deserialising a scene with a text
        // component creates it — in a 2D session, a 3D session, or a headless one. Until
        // #839 it had NO release site anywhere; the atlases survived every session on
        // every backend, and only Vulkan's allocator teardown ever said so.
        Font::ShutdownDefault();

        // Release the descriptor heap and its backend WHILE THE CONTEXT IS STILL
        // CURRENT. ~OpenGLRendererAPI cannot do this: it runs from the static
        // destructor of RenderCommand::s_RendererAPI, i.e. at atexit, by which
        // time the window and its GL context are gone and every
        // glMakeTextureHandleNonResidentARB in the release path faults inside the
        // driver (issue #691).
        RenderCommand::ShutdownGpuResources();

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
