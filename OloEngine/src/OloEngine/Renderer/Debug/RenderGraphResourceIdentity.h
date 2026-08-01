#pragma once

// =============================================================================
// RenderGraphResourceIdentity.h
//
// "What backend-native object backs this render-graph texture resource?" —
// asked by the introspection tools and the MCP capture endpoints they back
// (issue #691 Phase 2 step 3).
//
// WHY THIS EXISTS AS A SHARED FUNCTION RATHER THAN AN INLINE AT EACH CALLER.
// A PhysicalTexture carries a native id OR an identity, never both:
// ImportTexture supplies the first and leaves the identity null,
// ImportTextureHandle supplies the second and leaves TextureID at 0. That rule
// is deliberate — it is what keeps AllocateTextureHandle's change detection
// honest (see the ImportTextureCommon comment). Its consequence is that
// RenderGraph::ResolveTexture answers **0** for a handle-imported resource, so
// a caller that wants "whichever currency this resource happens to carry" has
// to try both, and one that does not SILENTLY LOSES the resource: it reports
// id 0, which is indistinguishable from a resource with no backing at all.
//
// That already happened. #732 migrated SSAO's noise texture to
// ImportTextureHandle and thereby removed it from olo_render_list_targets and
// olo_render_capture_target, with no warning and no failing test — and those
// endpoints are how CLAUDE.md's rendering-verification rule is enforced, so
// blinding them removes the check on the very slices doing the migrating.
//
// WHY IT LIVES IN Renderer/Debug/ SPECIFICALLY. Two constraints intersect here
// and this is the only directory that satisfies both:
//
//   * RHI::GetNativeHandleForDebug is baselined at zero uses outside
//     Renderer/Debug/ and Platform/ (`debug_escape_hatch` in
//     rhi_boundary_baseline.json). RHIResources.h names the introspection
//     tools and the MCP endpoints as its legitimate callers — this is them.
//   * The first home for this logic was OloEditor/src/MCP/, which
//     OloEngine-Tests does not link. That left the composition untestable,
//     which is precisely the configuration that let the original defect
//     through. Renderer/Debug/ is inside the engine library, so the test
//     target can reach it.
//
// Deliberately NOT a RenderGraph member: that would put the hatch inside
// Renderer/, where `backend_resolve_hatch` bans it. Moving that boundary is a
// decision to take on its own merits, not a side effect of a bug fix.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    class RenderGraph;
}

namespace OloEngine::Debug
{
    // The composition: try the native id, fall back to the identity. Returns 0
    // only when the resource genuinely has no backing in either currency —
    // which is the answer a diagnostic tool should print, and the ONLY case in
    // which it should.
    [[nodiscard]] u32 NativeTextureIdForDiagnostics(const RenderGraph& graph, RGTextureHandle handle);

    // The identity leg on its own, for a caller that already resolved one by
    // name (the graph's by-name lookups live on Renderer3D, not on RenderGraph).
    [[nodiscard]] u32 NativeTextureIdForDiagnostics(RHI::ResourceHandle identity);
} // namespace OloEngine::Debug
