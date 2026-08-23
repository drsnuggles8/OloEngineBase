#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"

#include "OloEngine/Renderer/Renderer.h"

// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): a factory TU
// may see Platform/<Backend>/ headers. This one stays neutral by including only
// the free-function declaration, not the FSR2 headers themselves.
#include "Platform/OpenGL/OpenGLTemporalUpscaler.h"

namespace OloEngine
{
    namespace
    {
        // Returned for every backend that has no temporal upscaler yet. It is a
        // real object, not a null Ref, so a caller always has something to ask
        // GetStatus() — the alternative is every call site inventing its own
        // "why is this null" story.
        class NoTemporalUpscaler final : public TemporalUpscaler
        {
          public:
            [[nodiscard]] TemporalUpscalerStatus GetStatus() const noexcept override
            {
                return TemporalUpscalerStatus::BackendUnsupported;
            }

            bool Configure(const TemporalUpscalerConfig&) override
            {
                return false;
            }

            bool Dispatch(const TemporalUpscalerDispatch&) override
            {
                return false;
            }

            [[nodiscard]] i32 GetJitterPhaseCount(u32, u32) const override
            {
                return 1;
            }

            [[nodiscard]] glm::vec2 GetJitterOffset(i32, i32) const override
            {
                return glm::vec2(0.0f);
            }
        };
    } // namespace

    Ref<TemporalUpscaler> TemporalUpscaler::Create()
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return CreateOpenGLTemporalUpscaler();
            case RendererAPI::API::Vulkan:
                // FSR2 does ship a Vulkan backend upstream, but wiring it needs
                // the Vulkan RHI's command buffer + image layout contract, which
                // is a separate piece of work from this one. Until then the
                // Vulkan path reports itself unsupported and the pipeline falls
                // back to the FSR1 spatial upscaler.
            case RendererAPI::API::None:
            default:
                return Ref<NoTemporalUpscaler>::Create().As<TemporalUpscaler>();
        }
    }
} // namespace OloEngine
