#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUCache/GPUCachePolicy.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedCache.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTextureTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class StorageBuffer;
    class TerrainMaterial;
    class Texture2D;
    class Texture2DArray;

    // @brief Adaptive-virtual-texture surfacing for terrain — slice 1 of issue
    // #715: a FIXED-GRID virtual image with an uncompressed physical cache and
    // a mip-chained indirection map.
    //
    // What this replaces: `Terrain_PBR.glsl` blends up to eight splat layers per
    // pixel with triplanar + height blending, so shading cost scales with layer
    // count and unique texel density is capped by the splatmap's resolution.
    // With VT on, the same blend runs ONCE PER CACHE TILE in a compute kernel and
    // the fragment stage does one indirection lookup plus one cache sample,
    // regardless of how many layers went in.
    //
    // ── The loop, and why it is shaped like this ─────────────────────────────
    //
    // Everything below happens inside `Update()`, which the terrain update pass
    // calls once per frame BEFORE the terrain draws are submitted:
    //
    //  1. **Capture.** Barrier, copy the feedback SSBO — which the PREVIOUS
    //     frame's terrain fragments wrote — into the next readback ring slot,
    //     fence it, then clear the SSBO for this frame's writes.
    //  2. **Poll.** Any ring slot whose fence has signalled is read back and
    //     handed to a `Tasks::Launch` job that reduces it to a unique page
    //     request list. **The fence check is what makes this not a stall**: the
    //     read only ever happens once the GPU has finished the copy, so it is a
    //     memcpy rather than an implicit `glFinish`. There is no
    //     `ClientWaitFence` anywhere in this class.
    //  3. **Service.** When an analysis job completes, its request list drives
    //     the page cache: touch what is resident, allocate (evicting via LRU)
    //     what is not, bounded by `MaxTileBakesPerFrame`.
    //  4. **Bake.** One compute dispatch composites every newly-mapped page's
    //     splat blend straight into its physical cache tile.
    //  5. **Publish.** When the resident set changed, the indirection texture is
    //     rebuilt: clear, stamp the resident pages at their own mip, then
    //     propagate coarse→fine so every unmapped texel inherits the nearest
    //     resident ancestor.
    //
    // Step 5's propagation is the anti-pop mechanism and the reason the
    // indirection map has a mip chain at all. A lookup that finds no page at the
    // mip it wanted reads a COARSER resident page instead of nothing, so a page
    // arriving mid-motion sharpens the surface rather than replacing a hole.
    // The coarsest (1x1) page is pinned by the analyzer, so that chain always
    // terminates in something resident.
    //
    // ── Threading ────────────────────────────────────────────────────────────
    //
    // Every method is render-thread-only. The single exception is the analysis
    // body, which runs on a Task worker over a plain `std::vector<u32>` copy and
    // touches no renderer state — see `VTFeedbackAnalyzer`.
    //
    // ── What is NOT here (slices 2-4 of #715) ────────────────────────────────
    //
    //  - Incremental indirection updates. Slice 1 rebuilds the whole map on any
    //    residency change, which is ~1.3 * VirtualPagesWide^2 texel writes and
    //    only on frames where something moved.
    //  - Adaptive / variable-size virtual images (the "A" in AVT). The grid is
    //    uniform over the terrain; per-region density would need the shared
    //    power-of-two atlas allocator tracked as #718.
    //  - BC-compressed cache tiles, which need the GPU compressor from item 3 of
    //    #624. The cache is uncompressed RGBA8 here, by design.
    class TerrainVirtualTexture : public RefCounted
    {
      public:
        // @brief Cache residency and request counters — acceptance criterion 3.
        struct Stats
        {
            u32 m_CacheTileCount = 0;        // physical tiles the cache holds
            u32 m_ResidentTiles = 0;         // tiles currently mapped to a page
            u32 m_PagesRequested = 0;        // unique pages the last analysis asked for
            u32 m_FeedbackTexelsWritten = 0; // feedback texels that carried a request
            u32 m_TilesBakedThisFrame = 0;
            u32 m_TilesBakedTotal = 0;
            u32 m_EvictionsTotal = 0;
            u32 m_BudgetStarvedRequests = 0; // requests deferred to a later frame
            // True when the camera wants more pages than the cache can hold. Not an
            // error — the coarse-mip fallback covers the misses — but it separates
            // "the cache is too small" from "the bake budget is too small", which look
            // identical from the viewport.
            bool m_WorkingSetExceedsCache = false;
            u32 m_ReadbackSlotsInFlight = 0;
            u64 m_CacheBytes = 0;
            u64 m_IndirectionBytes = 0;
        };

        // @brief What `Update()` needs from the terrain being surfaced.
        struct FrameInputs
        {
            u32 m_ViewportWidth = 0;
            u32 m_ViewportHeight = 0;
            // ABSOLUTE (not render-relative) terrain-local -> world transform.
            // Baked content is world-anchored because the triplanar projection
            // is, so a change here invalidates every resident tile.
            glm::mat4 m_Model{ 1.0f };
            const TerrainMaterial* m_Material = nullptr;
            RHI::ResourceHandle m_Heightmap{};
            u32 m_HeightmapResolution = 0;
            f32 m_WorldSizeX = 0.0f;
            f32 m_WorldSizeZ = 0.0f;
            f32 m_HeightScale = 0.0f;
            f32 m_TriplanarSharpness = 8.0f;
        };

        TerrainVirtualTexture();
        ~TerrainVirtualTexture();
        TerrainVirtualTexture(const TerrainVirtualTexture&) = delete;
        TerrainVirtualTexture& operator=(const TerrainVirtualTexture&) = delete;

        // (Re)create every GPU resource for `config`. Sanitizes the config first
        // and warns once if it had to change anything. Returns false when a
        // resource could not be created — the caller then keeps the splat path.
        bool Configure(const TerrainVirtualTextureConfig& config);

        // Release every GPU resource. Safe to call without a device.
        void Destroy();

        // One frame of the loop described above. Returns true when the virtual
        // texture is safe to shade with THIS frame; false means the caller must
        // fall back to the splat path (nothing resident yet, a shader failed to
        // load, or no device).
        bool Update(const FrameInputs& inputs);

        // True once the coarsest page is resident, i.e. every lookup is
        // guaranteed to resolve to real content. The gate for the shader's VT
        // branch — turning VT on before this shows the cache's zeroed contents.
        [[nodiscard]] bool IsReadyForShading() const
        {
            return m_ShadingReady;
        }

        [[nodiscard]] RHI::ResourceHandle GetIndirectionHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetCacheHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetFeedbackBufferHandle() const;

        [[nodiscard]] const TerrainVirtualTextureConfig& GetConfig() const
        {
            return m_Config;
        }
        [[nodiscard]] const Stats& GetStats() const
        {
            return m_Stats;
        }

        // The three vec4s the terrain shaders read out of the terrain UBO. Kept
        // here rather than at the call site so the packing has one owner; the
        // GLSL twin is oloVTUnpackParams() in include/TerrainVirtualTexture.glsl.
        void FillShaderParams(glm::vec4& outParams0, glm::vec4& outParams1, glm::vec4& outParams2) const;

        // Drop every resident page. Used when the baked content's inputs change
        // (material rebuild, sculpt, a transform that moves the world-anchored
        // triplanar projection).
        void Invalidate();

        // Directory-only page cache: this class owns the payload (the physical
        // cache texture) and uses #704's substrate purely for the page-index
        // allocation, the LRU order and the eviction notification.
        using PageCache = GPUPagedCache<u32, VTBakeRequest, LRUPolicy>;

      private:
        // How many frames of readback are in flight at once. Three is the
        // smallest number that lets a slot's fence be checked on a LATER frame
        // than the one that issued it without ever blocking: capture on N, poll
        // from N+1, and a third slot so a slow frame never has to reuse a slot
        // whose fence is still unsignalled.
        static constexpr u32 kReadbackSlots = 3;

        struct ReadbackSlot
        {
            RHI::ResourceHandle m_Buffer{};
            u64 m_Fence = 0;
            bool m_Pending = false;
        };

        struct PendingAnalysis
        {
            Tasks::TTask<bool> m_Task;
            std::shared_ptr<std::vector<u32>> m_Feedback;
            std::shared_ptr<VTFeedbackAnalyzer> m_Analyzer;
            // Which capture this reduces. Task completion order is NOT launch
            // order, so "the last one in the vector" is not "the newest" — see
            // RetireAnalysis.
            u64 m_Sequence = 0;
        };

        bool EnsureShaders();
        bool EnsureFeedbackResources(u32 viewportWidth, u32 viewportHeight);
        void DestroyReadbackSlots();

        void CaptureFeedback();
        void PollReadback();
        void RetireAnalysis();
        void ServiceRequests();
        void BakeTiles(const FrameInputs& inputs);
        void PublishIndirection();

        [[nodiscard]] glm::uvec2 TileCoord(u32 tileIndex) const;

        TerrainVirtualTextureConfig m_Config{};
        bool m_Created = false;
        bool m_ShadingReady = false;

        Ref<Texture2D> m_IndirectionTexture;
        Ref<Texture2DArray> m_CacheTexture;

        Ref<StorageBuffer> m_FeedbackBuffer;
        Ref<StorageBuffer> m_BakeBuffer;
        Ref<StorageBuffer> m_IndirectionUpdateBuffer;

        Ref<ComputeShader> m_TileBakeShader;
        Ref<ComputeShader> m_IndirectionClearShader;
        Ref<ComputeShader> m_IndirectionWriteShader;
        Ref<ComputeShader> m_IndirectionFillShader;
        bool m_ShadersLoaded = false;
        bool m_ShaderLoadFailed = false;

        glm::uvec2 m_FeedbackDims{ 0u };
        u32 m_FeedbackWords = 0;
        std::array<ReadbackSlot, kReadbackSlots> m_ReadbackSlots{};
        u32 m_NextReadbackSlot = 0;

        std::vector<PendingAnalysis> m_PendingAnalyses;
        // The most recent completed analysis, consumed by ServiceRequests().
        std::vector<VTPageRequest> m_Requests;
        bool m_HasFreshRequests = false;
        u64 m_NextAnalysisSequence = 1;
        u64 m_AdoptedAnalysisSequence = 0;

        PageCache m_PageCache;
        std::unordered_map<u32, u32> m_Resident; // page key -> physical tile index
        std::vector<u32> m_EvictedThisFrame;     // filled by the eviction listener

        std::vector<VTBakeRequest> m_BakeList;
        std::vector<VTIndirectionUpdate> m_UpdateScratch;
        std::vector<u8> m_UploadScratch;

        bool m_IndirectionDirty = false;
        glm::mat4 m_BakedModel{ 1.0f };
        bool m_HasBakedModel = false;
        // Which screen-hash slot writes feedback this frame; cycles over
        // FeedbackDownscale^2 so every screen pixel eventually contributes.
        u32 m_FeedbackFrame = 0;
        bool m_AllocationWarned = false;
        bool m_ConfigWarned = false;

        Stats m_Stats{};
    };
} // namespace OloEngine
