#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GraphicsContext.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanContext.h"
#endif

namespace OloEngine
{
    Scope<GraphicsContext> GraphicsContext::Create(void* const window)
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
                return CreateScope<OpenGLContext>(static_cast<GLFWwindow*>(window));
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                return CreateScope<VulkanContext>(static_cast<GLFWwindow*>(window));
#else
                // Unreachable: BackendSelection degrades a Vulkan request to OpenGL
                // (with an error) when the backend is not compiled in, so s_API can
                // only be Vulkan in an OLO_WITH_VULKAN=1 binary.
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan selected but OLO_WITH_VULKAN=OFF!");
                return nullptr;
#endif
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
