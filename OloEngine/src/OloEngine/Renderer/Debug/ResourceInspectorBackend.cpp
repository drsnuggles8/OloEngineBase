#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"
#include "OloEngine/Renderer/Renderer.h"

// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// factory TU may see Platform/OpenGL/ headers — the same shape as
// Framebuffer.cpp's Create switch, relocated next to the interface it mints.
#include "Platform/OpenGL/OpenGLResourceInspectorBackend.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanResourceInspectorBackend.h"
#endif

namespace OloEngine
{
    std::unique_ptr<IResourceInspectorBackend> CreateResourceInspectorBackend()
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                // Headless / no renderer: the inspector is an optional
                // instrument, so no backend and no assert — every entry point
                // degrades to a no-op.
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // No longer a stub (#810). The OLO_GPU_REGISTER_* macros still
                // fire only from Platform/OpenGL TUs, so this arm DISCOVERS the
                // live set from RHI::ResourceRegistry instead of being pushed
                // into — see VulkanResourceInspectorBackend's class comment for
                // the mapping and what it deliberately cannot answer.
                return std::make_unique<VulkanResourceInspectorBackend>();
#else
                // Vulkan selected in a build that did not compile the backend
                // in: nothing to inspect, and no way to say so more precisely.
                return nullptr;
#endif
            }
            case RendererAPI::API::OpenGL:
            {
                return std::make_unique<OpenGLResourceInspectorBackend>();
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
