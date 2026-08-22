#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIGpuFence.h"
#include "OloEngine/Renderer/Renderer.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanGpuFence.h"
#endif

namespace OloEngine::RHI
{
    Ref<GpuFence> GpuFence::Create(u64 initialValue)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    // The factory's contract is "null on failure" — every
                    // failure mode, including a vkCreateSemaphore throw from
                    // the backend constructor, must reach callers the same way.
                    try
                    {
                        return Ref<GpuFence>(new VulkanGpuFence(initialValue));
                    }
                    catch (const std::exception& e)
                    {
                        OLO_CORE_ERROR("RHI::GpuFence::Create: {} — returning null", e.what());
                        return nullptr;
                    }
                }
#endif
                OLO_CORE_WARN("RHI::GpuFence::Create: RendererAPI::Vulkan but no live VulkanDevice "
                              "(or OLO_WITH_VULKAN compiled out) — returning null");
                return nullptr;
            }
            case RendererAPI::API::None:
            case RendererAPI::API::OpenGL:
            default:
            {
                // No GL implementation yet (ADR 0011 §6: migrating the
                // GL-side FrameResourceManager fence chain is a follow-up).
                // Callers branch on null, the same contract as
                // DescriptorHeap::IsBindlessSupported().
                return nullptr;
            }
        }
    }
} // namespace OloEngine::RHI
