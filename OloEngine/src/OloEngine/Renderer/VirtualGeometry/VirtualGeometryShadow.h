#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/mat4x4.hpp>
#include <span>

namespace OloEngine
{
    // @brief Virtual-geometry shadow casting (issue #629).
    //
    // Called by ShadowRenderPass per shadow view — every CSM cascade AND every
    // local-light atlas entry (spot tile / point-light cube face) — after the
    // classic mesh casters: runs the cluster cull in orthographic mode against
    // the view's light view-projection (no cone/backface rejection — shadow maps
    // rasterize back faces — and no software-raster routing), then replays each
    // shadow-casting instance's compacted command segment with the depth-only
    // VirtualMeshShadowDepth shader into the currently bound target + viewport.
    // Shares immutable mesh/instance inputs and retains view-owned cull outputs
    // and parameter uploads. Residency requests are GPU atomic ORs accumulated
    // in the registry across GPU-ordered shadow views and the main view.
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
        struct ViewResources
        {
            Ref<StorageBuffer> Commands;
            Ref<StorageBuffer> Args;
            Ref<StorageBuffer> Visible;
            Ref<UniformBuffer> CullParams;
            Ref<UniformBuffer> DrawInfo;
        };

        // Primary-only: resolve frame instances/residency and allocate every
        // view's output and upload objects before a recording region opens.
        [[nodiscard]] bool PrepareViews(std::span<ViewResources> views);

        // Renders this frame's virtual-mesh shadow casters into the currently
        // bound target + viewport. lightVPRel is the render-origin-relative light
        // view-projection; shadowResolution is the target (cascade or atlas tile)
        // size in texels, used only to scale the ortho LOD error to pixels.
        void RenderCascade(const glm::mat4& lightVPRel, u32 shadowResolution, ViewResources& resources);

        // Releases the lazily-created shaders (Renderer3D::Shutdown).
        void Shutdown();
    } // namespace VirtualGeometryShadow
} // namespace OloEngine
