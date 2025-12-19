#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Texture.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTexture.h"

namespace OloEngine
{
    Ref<Texture2D> Texture2D::Create(const TextureSpecification& specification)
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
                return Ref<OpenGLTexture2D>::Create(specification);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<Texture2D> Texture2D::Create(const std::string& path)
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
                return Ref<OpenGLTexture2D>::Create(path);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
