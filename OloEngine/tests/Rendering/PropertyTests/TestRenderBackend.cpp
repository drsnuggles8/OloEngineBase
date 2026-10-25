#include "OloEnginePCH.h"
#include "TestRenderBackend.h"

#include "../../TestOptions.h"

#include "OloEngine/Renderer/GraphicsContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

#if OLO_WITH_VULKAN
#include "../VulkanTestSupport.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#endif

#include <cstdio>
#include <exception>
#include <optional>

namespace OloEngine::Tests::TestRenderBackend
{
    namespace
    {
        Scope<GraphicsContext> s_WindowlessContext;
        std::optional<Admission> s_Admission;
    } // namespace

    bool SelectFromOptions()
    {
        if (Options().Rhi != TestRhi::Vulkan)
            return true;
#if OLO_WITH_VULKAN
        // The Application constructor's order: the API first, then the facade,
        // and only then any context (rhi-abstraction-boundary.md, backend
        // selection). Nothing has touched the facade yet in this process.
        RendererAPI::SetAPI(RendererAPI::API::Vulkan);
        RenderCommand::RecreateForSelectedBackend();
        return true;
#else
        std::fprintf(stderr, "OloEngine-Tests: --olo-rhi=vulkan, but this binary was built with OLO_WITH_VULKAN=OFF.\n");
        return false;
#endif
    }

    bool IsVulkanProcess()
    {
        return Options().Rhi == TestRhi::Vulkan;
    }

    const char* EvidenceBackendPrefix()
    {
        return IsVulkanProcess() ? "Vulkan_" : "GL_";
    }

    std::string EvidenceName(const std::string_view feature)
    {
        return std::string(feature) + "_" + EvidenceBackendPrefix();
    }

    const Admission& EnsureWindowlessContext()
    {
        if (s_Admission)
            return *s_Admission;

        Admission admission;
#if OLO_WITH_VULKAN
        const VulkanDeviceTestGateResult probe = ProbeVulkanDeviceAvailability();
        if (!probe.Available)
        {
            admission.Reason = probe.Reason;
        }
        else
        {
            // Init throws to REFUSE (ADR 0010: never degrade), naming the
            // missing capability; that message is the skip reason.
            try
            {
                Scope<GraphicsContext> context =
                    GraphicsContext::CreateWindowless(kWindowlessTargetSize, kWindowlessTargetSize);
                if (!context)
                {
                    admission.Reason = "GraphicsContext::CreateWindowless returned no context for the Vulkan API";
                }
                else
                {
                    context->Init();
                    s_WindowlessContext = std::move(context);
                    admission.Available = true;
                    admission.DeviceName = probe.DeviceName;
                }
            }
            catch (const std::exception& e)
            {
                admission.Reason = std::string("windowless VulkanContext bring-up refused: ") + e.what();
            }
        }
#else
        admission.Reason = "this binary was built with OLO_WITH_VULKAN=OFF";
#endif
        s_Admission = std::move(admission);
        return *s_Admission;
    }

    FrameResult RunFrame(const std::function<void()>& tick)
    {
        FrameResult result;
        if (!IsVulkanProcess())
        {
            tick();
            result.Ran = true;
            return result;
        }
        if (!s_WindowlessContext)
            return result;

#if OLO_WITH_VULKAN
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        const u64 validationBefore = VulkanDevice::GetValidationErrorCount();
        const u64 stubsBefore = api.GetUnimplementedStubHitCount();
#endif
        bool ran = false;
        s_WindowlessContext->SetFrameRenderCallback(
            [&ran, &tick](const GraphicsContext::FrameRenderTarget&)
            {
                ran = true;
                tick();
                // The frame's content is wherever the scene put it; true lets
                // the context finalize the target if FinalRenderPass wrote it,
                // and its clear fallback covers a frame where nothing did.
                return true;
            });
        s_WindowlessContext->SwapBuffers();
        s_WindowlessContext->SetFrameRenderCallback({});
        result.Ran = ran;
#if OLO_WITH_VULKAN
        result.NewValidationErrors = VulkanDevice::GetValidationErrorCount() - validationBefore;
        result.NewStubHits = api.GetUnimplementedStubHitCount() - stubsBefore;
#endif
        return result;
    }

    void Shutdown()
    {
        s_WindowlessContext.reset();
    }
} // namespace OloEngine::Tests::TestRenderBackend
