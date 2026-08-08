#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace OloEngine
{
    RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;

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
                // Unreachable until Phase 5: this factory runs at static init (see
                // RenderCommand.cpp), before --rhi= is parsed, so s_API is still the
                // default here; and the Vulkan bring-up never routes RenderCommand.
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan has no RendererAPI implementation until #691 Phase 5!");
                return nullptr;
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
