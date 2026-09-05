#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Debug/StagedBufferReadback.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RGPreparedPass.h"
// Included, not forward-declared: `Ref<T>`'s destructor calls RefUtils::Release,
// which needs T complete wherever ~VirtualShadowMap is instantiated. Since
// ShadowMap.h includes this header, "wherever" is every consumer of the shadow
// system — ShadowMapTest.cpp is the one that found it (public headers must be
// self-contained, CLAUDE.md → Conventions).
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct ShadowMeshCaster;
    struct ShadowSkinnedCaster;

    // =========================================================================
    // Virtual Shadow Maps — sparse page-table directional shadows (issue #702)
    //
    // Replaces the fixed 4-cascade CSM for the directional light with a sparse,
    // GPU-driven page table: N concentric clip levels of a VSM_VIRTUAL_RESOLUTION²
    // virtual shadow map, backed on demand by a much smaller physical pool of
    // VSM_PAGE_SIZE² pages. A page is drawn once and KEPT — clip frusta move in
    // whole-page increments and the light matrix slides along a plane parallel to
    // its near plane, so a cached page's texels stay valid while the camera
    // translates. Pages entering a clip frustum reuse the table slots of pages
    // leaving it (2D wraparound addressing).
    //
    // Allocation, freeing and marking all run ON THE GPU. That is not a stylistic
    // choice: sparse/tiled texture APIs cannot be driven indirectly, so using them
    // would mean a readback round trip to learn which pages need backing — and a
    // round trip is exactly the shadow pop-in this system exists to avoid.
    //
    // Adapted from Timberdoodle's virtual_shadow_maps (Apache-2.0,
    // https://github.com/Sunset-Flock/Timberdoodle): the page-state bit layout,
    // the wrapped-coordinate scheme, the free/allocate split and the clip-level
    // heuristic. Reworked for GL 4.6 core — see VirtualShadowCommon.glsl for the
    // three places this deviates and why.
    //
    // ---- The frame, and why it straddles a frame boundary --------------------
    //
    // Page marking needs the SCENE DEPTH buffer, which does not exist yet when the
    // shadow pass runs (ShadowPass is the first node in the graph). So marking is
    // its own late graph node (VirtualShadowMapMarkPass, registered after
    // lighting) and the pages it marks are consumed by the NEXT frame's shadow
    // pass. Ordering, per frame N:
    //
    //   [shadow pass, start of frame N]   consuming frame N-1's marks
    //     1. FreeWrappedPages   — slots whose world page scrolled out of a clip
    //                             frustum, plus a full flush on light/origin change
    //                             (and, for local layers, on a light that moved)
    //     2. InvalidatePages    — dynamic casters re-dirty the pages they touch
    //     3. FindFreePages      — build the free list (unallocated, then LRU)
    //     4. AllocatePages      — back each request, mark it dirty
    //     5. ClearDirtyPages    — reset the physical texels of every dirty page
    //     6. BuildHPB           — 2x2 MAX reduction of the DIRTY flag, 7 mips
    //                             (then again per local layer, 6 mips)
    //     7. CullCasters + raster — indirect draws into the dirty pages only,
    //                             directional first, then the local layers
    //     8. EndFrame           — clear DIRTY / VISITED / REQUESTS for the marker
    //
    //   [mark pass, end of frame N]
    //     9. MarkRequiredPages  — project every depth texel into its clip level,
    //                             AND into every local light whose range reaches
    //                             it; request unallocated pages, VISIT allocated
    //                             ones
    //
    // ---- Local lights (issue #703) -------------------------------------------
    //
    // Point and spot lights read the SAME page table, SAME physical pool, SAME
    // allocator and SAME eviction policy as the clip levels. They differ in three
    // places and nowhere else:
    //
    //   * a LAYER instead of a clip level (six per point light, one per spot,
    //     from one flat 256-deep pool), assigned per light identity so a light
    //     keeps its cached pages while it stands still;
    //   * a MIP instead of a level — the layer is the face, the mip is how finely
    //     it is being resolved this frame, chosen per texel from the camera and
    //     light distances. This is the mechanism behind "detail scales with
    //     screen footprint": a distant light resolves to mip 5 and spends ONE
    //     page on a whole cube face;
    //   * a PERSPECTIVE projection instead of an ortho one, so the depth bias is
    //     a metre-to-NDC conversion rather than a constant.
    //
    // What it replaces is ShadowAtlas' priority ranking: there is no score, no
    // rank and no tile tier, because a layer costs nothing until a page under it
    // is actually on screen. `AtlasCasterRecord::Allocated` — the flag that
    // recorded a starved caster — is still filled, and it is what shows the
    // starvation case is gone.
    //
    // Step 8 must precede step 9 and step 3 of frame N+1 must follow it: VISITED
    // is what keeps a page off the LRU eviction list, so clearing it after the
    // marker ran would evict every resident page every frame.
    //
    // The one-frame lag means a surface disoccluded this frame has no fine page
    // yet. That is why sampling FALLS BACK to coarser clip levels instead of
    // reading "unshadowed": a coarse level is large and almost always resident, so
    // the worst case is one frame of blurrier shadow rather than a missing one.
    // =========================================================================

    namespace VSM
    {
        // Mirrors of include/VirtualShadowCommon.glsl. VirtualShadowMapTest pins
        // each against the shader text — a drift here silently reinterprets every
        // page entry, and nothing else would notice.
        inline constexpr u32 kVirtualResolution = 4096;
        inline constexpr u32 kPageSize = 64;
        inline constexpr u32 kPageSizeLog2 = 6;
        inline constexpr u32 kPageTableResolution = kVirtualResolution / kPageSize; // 64
        inline constexpr u32 kPagesPerClipLevel = kPageTableResolution * kPageTableResolution;
        inline constexpr u32 kClipLevels = 16;
        inline constexpr u32 kTotalVirtualPages = kPagesPerClipLevel * kClipLevels;

        // --- Local lights (issue #703) ---------------------------------------
        //
        // A point light owns six consecutive LAYERS (its cube faces, in the same
        // +X,-X,+Y,-Y,+Z,-Z order the atlas path uses), a spot light owns one,
        // and both come from ONE flat pool — so a scene spends its layers on the
        // mix of lights it actually has rather than on a fixed point/spot split.
        //
        // Unlike a clip level, a layer is MIPPED: mip m halves the resolution,
        // and the mip is chosen per texel from the light and camera distances
        // (VirtualShadowMap::SelectLocalMip). That is what makes a light's
        // detail follow its screen footprint instead of an authored rank — the
        // issue's second acceptance criterion.
        inline constexpr u32 kLocalVirtualResolution = 2048;
        inline constexpr u32 kLocalPageTableResolution = kLocalVirtualResolution / kPageSize; // 32
        inline constexpr u32 kLocalMipCount = 6;                                              // 32..1
        inline constexpr u32 kLocalPagesPerLayer = 1365;                                      // sum of (32 >> m)² for m in [0,6)
        inline constexpr u32 kMaxLocalLayers = 256;                                           // 6 x 32 point faces + 64 spots, as one pool
        inline constexpr u32 kTotalLocalPages = kLocalPagesPerLayer * kMaxLocalLayers;
        // The page table holds the clip levels first, then the layers, in one
        // buffer sharing one physical pool and one allocator.
        inline constexpr u32 kTotalPageTableEntries = kTotalVirtualPages + kTotalLocalPages;

        inline constexpr u32 kHPBMipCount = 7;
        inline constexpr u32 kHPBEntriesPerLevel = 5461; // sum of (64 >> m)² for m in [0,7)
        inline constexpr u32 kHPBTotalEntries = kHPBEntriesPerLevel * kClipLevels;
        inline constexpr u32 kLocalHPBEntriesPerLayer = kLocalPagesPerLayer;
        inline constexpr u32 kLocalHPBTotalEntries = kLocalHPBEntriesPerLayer * kMaxLocalLayers;
        inline constexpr u32 kTotalHPBEntries = kHPBTotalEntries + kLocalHPBTotalEntries;

        // Page-state bits — the C++ half of the encoding documented in the GLSL.
        inline constexpr u32 kPageAllocatedBit = 1u << 31;
        inline constexpr u32 kPageRequestsAllocBit = 1u << 30;
        inline constexpr u32 kPageDirtyBit = 1u << 29;
        inline constexpr u32 kPageVisitedBit = 1u << 28;
        inline constexpr u32 kPageAllocFailedBit = 1u << 27;

        inline constexpr u32 kMetaAllocatedBit = 1u << 31;
        inline constexpr u32 kMetaVisitedBit = 1u << 30;
        inline constexpr u32 kMetaLocalBit = 1u << 29;

        // An allocation request's `.w`. Directional requests write zero, which is
        // what the pre-#703 marker produced, so the fork is a set bit rather than
        // a value the old encoding could reach.
        inline constexpr u32 kRequestLocalBit = 1u << 31;
        // The mip rides the same word, beneath the flag. Mirrors
        // VSM_REQUEST_MIP_MASK; pinned disjoint from the flag by
        // VirtualShadowMapLocal.MetaLocalOwnerRoundTripsAndCannotAliasADirectionalOwner.
        inline constexpr u32 kRequestMipMask = 0x7u;

        // Allocation-request ring capacity. Mirrors VSM_MAX_REQUESTS.
        inline constexpr u32 kMaxRequests = 16384;

        // Per-frame CPU-side budgets. Each is a hard cap that degrades by dropping
        // work and saying so, never by writing past the end of a buffer:
        //   kMaxCasters       — shadow-casting mesh instances submitted per frame
        //   kMaxBatches       — distinct (VAO, index range, cull mode) groups
        //   kMaxDrawInstances — (caster x clip level) pairs the cull may emit,
        //                       plus the CPU-scheduled skinned records at the tail
        inline constexpr u32 kMaxCasters = 8192;
        // Halved in EFFECT by issue #703, not in value: the local cull writes a
        // second command per batch, at index `batchCount + b`, so a frame with
        // local lights uses two commands per batch out of this one budget.
        // RenderCasters caps the batch count at half of it when local lights are
        // active rather than growing the buffer, because a batch is a distinct
        // submesh and 512 of them is already an unusual scene.
        inline constexpr u32 kMaxBatches = 1024;
        inline constexpr u32 kMaxDrawInstances = 131072;
        // How many LAYERS one caster may be submitted to per frame. The local
        // twin of kClipLevels for run sizing: a caster inside three point lights
        // at once needs 18, and the cap costs it the surplus — counted in
        // Statistics::CullOverflows, never silently.
        inline constexpr u32 kLocalLayersPerCaster = 16;

        // One clip level's projection, as the GPU sees it (std140).
        //
        // Both the previous and the current page offset ride along because
        // FreeWrappedPages cannot decide from the current one alone whether a slot
        // changed owner: the offsets are taken modulo the table resolution, so a
        // move of exactly one whole table looks like no move at all. It needs the
        // previous offset AND the unwrapped delta.
        struct ClipProjection
        {
            // THE MATH flavour (ADR 0011 (59): a seam is defined by how a value is
            // READ). Raw on both backends. Everything that projects a world
            // position and then interprets the result itself uses this one: the
            // page marker, the cull, the invalidator and the lit pass's sampling.
            glm::mat4 ViewProjection{ 1.0f }; // 0   — render-relative world -> clip
            // THE RASTERIZER flavour — the same matrix through
            // RHI::AdjustProjectionForBackend, so it carries Vulkan's y flip and
            // z remap. ONLY the depth raster's gl_Position reads it. Identical to
            // the above on GL.
            //
            // Both are carried rather than one being derived in-shader because the
            // adjustment is a CPU-side function, and because keeping them apart is
            // what makes the physical pool's CONTENTS backend-identical: the
            // raster undoes the y flip when it turns gl_FragCoord into a virtual
            // texel (see include/VirtualShadowRasterStage.glsl), so a page holds
            // the same texels on both backends and sampling needs no fork at all.
            glm::mat4 ViewProjectionRaster{ 1.0f }; // 64
            glm::ivec2 PageOffset{ 0 };             // 128 — this frame's origin in pages, mod kPageTableResolution
            glm::ivec2 PrevPageOffset{ 0 };         // 136 — last frame's, same wrapping
            glm::ivec2 PageDelta{ 0 };              // 144 — unwrapped origin delta, saturated at +-kPageTableResolution
            f32 HalfExtent = 0.0f;                  // 152 — world half-width covered by this level
            f32 TexelWorldSize = 0.0f;              // 156 — world size of one virtual texel
        };
        static_assert(sizeof(ClipProjection) == 160, "VSM::ClipProjection std140 size drifted (160 B)");

        // UBO_VIRTUAL_SHADOW. Read by every VSM kernel, by the depth raster and by
        // every lit shader that samples the map, so all three agree on the clip
        // projections and the level heuristic without a second upload path.
        struct GlobalsUBO
        {
            std::array<ClipProjection, kClipLevels> Clips{}; // 0    (2560 B)
            glm::mat4 InverseViewProjection{ 1.0f };         // 2560 — depth NDC -> render-relative world (marking)
            glm::vec4 LightDirection{ 0.0f };                // 2624 — xyz normalized, w unused
            glm::vec4 CameraPosition{ 0.0f };                // 2640 — xyz render-relative, w unused
            // x = clip 0 half extent (m), y = clip-selection bias,
            // z = depth bias (clip units), w = normal bias (m)
            glm::vec4 Params0{ 0.0f }; // 2656
            // x = softness, y = max shadow distance, z = physical resolution,
            // w = physical page-table resolution
            glm::vec4 Params1{ 0.0f }; // 2672
            // x = enabled, y = debug mode, z = full invalidate, w = frame index
            glm::ivec4 Params2{ 0 }; // 2688
            // x = depth width, y = depth height, z = marking stride, w = unused
            glm::ivec4 Params3{ 0 }; // 2704
            // Local lights (issue #703). LayerCount is the layer pool's
            // high-water mark — what the cull and the HPB build dispatch over;
            // LightCount is how many b_LocalLightHead entries are valid — what
            // the MARKER walks. They differ because one point light spends six
            // layers, and conflating them either over-dispatches the cull or
            // makes the marker miss five faces out of six.
            glm::ivec4 Params4{ 0 }; // 2720
            // x = local detail bias, y = local depth bias (metres)
            glm::vec4 Params5{ 0.0f }; // 2736

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(GlobalsUBO));
            }
        };
        static_assert(sizeof(GlobalsUBO) % 16 == 0, "VSM::GlobalsUBO must be 16-byte aligned for std140");
        static_assert(sizeof(GlobalsUBO) == 2752, "VSM::GlobalsUBO std140 size drifted from GLSL expectation (2752 B)");

        // One local-light LAYER, as the GPU sees it (std430). C++ twin of
        // VSMLocalLight in include/VirtualShadowResources.glsl.
        struct LocalLight
        {
            // Same two flavours, same split, as ClipProjection: the raw matrix
            // for anything that projects and then interprets the result (the
            // marker, the cull, the sampler), the adjusted one for gl_Position.
            glm::mat4 ViewProjection{ 1.0f };       // 0
            glm::mat4 ViewProjectionRaster{ 1.0f }; // 64
            glm::vec4 PositionRange{ 0.0f };        // 128 — xyz render-relative position, w range
            // x = near, y = far (= range), z = face index (0..5; 0 for a spot),
            // w = kind: 0 unused, 1 spot, 2 point.
            glm::vec4 Params{ 0.0f }; // 144
        };
        static_assert(sizeof(LocalLight) == 160, "VSM::LocalLight std430 size drifted (160 B)");

        // Kind codes for LocalLight::Params.w. Spelled as constants because the
        // marker's walk and the cull's skip both compare against them.
        inline constexpr f32 kLocalKindUnused = 0.0f;
        inline constexpr f32 kLocalKindSpot = 1.0f;
        inline constexpr f32 kLocalKindPoint = 2.0f;

        // UBO_VIRTUAL_SHADOW_DRAW — per-dispatch / per-draw scratch, refilled
        // immediately before each use (the #691 pattern). Two disjoint
        // consumers share it because they never overlap in time:
        //   .x — VSM_BuildHPB: the mip being written
        //      — VSM_CullCasters: the caster count
        //      — the depth raster: the base of this batch's compacted instance run
        //
        // The CLIP LEVEL is deliberately NOT here: it travels per instance, which
        // is what lets one indirect draw per batch cover all 16 levels instead of
        // 16 draws covering one each.
        struct PassUBO
        {
            glm::uvec4 Params{ 0u };

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(PassUBO));
            }
        };
        static_assert(sizeof(PassUBO) == 16, "VSM::PassUBO std140 size drifted from GLSL expectation (16 B)");

        // Cull input: one entry per (caster, batch) submitted this frame (std430).
        struct CullInstance
        {
            glm::mat4 Transform{ 1.0f }; // 0  — render-relative model matrix
            glm::vec4 BoundsMin{ 0.0f }; // 64 — render-relative world AABB, w unused
            glm::vec4 BoundsMax{ 0.0f }; // 80
            // x = batch index, y = the batch's instance-run base,
            // z = the batch's instance-run capacity, w = 1 when bounds are absent
            // (an unbounded caster skips the frustum/HPB tests and always draws)
            glm::uvec4 Batch{ 0u }; // 96
            // The same three numbers for the LOCAL cull, which runs over the same
            // caster array but writes a disjoint command range and a disjoint
            // instance run (issue #703). Carried per caster rather than derived
            // in the kernel because the CPU is what sizes both runs, and a
            // kernel-side derivation would be a second copy of that arithmetic.
            // x = command index, y = run base, z = run capacity, w = unused.
            glm::uvec4 LocalBatch{ 0u }; // 112
        };
        static_assert(sizeof(CullInstance) == 128, "VSM::CullInstance std430 size drifted (128 B)");

        // Cull output: one entry per surviving (caster, clip level) pair, or per
        // surviving (caster, local layer) pair (std430). The two rasters read
        // different fields of the same record and never share a run.
        struct DrawInstance
        {
            glm::mat4 Transform{ 1.0f };
            u32 ClipLevel = 0;
            u32 LocalLayer = 0;
            u32 Pad1 = 0;
            u32 Pad2 = 0;
        };
        static_assert(sizeof(DrawInstance) == 80, "VSM::DrawInstance std430 size drifted (80 B)");

        // GL DrawElementsIndirectCommand. The cull writes InstanceCount; every
        // other field is CPU-authored from the batch.
        struct DrawCommand
        {
            u32 IndexCount = 0;
            u32 InstanceCount = 0;
            u32 FirstIndex = 0;
            u32 BaseVertex = 0;
            u32 BaseInstance = 0;
        };
        static_assert(sizeof(DrawCommand) == 20, "VSM::DrawCommand must match GL DrawElementsIndirectCommand (20 B)");

        // GPU-side counters, mirrored to the CPU one frame late (see
        // VirtualShadowMap::GetStatistics).
        struct Statistics
        {
            u32 PagesRequested = 0; // pages the marker asked to have backed
            u32 PagesAllocated = 0; // requests satisfied this frame
            u32 PagesFailed = 0;    // requests the physical pool could not satisfy
            u32 PagesDrawn = 0;     // pages actually redrawn — acceptance criterion #2
            u32 PagesResident = 0;  // physical pages currently backing a virtual page
            u32 PagesFreed = 0;     // pages released by wraparound / invalidation
            u32 DrawInstances = 0;  // (caster x clip level) pairs that survived the cull
            u32 CullOverflows = 0;  // pairs dropped because a batch run was full
            // The local-light subsets of Resident / Drawn (issue #703). Split
            // out rather than inferred, because the whole claim of this feature
            // is that local shadows now cost pages in proportion to what is on
            // screen — and a total that mixes them with the sun's cannot show it.
            u32 LocalPagesResident = 0;
            u32 LocalPagesDrawn = 0;
        };
    } // namespace VSM

    // Runtime knobs. Lives on ShadowSettings so it serializes and edits with the
    // rest of the shadow configuration.
    struct VirtualShadowMapSettings
    {
        // Off by default: VSM covers static + skinned MESH casters only. Terrain,
        // foliage, voxel and virtualized-geometry casters still render through the
        // CSM path, so a scene that relies on those must keep CSM. Turning this on
        // replaces the directional CSM and leaves the local-light atlas untouched.
        //
        // Backend-neutral: the only difference between the GL and Vulkan routes is
        // one line in include/VirtualShadowRasterStage.glsl that undoes Vulkan's
        // flipped fragment origin, which keeps the physical pool's CONTENTS
        // identical on both.
        bool Enabled = false;

        // Physical pool side length in texels. 4096² R32UI = 64 MB / 4096 pages.
        // 2048² = 16 MB / 1024 pages, which is exactly the VRAM a default 4x1024
        // CSM costs — the equal-VRAM comparison the issue's first acceptance
        // criterion asks for. Clamped to [1024, 8192] and rounded to a page
        // multiple by Init().
        u32 PhysicalResolution = 4096;

        // World half-extent of clip level 0. Level L covers this * 2^L, so the
        // default 2 m gives a 1 mm texel at level 0 and reaches 2 * 2^15 = 65 km
        // at level 15 — the "shadow distance well past 200 m" criterion, without
        // the near field paying for it.
        f32 Clip0HalfExtent = 2.0f;

        // Scales the distance fed to the clip-level heuristic. > 1 biases toward
        // coarser (cheaper, blurrier) levels, < 1 toward finer ones.
        f32 ClipSelectionBias = 1.0f;

        // Light-space depth half-range around the render origin, in metres. Fixed
        // and camera-INDEPENDENT on purpose: a depth range that tracked the camera
        // would rescale every cached page's stored depth the moment the camera
        // moved along the light axis, which is precisely the caching invariant
        // this system is built on.
        f32 DepthRange = 4096.0f;

        // Depth bias in METRES, converted to the ortho range's [0,1] depth on
        // upload. Authored in world units on purpose: the clip levels share one
        // fixed depth range, so a raw clip-space bias silently changes meaning the
        // moment DepthRange is retuned. The normal offset is world-space too, and
        // is additionally scaled by the selected level's texel size in the shader.
        f32 DepthBiasMeters = 0.05f;
        f32 NormalBias = 0.02f;

        // Point / spot lights read the same page table instead of the budgeted
        // atlas (issue #703). ON by default under Enabled, deliberately: a
        // sub-toggle nobody sets is a feature with zero coverage, and the whole
        // point of the layer pool is that it does not need a per-scene budget
        // decision the way the atlas' 16-light / 32-entry cap did.
        //
        // Turning this off leaves the local-light atlas exactly as it was, so
        // "VSM for the sun, atlas for the lamps" stays a supported combination.
        bool LocalLights = true;

        // Scales the mip a local light's texels resolve to. > 1 pushes toward
        // coarser (cheaper, blurrier) mips, < 1 toward finer. The local twin of
        // ClipSelectionBias, and like it, changing it invalidates every cached
        // page because the producer and the consumer must agree on the choice.
        //
        // DEFAULT 2.0, i.e. ONE MIP COARSER than directional-matched, and the
        // number is measured rather than taste. vsmLocalMipForDistances is
        // calibrated to give a local face the same world texel size a clip level
        // gives at the same camera distance — which is right for ONE light and
        // profligate for twenty, because every light that reaches a pixel needs
        // its own page for it while the sun needs one for all of them. At 1.0 a
        // point light standing near the camera measured 1033 resident pages of a
        // 4096-page pool; four such lights fill it.
        //
        // One mip coarser is 4x fewer pages (~260) and is exactly the trade the
        // ATLAS already made explicitly — ShadowAtlas::TileSizeForRank gives a
        // point light HALF its tier resolution per face, for the same reason
        // ("six full-tier faces would exhaust the atlas area"), and notes a cube
        // face covers a 90° slice so half resolution is comparable texel density
        // to a spot cone anyway. This lands a near light at roughly the atlas'
        // top-tier face resolution and lets distance take everything else down
        // from there.
        f32 LocalDetailBias = 2.0f;

        // Depth bias for local lights, in METRES, converted per sample to the
        // [0,1] depth the light's perspective projection produces at that
        // distance (vsmLocalDepthBias). Metres rather than NDC because a
        // perspective depth's metre-per-unit scale falls off as 1/d²: one NDC
        // constant is either useless near the light or a peter-pan at its range,
        // over a range that differs per light.
        f32 LocalDepthBiasMeters = 0.02f;

        // 0 = off, 1 = clip level tint, 2 = page address, 3 = residency,
        // 4 = shadow test, 5 = stored depth, 6 = receiver depth,
        // 7 = final shadow factor. Consumed by the lit pass through
        // GlobalsUBO::Params2.y.
        i32 DebugMode = 0;

        auto operator==(const VirtualShadowMapSettings&) const -> bool = default;
    };

    // The system itself. Owned by ShadowMap, driven by ShadowRenderPass (steps
    // 1-8) and VirtualShadowMapMarkPass (step 9).
    class VirtualShadowMap
    {
      public:
        VirtualShadowMap() = default;
        ~VirtualShadowMap();

        VirtualShadowMap(const VirtualShadowMap&) = delete;
        VirtualShadowMap& operator=(const VirtualShadowMap&) = delete;

        void Init(const VirtualShadowMapSettings& settings);
        void Shutdown();
        void SetSettings(const VirtualShadowMapSettings& settings);
        [[nodiscard]] const VirtualShadowMapSettings& GetSettings() const
        {
            return m_Settings;
        }

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }
        // True only when the settings enable it AND initialisation produced every
        // resource. Callers branch on THIS, never on Settings.Enabled, so a failed
        // shader load degrades to CSM instead of rendering nothing.
        [[nodiscard]] bool IsActive() const
        {
            return m_Settings.Enabled && m_Initialized && !m_Suppressed;
        }

        // The shadow system as a whole was switched off (ShadowSettings::Enabled).
        //
        // A separate flag rather than clearing Settings.Enabled, because that one
        // means "the user chose VSM over CSM" and SetSettings RECREATES the whole
        // resource set when it changes — so routing a per-frame global toggle
        // through it would destroy and rebuild the physical pool every time
        // somebody ticked the shadows checkbox.
        //
        // This gap was real and predates local lights: with shadows disabled,
        // ShadowRenderPass early-outs but VirtualShadowMapMarkPass does NOT (it
        // asks IsActive(), which knew nothing about ShadowSettings), so marking
        // continued, BindForSampling kept publishing an ENABLED globals block, and
        // the lit pass kept sampling a page table nothing was rasterizing into.
        // Local lights are what made it visible — a test that disabled shadows to
        // get a no-shadow control still got shadows, from a previous scene's
        // layers.
        void SetSuppressed(bool suppressed)
        {
            m_Suppressed = suppressed;
        }

        // --- Step 0: per-frame setup (CPU) -----------------------------------
        //
        // Recomputes the 16 clip projections for this frame's light direction and
        // camera position, snapping each level's origin to whole pages so cached
        // texels survive the move, and records the resulting page-offset delta for
        // FreeWrappedPages. `renderOrigin` is the camera-relative render origin
        // (issue #429): a change to it re-anchors light space and therefore
        // invalidates every cached page, which this detects.
        void BeginFrame(const glm::vec3& lightDirection, const glm::vec3& cameraPositionRelative,
                        const glm::vec3& renderOrigin, bool directionalEnabled = true);

        // --- Local lights (issue #703) ----------------------------------------
        //
        // Called from the scene's shadow setup INSTEAD OF ShadowAtlas::Allocate
        // when local VSM is active, and in the same place, because both need the
        // same inputs and both patch the same per-light shadow index.
        //
        //   BeginLocalLights();
        //   for each shadow-casting local light: base = RegisterLocalLight(...)
        //   EndLocalLights();
        //
        // There is no scoring and no ranking. A light either gets its layers or
        // the 256-layer pool is full — and a layer costs no memory of its own,
        // only the pages its on-screen footprint actually asks for. That is the
        // starvation case going away, not being re-prioritised.
        struct LocalLightDesc
        {
            // Stable identity, so a light keeps its LAYERS across frames and
            // therefore keeps its cached pages. Zero means "no identity", which
            // forces a fresh layer every frame — correct, but it redraws
            // everything, so callers should pass the light entity's UUID.
            u64 LightId = 0;
            glm::vec3 PositionRelative{ 0.0f };
            glm::vec3 Direction{ 0.0f, 0.0f, -1.0f }; // spot only
            f32 Range = 0.0f;
            f32 OuterCutoffDegrees = 0.0f; // spot only
            bool IsPoint = true;           // point / sphere-area (6 layers) vs spot (1)
        };

        static constexpr u32 kNoLocalSlot = ~0u;

        // The layer pool, as a self-contained unit with no GPU dependency.
        //
        // Extracted rather than inlined into VirtualShadowMap for one reason: its
        // behaviour is what decides whether the local cache works at all, and
        // every way it can be wrong is invisible on screen. A pool that reassigns
        // layers every frame is CORRECT and redraws everything; one that evicts a
        // slot without flushing its pages leaves the next owner reading a
        // fully-cached layer that is never redrawn; one that resizes a run in
        // place runs over its neighbour. None of those raise an error, and none
        // of them can be reached from a headless test through VirtualShadowMap
        // itself, whose every entry point requires a live GL context.
        //
        // Driven by VirtualShadowMapLocalTest.
        struct LocalLayerPool
        {
            struct Slot
            {
                u64 LightId = 0;
                u32 Base = 0;  // first layer
                u32 Count = 0; // 6 for a point light, 1 for a spot; 0 = free entry
                u64 LastUsedFrame = 0;
                // The pose the layers' projections were built from. A change to
                // any of it re-anchors the light's frusta, so every cached page
                // under this slot is stale — the local analogue of the
                // directional path's light-direction / render-origin check.
                glm::vec3 Position{ 0.0f };
                glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
                f32 Range = 0.0f;
                f32 Cutoff = 0.0f;
                bool IsPoint = true;
                bool InUse = false;
            };

            // Slots are never ERASED — Owner and ByLight both store indices into
            // this vector, and erasing would silently repoint every index past
            // the hole. Count == 0 is the free marker.
            std::vector<Slot> Slots;
            std::unordered_map<u64, u32> ByLight;               // light id -> index into Slots
            std::array<u16, VSM::kMaxLocalLayers> Owner{};      // layer -> slot index + 1 (0 = free)
            std::array<u32, VSM::kMaxLocalLayers> Invalidate{}; // 1 = flush this layer's pages this frame
            u64 FrameCounter = 0;

            void BeginFrame();
            // Returns a slot index, or kNoLocalSlot when the pool is full of
            // slots that are all in use this frame. `outMoved` reports whether
            // the projections must be rebuilt — true for a fresh slot and for a
            // pose change, false for a light that stood still (which is what
            // keeps its pages cached).
            [[nodiscard]] u32 Acquire(const LocalLightDesc& desc, bool& outMoved);
            void Release(u32 slotIndex);
            void InvalidateSlot(u32 slotIndex);
            // Highest layer + 1 over every ALLOCATED slot, in use or not: an idle
            // slot still owns allocated pages until the LRU takes them, and a
            // shorter bound would leave them out of the HPB — which makes them
            // look permanently clean and never redrawn if the light comes back.
            [[nodiscard]] u32 LayerHighWater() const;
            void Clear();

          private:
            // Takes a contiguous run of `count` layers for a light with no slot
            // yet, evicting one least-recently-used idle slot if the pool is
            // full. Private because Acquire is the only correct entry point:
            // calling this for a light that already HAS a slot would strand its
            // old run.
            [[nodiscard]] u32 AcquireFreshSlot(u64 lightId, u32 count);
        };

        void BeginLocalLights();
        // Layer base, or -1 when the pool is exhausted (counted, and warned once).
        i32 RegisterLocalLight(const LocalLightDesc& desc);
        // Builds the per-layer projections and uploads the layer buffer. MUST be
        // called every frame local lights are active, including the frame the
        // count drops to zero — it is what retires the layers of lights that
        // stopped casting, and a skipped call leaves their pages resident and
        // their stale shadows on screen.
        void EndLocalLights();

        [[nodiscard]] bool AreLocalLightsActive() const
        {
            return IsActive() && m_Settings.LocalLights;
        }
        // How many LIGHTS got layers this frame (a point light counts once).
        //
        // Gated on AreLocalLightsActive(), because the head list is only rebuilt
        // while the system is on: with it off the scene never calls
        // BeginLocalLights, so an ungated read reports whatever the last active
        // frame left behind. That is not a cosmetic difference — this is the
        // third source ShadowMap::AnyShadowsRequested() consults, and a stale
        // non-zero there would hold the whole shadow pass open for lights that
        // are no longer registered.
        [[nodiscard]] u32 GetLocalLightCount() const
        {
            return AreLocalLightsActive() ? static_cast<u32>(m_LocalHeads.size()) : 0u;
        }
        // Layers in use — 6 per point light, 1 per spot. The number the cull and
        // the HPB build dispatch over.
        [[nodiscard]] u32 GetLocalLayerCount() const
        {
            return m_LocalLayerHighWater;
        }
        // Lights this frame that asked for layers and got none. Acceptance
        // criterion #1 is that this stays zero in a scene that starved the atlas.
        [[nodiscard]] u32 GetLocalLightsStarved() const
        {
            return m_LocalLightsStarved;
        }

        // --- Steps 1-6: page management (GPU) ---------------------------------
        void UpdatePages();

        // Softness and max shadow distance come from ShadowSettings, which VSM
        // does not own. Call after BeginFrame(), before UpdatePages().
        void SetSamplingParams(f32 softness, f32 maxShadowDistance);

        // --- Step 7: cull + raster -------------------------------------------
        //
        // Renders the submitted mesh casters into the pages marked dirty this
        // frame. Returns false when there was nothing to do.
        //
        // `uploadBones` is invoked once per skinned caster, immediately before its
        // draw, to publish that caster's bone palette into the supplied item-owned
        // upload buffer. The callback resolves the frame's immutable bone data.
        using BoneUploader = std::function<void(const ShadowSkinnedCaster&, UniformBuffer&)>;
        bool RenderCasters(const std::vector<ShadowMeshCaster>& meshCasters,
                           const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                           const glm::vec3& renderOrigin,
                           const BoneUploader& uploadBones);

        // --- Step 8: end of the shadow pass ----------------------------------
        void EndFrame();

        // --- Step 9: page marking, from the late graph node -------------------
        //
        // `sceneDepth` is this frame's depth texture, `inverseViewProjection` the
        // matrix that takes its NDC back to render-relative world space.
        void MarkRequiredPages(RHI::ResourceHandle sceneDepth, u32 depthWidth, u32 depthHeight,
                               const glm::mat4& inverseViewProjection, const glm::vec3& cameraPositionRelative);
        RGPreparedPass PreparePageMarking(RHI::ResourceHandle sceneDepth, u32 depthWidth, u32 depthHeight,
                                          const glm::mat4& inverseViewProjection, const glm::vec3& cameraPositionRelative);

        // Marks every allocated page a moving caster's swept bounds touch, so a
        // cached page containing a dynamic object is redrawn. Submitted during the
        // shadow pass, consumed by UpdatePages() in the same frame.
        void AddDynamicInvalidation(const glm::vec3& boundsMin, const glm::vec3& boundsMax);

        // Finds this frame's movers and submits their swept bounds. MUST be called
        // BEFORE UpdatePages(), which is what consumes the invalidations — running
        // it after would allocate and clear the pages first and leave the mover's
        // old silhouette baked into a page that is now marked clean.
        //
        // Movement is detected by comparing each caster's transform against the
        // same index in the previous frame's list, because a shadow caster carries
        // no "I moved" flag and the ECS is not this class's business. The list
        // order is stable across frames in practice (Scene traverses the registry
        // in registration order), and the failure mode when it is not is a
        // conservative one: a reordered list reads as "everything moved", which
        // costs a redraw rather than leaving a stale shadow behind. Skinned casters
        // are always treated as moving — they are animating by definition.
        void SubmitDynamicInvalidations(const std::vector<ShadowMeshCaster>& meshCasters,
                                        const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                        const glm::vec3& renderOrigin);

        // --- Binding + diagnostics -------------------------------------------

        // Publishes the globals UBO and binds the page table / physical pool for a
        // consumer (the lit pass, DDGI relight, the fog scatter). Safe to call when
        // inactive: it uploads a disabled globals block and binds the 1x1
        // placeholder pool so a shader that samples unconditionally reads "lit".
        void BindForSampling();

        [[nodiscard]] RHI::ResourceHandle GetPhysicalPoolHandle() const
        {
            return m_PhysicalPool;
        }
        [[nodiscard]] u32 GetPhysicalResolution() const
        {
            return m_PhysicalResolution;
        }
        // Bytes of GPU memory this system owns. Reported rather than estimated:
        // the acceptance criterion is a VRAM comparison against CSM.
        [[nodiscard]] u64 GetVRAMBytes() const;

        // One frame stale — the counters are read back from the buffer the
        // PREVIOUS frame wrote, so reading them never stalls the GPU.
        [[nodiscard]] const VSM::Statistics& GetStatistics() const
        {
            return m_Statistics;
        }
        // False until the first previous-frame counter block has actually
        // returned. Zero counters after that point are a valid idle sample.
        [[nodiscard]] bool HasStatistics() const
        {
            return m_HasStatistics;
        }

        // Render-graph resource name of the physical pool, so
        // olo_render_list_targets / olo_render_capture_target can reach it.
        static constexpr const char* kPhysicalPoolTargetName = "VSMPhysicalPages";

        // --- The core maths, as pure functions --------------------------------
        //
        // Extracted from BeginFrame so it can be tested without a GL context, and
        // it is worth testing: all three of the system's invariants live here and
        // every one of them fails silently on screen. See VirtualShadowMapTest.

        // Builds this frame's clip projections. `prevOrigins` are the previous
        // frame's frustum origins in ABSOLUTE page units — needed (rather than
        // just the wrapped offsets) because those are taken modulo the table
        // resolution, so a move of exactly one whole table is indistinguishable
        // from no move. `fullInvalidate` treats the frame as the first one.
        static void BuildClipProjections(const glm::vec3& lightDirection,
                                         const glm::vec3& cameraPositionRelative,
                                         const VirtualShadowMapSettings& settings,
                                         const std::array<glm::ivec2, VSM::kClipLevels>& prevOrigins,
                                         bool fullInvalidate,
                                         std::array<VSM::ClipProjection, VSM::kClipLevels>& outClips,
                                         std::array<glm::ivec2, VSM::kClipLevels>& outOrigins);

        // The clip-level heuristic. THE producer and THE consumer must agree on
        // it, so both go through this one function (the GLSL twin is
        // vsmClipLevelForDistance in include/VirtualShadowCommon.glsl).
        [[nodiscard]] static i32 SelectClipLevel(f32 distanceToCamera, f32 clip0HalfExtent, f32 bias);

        // Projects a render-relative world point into a clip level and returns the
        // wrapped page-table slot that owns it. False when the point is outside
        // that level's frustum.
        [[nodiscard]] static bool WorldPointToWrappedPage(const VSM::ClipProjection& clip,
                                                          const glm::vec3& worldPosRelative,
                                                          glm::ivec2& outWrappedPage);

        // Clamps a requested physical resolution to a whole number of pages inside
        // the range the page-entry encoding can address.
        [[nodiscard]] static u32 SanitizeResolution(u32 requested);

        // The local-light mip heuristic. Same contract as SelectClipLevel: THE
        // producer (VSM_MarkRequiredPages) and THE consumer (the lit pass) must
        // agree, so both go through the one GLSL twin
        // (vsmLocalMipForDistances in include/VirtualShadowCommon.glsl) and this
        // is its C++ mirror. A one-mip disagreement makes the sample land on an
        // unbacked page and the surface read fully LIT.
        [[nodiscard]] static i32 SelectLocalMip(f32 distanceToCamera, f32 distanceToLight, f32 bias);

        // Dominant-axis cube face in the layer order a point light's six layers
        // are built in: +X,-X,+Y,-Y,+Z,-Z. Mirrors vsmCubeFace / atlasCubeFace.
        [[nodiscard]] static u32 SelectCubeFace(const glm::vec3& direction);

        // Flat page-table index of a local page. The C++ twin of
        // vsmLocalPageIndex — the tests drive this, and a drift between the two
        // silently addresses another layer's pages.
        [[nodiscard]] static u32 LocalPageIndex(u32 layer, u32 mip, glm::uvec2 page);
        // Offset of a mip inside one layer's 1365-entry pyramid.
        [[nodiscard]] static u32 LocalMipOffset(u32 mip);

        // The six face view-projections of a point light, and the single one of a
        // spot, both in RENDER-RELATIVE space. Shared with the atlas path's
        // builders on purpose (light-path-photometric-parity.md): the two shadow
        // techniques must place a light's frustum identically, or switching
        // between them moves the shadow.
        static void BuildLocalLightProjections(const LocalLightDesc& desc,
                                               std::array<glm::mat4, 6>& outViewProjections,
                                               f32& outNear, f32& outFar);

      private:
        struct RasterItemResources
        {
            Ref<UniformBuffer> Pass;
            Ref<UniformBuffer> Animation;
            Ref<StorageBuffer> Instances;
        };
        struct PreparedSkinnedDraw
        {
            u32 CasterIndex;
            u32 InstanceOffset;
            u32 InstanceCount;
        };
        void PrepareRasterItems(u32 count);
        u32 RenderBatches(bool local);
        u32 RecordSkinnedDraws(const std::vector<ShadowSkinnedCaster>& casters,
                               const BoneUploader& uploadBones);

        // One (VAO, index range, cull mode) group of static casters — the same key
        // ShadowRenderPass's CSM path batches on, so a scene batches identically
        // whichever directional technique is active.
        struct Batch
        {
            RHI::ResourceHandle Vao{};
            u32 IndexCount = 0;
            u32 BaseIndex = 0;
            bool TwoSided = false;
            u32 CasterCount = 0;
            u32 RunBase = 0; // start of this batch's compacted instance run
            // The same, for the LOCAL cull's disjoint run (issue #703). Sized
            // casterCount * kLocalLayersPerCaster, so unlike the directional run
            // it is a CAP rather than an exact fit — a caster can genuinely reach
            // more layers than that, and the surplus is counted as an overflow.
            u32 LocalRunBase = 0;
            u32 LocalRunCapacity = 0;
        };

        // The batch identity, as a hashable key. RenderCasters used to find a
        // caster's batch with a linear scan over m_Batches -- run TWICE per
        // caster per frame, which at the documented budgets (kMaxCasters x
        // kMaxBatches) is millions of four-field compares on the render thread.
        // The map also removes the duplicated predicate the two scans carried.
        struct BatchKey
        {
            u64 Vao = 0; // ResourceHandle packed as (Generation << 32) | Index
            u32 IndexCount = 0;
            u32 BaseIndex = 0;
            bool TwoSided = false;

            bool operator==(const BatchKey&) const = default;
        };
        struct BatchKeyHash
        {
            [[nodiscard]] sizet operator()(const BatchKey& key) const
            {
                // FNV-1a over the four fields -- cheap, and a collision only
                // costs an equality re-check.
                u64 hash = 1469598103934665603ull;
                const auto mix = [&hash](u64 value)
                {
                    hash ^= value;
                    hash *= 1099511628211ull;
                };
                mix(key.Vao);
                mix(key.IndexCount);
                mix(key.BaseIndex);
                mix(key.TwoSided ? 1u : 0u);
                return static_cast<sizet>(hash);
            }
        };

        void CreateResources();
        void DestroyResources();
        bool LoadShaders();
        void ResetPageState();
        void ReadbackStatistics();
        void BindWorkingSet();
        u32 RenderSkinnedCasters(const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                 const glm::vec3& renderOrigin, u32 instanceBase,
                                 const BoneUploader& uploadBones);

        // Binds the physical pool on image unit 0. MUST be called with the
        // consuming VSM shader already bound — see the definition.
        void BindPhysicalPoolImage() const;

        // Step 7b — the local-light half of the cull + raster. Split out rather
        // than folded into RenderCasters so the two rasters' GL state changes stay
        // readable: they share a framebuffer and a pool image but not a viewport,
        // not a shader and not a projection source.
        u32 RenderLocalCasters(const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                               const glm::vec3& renderOrigin, u32 instanceBase,
                               const BoneUploader& uploadBones);
        // Uploads the layer header + projections. Also what resets the GPU-written
        // per-layer raster mip, so it must run before UpdatePages' HPB build.
        void UploadLocalLights();

        // Dispatch helper: binds `shader`, dispatches ceil(count / groupSize)
        // groups on X, and issues the SSBO/image barrier the next stage needs.
        // (Deliberately does NOT touch the pool image — the callers that need it
        // bind it themselves, in shader-then-image order.)
        void DispatchKernel(const Ref<ComputeShader>& shader, u32 threadCount, u32 groupSize) const;

        VirtualShadowMapSettings m_Settings{};
        bool m_Initialized = false;
        bool m_Suppressed = false;

        // Rounded, validated copy of Settings::PhysicalResolution.
        u32 m_PhysicalResolution = 0;
        u32 m_PhysicalPageTableResolution = 0;

        // R32UI physical page pool. R32UI rather than a depth format because the
        // raster resolves visibility with imageAtomicMin — the page indirection
        // happens in the fragment shader, so there is no fixed-function depth test
        // to lean on. Non-negative IEEE floats order as their bit patterns, so the
        // stored bits are directly comparable.
        RHI::ResourceHandle m_PhysicalPool{};

        // The raster's render SCOPE, and nothing else: GL needs a complete
        // framebuffer to define a render area, that area must be the VIRTUAL
        // resolution, and the physical pool cannot serve because it is a different
        // (configurable) size — a smaller attachment would clip three quarters of
        // every clip level away with no diagnostic at all. R8UInt is the cheapest
        // renderable format; draw buffers are NONE and nothing is ever written
        // through it (the pass writes only through the image unit).
        RHI::ResourceHandle m_RasterScope{};
        RHI::ResourceHandle m_RasterFramebuffer{};

        // Page state (all std430 SSBOs — SSBO atomics are core GL 4.6, whereas the
        // reference's R64_UINT meta image would need int64 image atomics).
        Ref<StorageBuffer> m_PageTable;     // kTotalVirtualPages uints
        Ref<StorageBuffer> m_MetaTable;     // physical page -> owner
        Ref<StorageBuffer> m_HPB;           // kHPBTotalEntries uints
        Ref<StorageBuffer> m_Requests;      // counter + allocation requests
        Ref<StorageBuffer> m_FreePages;     // counter + free physical page list
        Ref<StorageBuffer> m_Invalidations; // counter + dynamic caster AABBs
        Ref<StorageBuffer> m_CullInstances; // cull input
        Ref<StorageBuffer> m_DrawInstances; // cull output
        Ref<StorageBuffer> m_DrawCommands;  // indirect commands, one per batch
        // glMultiDrawElementsIndirectCount takes its DRAW count from a buffer.
        // Every batch issues exactly one command, so this holds a constant 1 — the
        // number that must not round-trip the CPU is the INSTANCE count inside the
        // command, and that one the cull writes.
        Ref<StorageBuffer> m_DrawCountBuffer;
        std::array<Ref<StorageBuffer>, 2> m_StatsBuffers; // ping-ponged, read one frame late
        // ...and read through a staging copy, never off the SSBO itself: the
        // kernels atomicAdd into these every frame, so a CPU read would migrate
        // them VIDEO -> HOST and make every one of those atomics slower for the
        // rest of the session. The ping-pong removes the STALL; this removes the
        // migration. Staged in EndFrame() and read in UpdatePages() — staging at
        // the READ site instead would have cost a second stale frame. See
        // StagedBufferReadback.
        StagedBufferReadback m_StatsReadback;

        Ref<UniformBuffer> m_GlobalsUBO;
        Ref<UniformBuffer> m_MarkGlobalsUBO;
        Ref<UniformBuffer> m_PassUBO;

        Ref<ComputeShader> m_FreeWrappedShader;
        Ref<ComputeShader> m_MarkShader;
        Ref<ComputeShader> m_InvalidateShader;
        Ref<ComputeShader> m_FindFreeShader;
        Ref<ComputeShader> m_AllocateShader;
        Ref<ComputeShader> m_ClearPagesShader;
        Ref<ComputeShader> m_BuildHPBShader;
        Ref<ComputeShader> m_CullShader;
        Ref<ComputeShader> m_CullLocalShader;
        Ref<ComputeShader> m_EndFrameShader;
        Ref<Shader> m_DepthShader;
        Ref<Shader> m_DepthSkinnedShader;
        Ref<Shader> m_DepthLocalShader;
        Ref<Shader> m_DepthLocalSkinnedShader;

        // The local-light layer buffer (SSBO 78): a CPU-written header of
        // per-layer raster mips / light heads / invalidate flags, followed by the
        // per-layer projections.
        Ref<StorageBuffer> m_LocalLights;

        VSM::GlobalsUBO m_Globals{};
        // Previous frame's per-level page offsets, in ABSOLUTE page units (not
        // wrapped) — FreeWrappedPages needs the true world page index on both
        // sides to decide whether a slot changed owner, and the wrapped offset
        // cannot answer that for a move of a whole table.
        std::array<glm::ivec2, VSM::kClipLevels> m_PrevOrigins{};
        std::array<glm::ivec2, VSM::kClipLevels> m_CurrOrigins{};
        glm::vec3 m_PrevLightDirection{ 0.0f };
        glm::vec3 m_PrevRenderOrigin{ 0.0f };
        bool m_FullInvalidate = true;

        std::vector<glm::vec4> m_PendingInvalidations; // pairs of (min,max), flushed by UpdatePages

        // Per-frame scratch, kept as members so the allocations survive across
        // frames (per-frame-scratch-reuse.md: these are cleared, never read
        // across a frame boundary, and touched only from the render thread).
        // Previous frame's caster poses, for the movement detection in
        // SubmitDynamicInvalidations. Bounds ride along so a mover can invalidate
        // the pages it LEFT as well as the ones it arrived on — invalidating only
        // the new position leaves the old silhouette in a cached page, which is
        // the shadow-that-stays-behind bug.
        struct CasterPose
        {
            glm::mat4 Transform{ 1.0f };
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            bool HasBounds = false;
        };
        std::vector<CasterPose> m_PrevCasterPoses;

        std::vector<Batch> m_Batches;
        std::unordered_map<BatchKey, u32, BatchKeyHash> m_BatchLookup; // key -> index into m_Batches
        std::vector<VSM::CullInstance> m_CullInput;
        std::vector<VSM::DrawCommand> m_DrawCommandStaging;
        std::vector<VSM::DrawInstance> m_SkinnedInstanceStaging;
        std::vector<PreparedSkinnedDraw> m_PreparedSkinnedDraws;
        std::vector<VSM::DrawInstance> m_PreparedSkinnedInstances;
        std::vector<RasterItemResources> m_RasterItems;

        // --- Local-light layer state (issue #703) -----------------------------
        LocalLayerPool m_LocalPool;
        std::array<VSM::LocalLight, VSM::kMaxLocalLayers> m_LocalLayers{};
        // (layerBase << 1) | isPoint, compacted — what the marker walks.
        std::vector<u32> m_LocalHeads;
        u32 m_LocalLayerHighWater = 0;
        u32 m_LocalLightsStarved = 0;
        bool m_LoggedLocalPoolExhausted = false;
        bool m_LoggedLocalDrawBudgetExhausted = false;
        // Base of the local raster's region of m_DrawInstances, and how much of
        // it is left. Computed in RenderCasters (the directional runs come
        // first) and consumed by RenderLocalCasters.
        u32 m_LocalInstanceBase = 0;
        u32 m_LocalInstanceCapacity = 0;
        std::vector<VSM::DrawInstance> m_LocalSkinnedStaging;

        u32 m_StatsWriteIndex = 0;
        VSM::Statistics m_Statistics{};
        bool m_HasStatistics = false;
        bool m_LoggedRasterIncomplete = false;
        bool m_LoggedDrawBudgetExhausted = false;
        bool m_LoggedCasterBudgetExhausted = false;
    };
} // namespace OloEngine
