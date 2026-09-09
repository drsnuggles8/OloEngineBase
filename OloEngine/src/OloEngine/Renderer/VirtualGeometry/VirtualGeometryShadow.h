#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <span>
#include <vector>

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
    //
    // TWO ROUTES, not one replacing the other (issue #1149). The above is the
    // classic one and still runs everywhere VSM is off, which is the default.
    // RenderVirtualShadowMapLevels is the second: the same cull and the same
    // command stream, but per VSM CLIP LEVEL, gated on the dirty-page pyramid so
    // a cluster over pages that already hold valid texels never reaches a draw,
    // and rasterized through the page-table indirection instead of into a depth
    // attachment. What the two share — the ortho error scale and the watertight
    // DAG cut — is shared as code, not as a second copy.
    namespace VirtualGeometryShadow
    {
        struct ViewResources
        {
            Ref<StorageBuffer> Commands;
            Ref<StorageBuffer> Args;
            Ref<StorageBuffer> Visible;
            Ref<UniformBuffer> CullParams;
            Ref<UniformBuffer> DrawInfo;
            // VSM route only (issue #1149), created on first use by
            // RenderVirtualShadowMapLevels. The classic path takes both of these
            // from the recording item instead: its camera UBO is uploaded by
            // ShadowRenderPass::RenderCascadeOrFace, and it has no VSM pass block
            // at all. Safe to own here because the VSM raster is one sequential
            // region with a single writer, unlike the forked cascade region.
            Ref<UniformBuffer> VsmCamera;
            Ref<UniformBuffer> VsmPass;
        };

        // One clip level of the Virtual Shadow Map, as this route needs to see it
        // (issue #1149). Filled from VSM::ClipProjection by the caller, so this
        // header does not have to pull the whole shadow system in.
        struct VsmClipView
        {
            // The MATH flavour of the level's view-projection: the cluster cull
            // projects with it and then interprets the result (page footprint,
            // LOD error), so it must NOT carry Vulkan's y flip. The RASTER reads
            // the adjusted twin straight out of the VSM globals block instead.
            glm::mat4 ViewProjection{ 1.0f };
            // Wrapped page-table origin of this level, for the dirty-page gate.
            glm::ivec2 PageOffset{ 0 };
            u32 ClipLevel = 0;
        };

        // Primary-only: resolve frame instances/residency and allocate every
        // view's output and upload objects before a recording region opens.
        [[nodiscard]] bool PrepareViews(std::span<ViewResources> views);

        // Renders this frame's virtual-mesh shadow casters into the currently
        // bound target + viewport. lightVPRel is the render-origin-relative light
        // view-projection; shadowResolution is the target (cascade or atlas tile)
        // size in texels, used only to scale the ortho LOD error to pixels.
        void RenderCascade(const glm::mat4& lightVPRel, u32 shadowResolution, ViewResources& resources);

        // Renders this frame's virtual-mesh shadow casters into the Virtual
        // Shadow Map's pages (issue #1149), one cull + replay per supplied clip
        // level, into the raster scope the VSM has already bound.
        //
        // This is the SECOND route, not a replacement: RenderCascade above still
        // serves every configuration with VSM off, which is the default.
        //
        // What makes it page-driven rather than view-driven is the gate inside
        // the cluster cull — a cluster whose page footprint covers no page that
        // is being redrawn this frame never reaches the command stream. So a
        // level costs one dispatch and one empty indirect draw when the scene is
        // static, and the caller drops levels no instance touches at all.
        //
        // `virtualResolution` is the VSM's virtual resolution in texels; like
        // RenderCascade's shadowResolution it only scales the ortho LOD error to
        // pixels. Returns the number of levels that issued draws.
        //
        // `bindPhysicalPool` must call VirtualShadowMap::BindPhysicalPoolImage,
        // and is called here rather than by the caller because it has to land
        // BETWEEN this route's program bind and its draws: the bind forks on
        // whether the program currently in flight is bindless, so it cannot be
        // hoisted out of a shader switch. It is not optional even though the mesh
        // raster usually bound the pool a moment earlier — in a scene whose ONLY
        // casters are virtual, the mesh raster returns before binding anything,
        // and an unbound pool discards every imageAtomicMin in this pass and
        // renders a silently unshadowed frame.
        [[nodiscard]] u32 RenderVirtualShadowMapLevels(std::span<const VsmClipView> levels, u32 virtualResolution,
                                                       const std::function<void()>& bindPhysicalPool,
                                                       ViewResources& resources);

        // Loads this route's raster program and allocates its upload objects.
        //
        // Separate from PrepareViews, and called BEFORE the VSM opens its raster
        // scope, for two reasons: the classic cascades share PrepareViews and
        // must not pay for a shader they never bind, and a shader that fails to
        // load has to say so somewhere a diagnosis can see it — inside the raster
        // scope the only honest thing left to do is return zero draws, which
        // presents as "virtual geometry casts no shadow" with nothing logged.
        // Warns once on failure. Returns false when the route cannot run.
        [[nodiscard]] bool PrepareVirtualShadowMapRoute(ViewResources& resources);

        // One shadow-casting virtual instance, as the VSM route needs to see it
        // on the CPU (issue #1149).
        struct ShadowCasterBounds
        {
            // Render-origin-relative world AABB of this pose and the previous
            // one. Nothing else on the CPU knows these: the cluster cull does its
            // culling on the GPU, so the registry never needed them there.
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
            glm::vec3 PrevMin{ 0.0f };
            glm::vec3 PrevMax{ 0.0f };
            // Did the instance move since last frame?
            //
            // Decided from the TRANSFORMS, not from the bounds above, and the
            // difference is not academic: a rotation about the centre of a
            // symmetric object — a spinning turret, a 90-degree yaw snap of a
            // crate — leaves the world AABB bit-identical while the silhouette
            // the shadow map holds changes completely. Comparing the boxes would
            // call that "did not move" and freeze its shadow at the first angle.
            bool Moved = false;
        };

        // This frame's shadow-casting virtual instances, appended to `out`.
        //
        // Exists so the VSM can invalidate the pages a MOVING virtual caster
        // covers — a cached shadow page holding a mover's old silhouette is
        // exactly the artefact page caching produces when nobody re-dirties it —
        // and so the caller can drop clip levels no instance reaches.
        //
        // Requires PrepareViews (or RenderVirtualShadowMapLevels) to have run
        // this frame; returns false when there is no prepared frame to read.
        [[nodiscard]] bool CollectShadowCasterBounds(std::vector<ShadowCasterBounds>& out);

        // Releases the lazily-created shaders (Renderer3D::Shutdown).
        void Shutdown();
    } // namespace VirtualGeometryShadow
} // namespace OloEngine
