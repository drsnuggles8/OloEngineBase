#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"

namespace OloEngine::Debug
{
    u32 NativeTextureIdForDiagnostics(const RenderGraph& graph, const RGTextureHandle handle)
    {
        // Native first, deliberately: it is the currency the overwhelming
        // majority of resources still carry, and asking the registry costs a
        // bounds + generation check per resource on a path that runs over every
        // registered resource in the graph.
        if (const u32 nativeId = graph.ResolveTexture(handle); nativeId != 0)
            return nativeId;

        return NativeTextureIdForDiagnostics(graph.ResolveTextureHandle(handle));
    }

    u32 NativeTextureIdForDiagnostics(const RHI::ResourceHandle identity)
    {
        if (!identity.IsValid())
            return 0;

        // A stale handle resolves to 0 here rather than to a name the driver
        // may since have reissued — so a diagnostic never reports a live object
        // for a dead resource, which would be worse than reporting nothing.
        return static_cast<u32>(RHI::GetNativeHandleForDebug(identity).Value);
    }

    u64 NativeHandleForDiagnostics(const RenderGraph& graph, const RGTextureHandle handle)
    {
        // Same order as the u32 form, and for the same reason: the native leg
        // is the currency most resources still carry and it costs no registry
        // lookup. A GL name widens losslessly, so nothing is lost by taking it
        // first — unlike the truncation on the way OUT, which is what #890 was.
        if (const u32 nativeId = graph.ResolveTexture(handle); nativeId != 0)
            return static_cast<u64>(nativeId);

        return NativeHandleForDiagnostics(graph.ResolveTextureHandle(handle));
    }

    u64 NativeHandleForDiagnostics(const RHI::ResourceHandle identity)
    {
        // A stale handle answers 0 rather than a name the driver may since have
        // reissued — a diagnostic must never report a live object for a dead
        // resource. Note this 0 is indistinguishable from the legitimate 0 a
        // live Vulkan image-less resource carries, which is exactly why this
        // value may be printed and never decided upon.
        if (!identity.IsValid())
            return 0;

        return RHI::GetNativeHandleForDebug(identity).Value;
    }

    bool HasLiveTextureStorage(const RHI::ResourceHandle identity)
    {
        if (!identity.IsValid() || !RHI::ResourceRegistry::Get().IsLive(identity))
            return false;

        // The backend's own answer. Deliberately NOT "the handle is live":
        // a registered handle whose image was destroyed and not yet retired
        // would pass that, and a diagnostic saying "backed" about storage that
        // is gone is the failure mode in the opposite direction.
        u32 width = 0;
        u32 height = 0;
        RenderCommand::GetTextureDimensions(identity, 0u, width, height);
        return width != 0 && height != 0;
    }

    bool HasLiveTextureStorage(const RenderGraph& graph, const RGTextureHandle handle)
    {
        // IDENTITY FIRST, and this ordering is load-bearing (issue #890).
        //
        // An identity means the backend CAN be asked, so its storage answer is
        // final — in BOTH directions. Testing the native leg first instead
        // looks harmless and is not: under OpenGL a transient the planner
        // never allocated still resolves to a recycled, non-zero GL name, so a
        // native-first check returns "backed" for a resource with no storage
        // and never runs the query at all. That is the exact false negative
        // this function exists to remove, and it survived a first pass of the
        // fix because every unit test around it supplied a null identity.
        //
        // The native leg is therefore reached ONLY when there is no identity
        // to interrogate — a resource imported as a bare native id — where a
        // non-zero name is the only evidence available.
        if (const RHI::ResourceHandle identity = graph.ResolveTextureHandle(handle); identity.IsValid())
            return HasLiveTextureStorage(identity);

        return graph.ResolveTexture(handle) != 0;
    }
} // namespace OloEngine::Debug
