#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanRendererAPI.h"
#endif

namespace OloEngine
{
    // constinit is load-bearing, not decoration: RenderCommand.cpp's namespace-scope
    // `s_RendererAPI = RendererAPI::Create()` reads s_API from ANOTHER TU during static
    // initialisation. That read is only well-defined because s_API is constant-initialised
    // and therefore already has its value before any dynamic initialiser runs. Giving s_API
    // a dynamic initialiser would turn that into an unordered cross-TU read -- the Tracy /
    // entt failure shape (issue #763). This annotation makes the compiler enforce it.
    constinit RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;

    void RendererAPI::BindTexture(u32 slot, RHI::ResourceHandle texture, const RHI::SamplerDesc& /*sampler*/)
    {
        // Default: the two-arg bind. GL's slot path samples with the texture
        // OBJECT's parameters and always has — an explicit desc is only
        // actionable on a backend whose samplers are separate objects (the
        // Vulkan override). Out of line because RendererAPI.h forward-declares
        // SamplerDesc rather than paying for RHIResources.h everywhere.
        BindTexture(slot, texture);
    }

    [[nodiscard("Store this!")]] Scope<RendererAPI> RendererAPI::Create()
    {
        switch (s_API)
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
                // Reachable ONLY through
                // RenderCommand::RecreateForSelectedBackend() — the static-init
                // construction (RenderCommand.cpp) always runs before --rhi=
                // parses and therefore always builds the OpenGL default
                // (ADR 0011 amendment (39)).
#if OLO_WITH_VULKAN
                return CreateScope<VulkanRendererAPI>();
#else
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan selected but OLO_WITH_VULKAN=OFF — the backend is not compiled in!");
                return nullptr;
#endif
            }
            case RendererAPI::API::OpenGL:
            {
                return CreateScope<OpenGLRendererAPI>();
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
