#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTextureCubemapArray.h"

namespace OloEngine
{
    Ref<TextureCubemapArray> TextureCubemapArray::Create(const CubemapArraySpecification& specification)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan has no resource factories until #691 Phase 5/6!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTextureCubemapArray>::Create(specification);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
