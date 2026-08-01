#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"

#include "OloEngine/Renderer/RHI/RHIResources.h"
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
} // namespace OloEngine::Debug
