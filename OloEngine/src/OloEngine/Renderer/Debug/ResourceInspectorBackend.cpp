#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"
#include "OloEngine/Renderer/Renderer.h"

// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// factory TU may see Platform/OpenGL/ headers — the same shape as
// Framebuffer.cpp's Create switch, relocated next to the interface it mints.
#include "Platform/OpenGL/OpenGLResourceInspectorBackend.h"

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
                // Deliberate, documented stub (#691, ADR 0011 §1.6):
                // resource registration is a GL-side instrument — the
                // OLO_GPU_REGISTER_* macros fire only from Platform/OpenGL
                // TUs, so under Vulkan there is nothing to inspect here.
                // Vulkan visibility comes from the VMA/root-object registries.
                return nullptr;
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
