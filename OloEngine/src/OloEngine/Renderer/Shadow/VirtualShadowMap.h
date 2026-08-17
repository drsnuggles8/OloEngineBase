#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
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
    //     2. InvalidatePages    — dynamic casters re-dirty the pages they touch
    //     3. FindFreePages      — build the free list (unallocated, then LRU)
    //     4. AllocatePages      — back each request, mark it dirty
    //     5. ClearDirtyPages    — reset the physical texels of every dirty page
    //     6. BuildHPB           — 2x2 MAX reduction of the DIRTY flag, 7 mips
    //     7. CullCasters + raster — indirect draws into the dirty pages only
    //     8. EndFrame           — clear DIRTY / VISITED / REQUESTS for the marker
    //
    //   [mark pass, end of frame N]
    //     9. MarkRequiredPages  — project every depth texel into its clip level,
    //                             request unallocated pages, VISIT allocated ones
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

        inline constexpr u32 kHPBMipCount = 7;
        inline constexpr u32 kHPBEntriesPerLevel = 5461; // sum of (64 >> m)² for m in [0,7)
        inline constexpr u32 kHPBTotalEntries = kHPBEntriesPerLevel * kClipLevels;

        // Page-state bits — the C++ half of the encoding documented in the GLSL.
        inline constexpr u32 kPageAllocatedBit = 1u << 31;
        inline constexpr u32 kPageRequestsAllocBit = 1u << 30;
        inline constexpr u32 kPageDirtyBit = 1u << 29;
        inline constexpr u32 kPageVisitedBit = 1u << 28;
        inline constexpr u32 kPageAllocFailedBit = 1u << 27;

        inline constexpr u32 kMetaAllocatedBit = 1u << 31;
        inline constexpr u32 kMetaVisitedBit = 1u << 30;

        // Allocation-request ring capacity. Mirrors VSM_MAX_REQUESTS.
        inline constexpr u32 kMaxRequests = 16384;

        // Per-frame CPU-side budgets. Each is a hard cap that degrades by dropping
        // work and saying so, never by writing past the end of a buffer:
        //   kMaxCasters       — shadow-casting mesh instances submitted per frame
        //   kMaxBatches       — distinct (VAO, index range, cull mode) groups
        //   kMaxDrawInstances — (caster x clip level) pairs the cull may emit,
        //                       plus the CPU-scheduled skinned records at the tail
        inline constexpr u32 kMaxCasters = 8192;
        inline constexpr u32 kMaxBatches = 1024;
        inline constexpr u32 kMaxDrawInstances = 131072;

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

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(GlobalsUBO));
            }
        };
        static_assert(sizeof(GlobalsUBO) % 16 == 0, "VSM::GlobalsUBO must be 16-byte aligned for std140");
        static_assert(sizeof(GlobalsUBO) == 2720, "VSM::GlobalsUBO std140 size drifted from GLSL expectation (2720 B)");

        // UBO_VIRTUAL_SHADOW_DRAW — per-dispatch / per-draw scratch, refilled
        // immediately before each use (the #691 Phase 7 pattern). Two disjoint
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
        };
        static_assert(sizeof(CullInstance) == 112, "VSM::CullInstance std430 size drifted (112 B)");

        // Cull output: one entry per surviving (caster, clip level) pair (std430).
        struct DrawInstance
        {
            glm::mat4 Transform{ 1.0f };
            u32 ClipLevel = 0;
            u32 _pad0 = 0;
            u32 _pad1 = 0;
            u32 _pad2 = 0;
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

        // 0 = off, 1 = clip level tint, 2 = page address, 3 = pages drawn this
        // frame. Consumed by the lit pass through GlobalsUBO::Params2.y.
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
            return m_Settings.Enabled && m_Initialized;
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
                        const glm::vec3& renderOrigin);

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
        // draw, to publish that caster's bone palette. It is a callback rather
        // than something this class does itself because the animation UBO belongs
        // to ShadowMap — inlining it here would give VSM a second owner of shadow
        // pass state.
        using BoneUploader = std::function<void(const ShadowSkinnedCaster&)>;
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

      private:
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

        // Dispatch helper: binds `shader`, dispatches ceil(count / groupSize)
        // groups on X, and issues the SSBO/image barrier the next stage needs.
        // (Deliberately does NOT touch the pool image — the callers that need it
        // bind it themselves, in shader-then-image order.)
        void DispatchKernel(const Ref<ComputeShader>& shader, u32 threadCount, u32 groupSize) const;

        VirtualShadowMapSettings m_Settings{};
        bool m_Initialized = false;

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

        Ref<UniformBuffer> m_GlobalsUBO;
        Ref<UniformBuffer> m_PassUBO;

        Ref<ComputeShader> m_FreeWrappedShader;
        Ref<ComputeShader> m_MarkShader;
        Ref<ComputeShader> m_InvalidateShader;
        Ref<ComputeShader> m_FindFreeShader;
        Ref<ComputeShader> m_AllocateShader;
        Ref<ComputeShader> m_ClearPagesShader;
        Ref<ComputeShader> m_BuildHPBShader;
        Ref<ComputeShader> m_CullShader;
        Ref<ComputeShader> m_EndFrameShader;
        Ref<Shader> m_DepthShader;
        Ref<Shader> m_DepthSkinnedShader;

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

        u32 m_StatsWriteIndex = 0;
        VSM::Statistics m_Statistics{};
        bool m_LoggedRasterIncomplete = false;
        bool m_LoggedDrawBudgetExhausted = false;
        bool m_LoggedCasterBudgetExhausted = false;
    };
} // namespace OloEngine
