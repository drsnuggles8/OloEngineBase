#include "OloEnginePCH.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLStorageBuffer.h"

namespace OloEngine
{
    Ref<StorageBuffer> StorageBuffer::Create(u32 size, u32 binding, StorageBufferUsage usage)
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
                return Ref<StorageBuffer>(new OpenGLStorageBuffer(size, binding, usage));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
