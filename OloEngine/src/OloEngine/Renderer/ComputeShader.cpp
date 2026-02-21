#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"

namespace OloEngine
{
    Ref<ComputeShader> ComputeShader::Create(const std::string& filepath)
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
                return Ref<ComputeShader>(new OpenGLComputeShader(filepath));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
