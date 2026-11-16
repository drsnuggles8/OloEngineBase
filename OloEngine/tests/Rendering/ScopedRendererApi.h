#pragma once

// =============================================================================
// ScopedRendererApi — select a backend for one scope and give back the value
// the scope found (#1693).
//
// `RendererAPI::SetAPI` is process-global, and the test binary runs as two
// processes: the default OpenGL one and `--olo-rhi=vulkan`. A scope that
// "restores" a constant instead of what it found is correct in exactly one of
// them. Nine device-test scopes restored `OpenGL`; in the Vulkan process the
// first of them to run left the process on OpenGL, every later scene test was
// refused its windowless context, and the run still reported ~9,980 passed.
//
// This only moves the selector. A scope that also swaps the RenderCommand
// facade is ScopedVulkanRenderCommandSelection (VulkanTestSupport.h).
// RendererBackendCheck fails any test that ends with the selector changed.
// =============================================================================

#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine::Tests
{
    class ScopedRendererApi
    {
      public:
        explicit ScopedRendererApi(const RendererAPI::API api)
            : m_Previous(RendererAPI::GetAPI())
        {
            RendererAPI::SetAPI(api);
        }

        ~ScopedRendererApi()
        {
            RendererAPI::SetAPI(m_Previous);
        }

        ScopedRendererApi(const ScopedRendererApi&) = delete;
        ScopedRendererApi& operator=(const ScopedRendererApi&) = delete;
        ScopedRendererApi(ScopedRendererApi&&) = delete;
        ScopedRendererApi& operator=(ScopedRendererApi&&) = delete;

        [[nodiscard]] RendererAPI::API Previous() const
        {
            return m_Previous;
        }

      private:
        RendererAPI::API m_Previous;
    };

    // The device suites' spelling: Vulkan's resource factories
    // (Texture2D::Create, StorageBuffer::Create, ...) switch on the selector.
    class ScopedVulkanApiSelection : public ScopedRendererApi
    {
      public:
        ScopedVulkanApiSelection()
            : ScopedRendererApi(RendererAPI::API::Vulkan)
        {
        }
    };
} // namespace OloEngine::Tests
