#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/mat4x4.hpp>

namespace OloEngine
{
    // @brief Virtual-geometry shadow casting (issue #629, slice 6).
    //
    // Called by ShadowRenderPass per shadow view — every CSM cascade AND every
    // local-light atlas entry (spot tile / point-light cube face) — after the
    // classic mesh casters: runs the cluster cull in orthographic mode against
    // the view's light view-projection (no cone/backface rejection — shadow maps
    // rasterize back faces — and no software-raster routing), then replays each
    // shadow-casting instance's compacted command segment with the depth-only
    // VirtualMeshShadowDepth shader into the currently bound target + viewport.
    // Reuses the per-frame instance/command/args buffers the main pass also uses
    // (the cull overwrites them per view; the main pass re-culls afterwards).
    //
    // The ortho error scale is exact for the cascades' orthographic VPs and a
    // conservative approximation for the atlas' perspective VPs; the DAG cut is
    // watertight at any threshold, so either way the shadow stays crack-free.
    //
    // The shadow camera UBO (binding 0) must already carry the view's
    // render-origin-relative light VP — exactly what RenderCascadeOrFace
    // uploads before invoking this.
    namespace VirtualGeometryShadow
    {
        // Renders this frame's virtual-mesh shadow casters into the currently
        // bound target + viewport. lightVPRel is the render-origin-relative light
        // view-projection; shadowResolution is the target (cascade or atlas tile)
        // size in texels, used only to scale the ortho LOD error to pixels.
        void RenderCascade(const glm::mat4& lightVPRel, u32 shadowResolution);

        // Releases the lazily-created shaders (Renderer3D::Shutdown).
        void Shutdown();
    } // namespace VirtualGeometryShadow
} // namespace OloEngine
