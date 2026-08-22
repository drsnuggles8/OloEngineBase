#include "OloEnginePCH.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

namespace OloEngine
{
    namespace
    {
        using SBL = ShaderBindingLayout;

        // Must match the local_size of the four kernels.
        constexpr u32 kTileBakeGroupSize = 8;
        constexpr u32 kIndirectionClearGroupSize = 8;
        constexpr u32 kIndirectionWriteGroupSize = 64;
        constexpr u32 kIndirectionFillGroupSize = 8;

        // Image units the kernels declare. Both stay well under
        // ShaderBindingLayout::MAX_ENGINE_IMAGE_SLOTS.
        constexpr u32 kImageUnitTarget = 0;
        constexpr u32 kImageUnitCoarser = 1;

        // std430 header of the bake buffer — the C++ twin of everything before
        // `VTBakeRequest b_VTBakeRequests[]` in compute/TerrainVTTileBake.comp.
        // The parameters ride the payload buffer rather than a UBO on purpose:
        // the UBO namespace has one free slot left engine-wide and this feature
        // does not get to spend it (see SSBO_TERRAIN_VT_BAKE's note).
        struct VTBakeHeader
        {
            glm::uvec4 m_Config0{ 0u }; // pagesWide, pageTexels, borderTexels, requestCount
            glm::uvec4 m_Config1{ 0u }; // layerCount, tileTexels, layerResolution, unused
            glm::vec4 m_World{ 0.0f };  // worldSizeX, worldSizeZ, heightScale, triplanarSharpness
            glm::vec4 m_Texel{ 0.0f };  // heightmap texel size
            glm::mat4 m_Model{ 1.0f };
            glm::vec4 m_Tiling0{ 0.0f };
            glm::vec4 m_Tiling1{ 0.0f };
            glm::vec4 m_Sharp0{ 0.0f };
            glm::vec4 m_Sharp1{ 0.0f };
        };
        static_assert(sizeof(VTBakeHeader) == 192, "VTBakeHeader must match the std430 header of TerrainVTBake");

        // std430 header of the indirection update buffer. Declared IDENTICALLY by
        // both kernels that read the buffer (compute/TerrainVTIndirectionWrite
        // and ...Fill) — they are separate programs reading one buffer, so a
        // member added to one declaration and not the other is a silent
        // misalignment of `b_VTUpdates`, not a link error.
        struct VTIndirectionHeader
        {
            glm::uvec4 m_WriteParams{ 0u }; // baseIndex, count, unused, unused
            glm::uvec4 m_FillParams{ 0u };  // originX, originY, sizeX, sizeY (texels of the bound mip)
        };
        static_assert(sizeof(VTIndirectionHeader) == 32, "VTIndirectionHeader must match its std430 twin");

        // Byte offsets of the two header vectors, for the partial uploads in
        // PublishIndirection. Spelled out rather than taken with offsetof: a
        // glm::uvec4 is a class type, so offsetof on this struct is only
        // conditionally supported (and SonarCloud rejects it outright). The
        // std430 layout is the contract either way, and the assert below pins
        // these two constants to the struct rather than leaving them a
        // hand-maintained second copy of it.
        constexpr u32 kWriteParamsOffset = 0u;
        constexpr u32 kFillParamsOffset = static_cast<u32>(sizeof(glm::uvec4));
        static_assert(kFillParamsOffset + sizeof(glm::uvec4) == sizeof(VTIndirectionHeader),
                      "the header's two uvec4s must exactly tile it — a member added to either end "
                      "moves b_VTUpdates and both GLSL declarations with it");

        [[nodiscard]] u32 DivRoundUp(u32 value, u32 divisor)
        {
            return (value + divisor - 1u) / divisor;
        }

        // Explicit epsilon compare rather than `!=` — glm's matrix operator!=
        // is an exact float comparison, which this repo forbids (see
        // docs/agent-rules/cpp-coding-quality.md §2). The tolerance is
        // deliberately loose: the question is "did the terrain MOVE", and a
        // last-bit difference is not a move, but re-baking the whole cache for
        // one would be a per-frame stutter.
        [[nodiscard]] bool MatricesDiffer(const glm::mat4& a, const glm::mat4& b)
        {
            constexpr f32 kEpsilon = 1e-5f;
            for (i32 column = 0; column < 4; ++column)
            {
                if (!glm::all(glm::epsilonEqual(a[column], b[column], kEpsilon)))
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    TerrainVirtualTexture::TerrainVirtualTexture() = default;

    TerrainVirtualTexture::~TerrainVirtualTexture()
    {
        Destroy();
    }

    bool TerrainVirtualTexture::Configure(const TerrainVirtualTextureConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        TerrainVirtualTextureConfig sanitized = config;
        if (!sanitized.Sanitize() && !m_ConfigWarned)
        {
            m_ConfigWarned = true;
            OLO_CORE_WARN("TerrainVirtualTexture: requested configuration is not expressible "
                          "(pages={}, pageTexels={}, border={}, tiles={}) — using {}/{}/{}/{} instead",
                          config.VirtualPagesWide, config.PageTexels, config.BorderTexels, config.CacheTilesWide,
                          sanitized.VirtualPagesWide, sanitized.PageTexels, sanitized.BorderTexels,
                          sanitized.CacheTilesWide);
        }

        if (m_Created && m_Config == sanitized)
        {
            return true;
        }

        if (!sanitized.IsValid())
        {
            OLO_CORE_ERROR("TerrainVirtualTexture: configuration rejected after sanitizing — VT disabled");
            return false;
        }
        // Checked BEFORE Destroy(): a headless caller reaches this every frame
        // (the scene tick has no idea there is no device), and tearing down and
        // re-testing an empty object each time would be pointless churn.
        // Headless is not an error — the caller keeps the splat path, and the
        // CPU-side types stay unit-testable without a context.
        if (!RenderCommand::IsDeviceAvailable())
        {
            return false;
        }

        Destroy();
        m_Config = sanitized;

        TextureSpecification indirectionSpec;
        indirectionSpec.Width = m_Config.VirtualPagesWide;
        indirectionSpec.Height = m_Config.VirtualPagesWide;
        indirectionSpec.Format = ImageFormat::RGBA8;
        indirectionSpec.GenerateMips = true;
        indirectionSpec.MipLevels = m_Config.MipCount();
        indirectionSpec.SRGB = false;
        m_IndirectionTexture = Texture2D::Create(indirectionSpec);

        const bool compressed = m_Config.CompressedCache;

        Texture2DArraySpecification cacheSpec;
        cacheSpec.Width = m_Config.CacheTexels();
        cacheSpec.Height = m_Config.CacheTexels();
        cacheSpec.Layers = 2; // 0 = albedo + AO, 1 = normal.xy + roughness + metallic
        // BC7 (slice 4): 1 byte per texel per layer instead of 4. imageStore
        // cannot write a block-compressed image, so the bake lands in the
        // scratch below, the compress kernel packs blocks into the staging,
        // and a block-compatible copy places each tile here.
        cacheSpec.Format = compressed ? Texture2DArrayFormat::BC7 : Texture2DArrayFormat::RGBA8;
        // No mips, and that is the VT contract rather than an omission: a mip
        // chain over an ATLAS blurs neighbouring tiles into each other, which is
        // precisely what the per-tile border exists to avoid. Minification is
        // handled by picking a coarser PAGE, not a coarser texel.
        cacheSpec.GenerateMipmaps = false;
        m_CacheTexture = Texture2DArray::Create(cacheSpec);

        if (compressed)
        {
            // One scratch slot per bake-budget entry, side by side in x, so the
            // whole batch bakes in one dispatch and compresses in one more.
            Texture2DArraySpecification scratchSpec;
            scratchSpec.Width = m_Config.TileTexels() * m_Config.MaxTileBakesPerFrame;
            scratchSpec.Height = m_Config.TileTexels();
            scratchSpec.Layers = 2;
            scratchSpec.Format = Texture2DArrayFormat::RGBA8;
            scratchSpec.GenerateMipmaps = false;
            m_ScratchTexture = Texture2DArray::Create(scratchSpec);

            // One RGBA32UI texel per 4x4 BC7 block. Sanitize() keeps
            // TileTexels() a multiple of 4, so the division is exact and every
            // tile origin in the cache stays block-aligned.
            const u32 tileBlocks = m_Config.TileTexels() / 4u;
            Texture2DArraySpecification stagingSpec;
            stagingSpec.Width = tileBlocks * m_Config.MaxTileBakesPerFrame;
            stagingSpec.Height = tileBlocks;
            stagingSpec.Layers = 2;
            stagingSpec.Format = Texture2DArrayFormat::RGBA32UI;
            stagingSpec.GenerateMipmaps = false;
            m_StagingTexture = Texture2DArray::Create(stagingSpec);
        }

        const u32 bakeBytes = static_cast<u32>(sizeof(VTBakeHeader)) +
                              m_Config.MaxTileBakesPerFrame * static_cast<u32>(sizeof(VTBakeRequest));
        m_BakeBuffer = StorageBuffer::Create(bakeBytes, SBL::SSBO_TERRAIN_VT_BAKE, StorageBufferUsage::DynamicDraw);

        // TWICE the tile count, because the worst case is not the resident set.
        // A full rebuild uploads one entry per resident page, so tile count is
        // the ceiling there. A DELTA can hold an unmap for every page that was
        // resident plus a map for every page that replaced one — which is what
        // an Invalidate() followed by servicing in the same frame produces. A
        // delta past this ceiling escalates to a rebuild rather than being
        // uploaded in pieces (see PublishIndirection).
        m_IndirectionUpdateCapacity = 2u * m_Config.CacheTileCount();
        const u32 updateBytes = static_cast<u32>(sizeof(VTIndirectionHeader)) +
                                m_IndirectionUpdateCapacity * static_cast<u32>(sizeof(VTIndirectionUpdate));
        m_IndirectionUpdateBuffer =
            StorageBuffer::Create(updateBytes, SBL::SSBO_TERRAIN_VT_INDIRECTION, StorageBufferUsage::DynamicDraw);

        // Handle validity, not just Ref nullness: a backend can construct the
        // wrapper and refuse the storage (Vulkan refuses BC7 arrays outright;
        // GL leaves the object storageless past GL_MAX_TEXTURE_SIZE). A cache
        // with no image behind it would otherwise sail past this guard, CPU
        // residency bookkeeping would still converge, and the shader would
        // sample a never-populated texture instead of taking the splat path.
        const auto textureUsable = [](const Ref<Texture2DArray>& texture)
        { return texture && texture->GetRHIHandle().IsValid(); };
        if (!m_IndirectionTexture || !m_IndirectionTexture->GetRHIHandle().IsValid() ||
            !textureUsable(m_CacheTexture) || !m_BakeBuffer || !m_IndirectionUpdateBuffer ||
            (compressed && (!textureUsable(m_ScratchTexture) || !textureUsable(m_StagingTexture))))
        {
            OLO_CORE_ERROR("TerrainVirtualTexture: GPU resource allocation failed — VT disabled");
            Destroy();
            return false;
        }

        // Directory-only (page size 0): this class owns the payload, so the
        // substrate supplies the page-index allocator, the LRU order and the
        // eviction notification and nothing else. HostOnly because the terrain
        // shader resolves residency through the indirection TEXTURE, not by
        // probing the hash map — which also means the whole page-management half
        // of this class runs headlessly.
        if (!m_PageCache.Create(0, m_Config.CacheTileCount(), GPUCacheBacking::HostOnly))
        {
            OLO_CORE_ERROR("TerrainVirtualTexture: page cache creation failed for {} tiles",
                           m_Config.CacheTileCount());
            Destroy();
            return false;
        }
        m_PageCache.SetEvictionListener(
            [this](const u32& victim)
            {
                // Runs mid-allocation and must not re-enter the cache — dropping
                // our own record, recording the unmap and flagging the map is all
                // it does.
                //
                // **The unmap is the whole reason this listener is load-bearing
                // by the incremental deltas.** The whole-map rebuild could get away with only setting the
                // dirty flag, because the rebuild cleared everything first. With
                // no clear pass, a page that leaves the cache without an entry
                // here keeps its old physical address in the indirection map and
                // the terrain goes on sampling a tile some other page now owns.
                m_Resident.erase(victim);
                VTRecordEviction(m_IndirectionDelta, victim);
                m_IndirectionDirty = true;
                ++m_Stats.m_EvictionsTotal;
            });

        m_IndirectionDelta.Reset(m_Config);
        // Freshly created texture storage holds undefined bytes, not zeroes, so
        // the first publish has to be the clear-everything path — there is no
        // "unchanged" state to be incremental against yet.
        m_IndirectionNeedsFullRebuild = true;
        m_IndirectionDirty = true;
        EnsureTimingQueries();

        // The virtual atlas the per-sector images are allocated from, in PAGE
        // units — the allocator is unitless, and pages are what everything
        // downstream addresses. Granularity is the minimum image size, which
        // keeps the node table small AND keeps the level count under the
        // allocator's hard 12-level ceiling; Sanitize() enforces the ratio, but
        // the zero-capacity double-check stays because that failure mode is
        // otherwise SILENT (every Allocate returns kInvalidNode, no log).
        static_assert(UBOStructures::TerrainUBO::kTerrainVTMaxSectors == kVTMaxSectorCount,
                      "the sector table in the terrain UBO and the VT config cap must stay one number");
        m_AtlasAllocator = AtlasAllocator(m_Config.VirtualPagesWide, m_Config.EffectiveMinImagePagesWide());
        if (m_AtlasAllocator.AtlasSize() == 0u)
        {
            OLO_CORE_ERROR("TerrainVirtualTexture: atlas allocator rejected {} pages at {}-page granularity — "
                           "VT disabled",
                           m_Config.VirtualPagesWide, m_Config.EffectiveMinImagePagesWide());
            Destroy();
            return false;
        }
        m_Sectors.assign(m_Config.EffectiveSectorCount(), SectorImage{});
        for (u32 i = 0; i < m_Sectors.size(); ++i)
        {
            // Every sector starts at the minimum (for the fixed-grid config the
            // "minimum" IS the whole atlas); growth is earned through feedback.
            ResizeSectorImage(i, m_Config.EffectiveMinImagePagesWide());
            if (!m_Sectors[i].m_Snapshot.IsAllocated())
            {
                OLO_CORE_ERROR("TerrainVirtualTexture: initial image allocation failed for sector {} of {} — "
                               "VT disabled",
                               i, m_Sectors.size());
                Destroy();
                return false;
            }
        }

        m_Stats = Stats{};
        m_Stats.m_CacheTileCount = m_Config.CacheTileCount();
        m_Stats.m_SectorCount = static_cast<u32>(m_Sectors.size());
        m_Stats.m_CacheCompressed = compressed;
        const u64 cacheTexels = static_cast<u64>(m_Config.CacheTexels()) * m_Config.CacheTexels();
        // BC7 is one byte per texel; the scratch/staging pair the compressed
        // path adds is per-batch, not per-tile, and small enough to ignore.
        m_Stats.m_CacheBytes = cacheTexels * (compressed ? 1ull : 4ull) * cacheSpec.Layers;
        u64 indirectionTexels = 0;
        for (u32 mip = 0; mip < m_Config.MipCount(); ++mip)
        {
            const u64 side = m_Config.VirtualPagesWide >> mip;
            indirectionTexels += side * side;
        }
        m_Stats.m_IndirectionBytes = indirectionTexels * 4ull;

        m_Created = true;
        return true;
    }

    void TerrainVirtualTexture::Destroy()
    {
        // A TTask asserts if it is destroyed before completion, so the analysis
        // jobs have to be joined rather than dropped. They touch no renderer
        // state, so this waits on pure CPU work.
        for (auto& pending : m_PendingAnalyses)
        {
            (void)pending.m_Task.Wait();
        }
        m_PendingAnalyses.clear();

        DestroyReadbackSlots();
        DestroyTimingQueries();

        // Before the listener is dropped, not after: DeallocateObject does not
        // notify (only an LRU eviction does), so clearing the cache below would
        // not have produced the unmaps anyway — but a listener still installed
        // while the cache tears down would push entries into a delta that is
        // about to be reset, which reads as a leak when it is not.
        m_PageCache.SetEvictionListener(nullptr);
        m_PageCache.Destroy();
        m_Resident.clear();
        m_BakeList.clear();
        m_Requests.clear();
        m_IndirectionDelta.Reset(TerrainVirtualTextureConfig{});
        m_IndirectionUpdateCapacity = 0;
        m_IndirectionNeedsFullRebuild = false;
        m_UploadScratch.clear();

        m_IndirectionTexture = nullptr;
        m_CacheTexture = nullptr;
        m_ScratchTexture = nullptr;
        m_StagingTexture = nullptr;
        m_FeedbackBuffer = nullptr;
        m_BakeBuffer = nullptr;
        m_IndirectionUpdateBuffer = nullptr;

        m_TileBakeShader = nullptr;
        m_CompressShader = nullptr;
        m_IndirectionClearShader = nullptr;
        m_IndirectionWriteShader = nullptr;
        m_IndirectionFillShader = nullptr;
        m_ShadersLoaded = false;
        m_ShaderLoadFailed = false;

        // Reset as a PAIR: the sector table's allocator nodes are non-RAII
        // handles into this allocator, so dropping both together is what makes
        // per-element frees unnecessary (and a stale node impossible).
        m_AtlasAllocator = AtlasAllocator{};
        m_Sectors.clear();
        m_SectorFeedback.clear();
        m_HasSectorFeedback = false;
        m_NextSizingSector = 0;

        m_FeedbackDims = glm::uvec2(0u);
        m_FeedbackWords = 0;
        m_NextReadbackSlot = 0;
        m_NextAnalysisSequence = 1;
        m_AdoptedAnalysisSequence = 0;
        m_IndirectionDirty = false;
        m_HasBakedModel = false;
        m_ShadingReady = false;
        m_Created = false;
        m_Stats = Stats{};
    }

    void TerrainVirtualTexture::DestroyReadbackSlots()
    {
        const bool deviceAlive = RenderCommand::IsDeviceAvailable();
        for (auto& slot : m_ReadbackSlots)
        {
            if (deviceAlive)
            {
                if (slot.m_Fence != 0)
                {
                    RenderCommand::DestroyFence(slot.m_Fence);
                }
                if (slot.m_Buffer.IsValid())
                {
                    RenderCommand::DeleteBuffer(slot.m_Buffer);
                }
            }
            slot = ReadbackSlot{};
        }
    }

    void TerrainVirtualTexture::Invalidate()
    {
        // Deallocate rather than Destroy: the physical tiles keep their storage,
        // they just stop being claimed by anyone, so the next frame re-bakes into
        // them.
        //
        // **The unmap has to be written HERE, by hand.** DeallocateObject does
        // not fire the eviction listener — only an LRU eviction does — so
        // dropping every page without recording it would leave the whole
        // indirection map pointing into a cache nothing owns any more. This is
        // the one residency change in the class with no notification behind it.
        for (const auto& [key, tile] : m_Resident)
        {
            (void)tile;
            VTRecordEviction(m_IndirectionDelta, key);
            m_PageCache.DeallocateObject(key);
        }
        m_Resident.clear();
        m_BakeList.clear();
        m_IndirectionDirty = true;
        m_ShadingReady = false;
    }

    RHI::ResourceHandle TerrainVirtualTexture::GetIndirectionHandle() const
    {
        return m_IndirectionTexture ? m_IndirectionTexture->GetRHIHandle() : RHI::ResourceHandle{};
    }

    RHI::ResourceHandle TerrainVirtualTexture::GetCacheHandle() const
    {
        return m_CacheTexture ? m_CacheTexture->GetRHIHandle() : RHI::ResourceHandle{};
    }

    RHI::ResourceHandle TerrainVirtualTexture::GetFeedbackBufferHandle() const
    {
        return m_FeedbackBuffer ? m_FeedbackBuffer->GetRHIHandle() : RHI::ResourceHandle{};
    }

    void TerrainVirtualTexture::FillShaderParams(glm::vec4& outParams0, glm::vec4& outParams1, glm::vec4& outParams2,
                                                 glm::vec4& outParams3) const
    {
        outParams0 = glm::vec4(static_cast<f32>(m_Config.VirtualPagesWide), static_cast<f32>(m_Config.PageTexels),
                               static_cast<f32>(m_Config.BorderTexels), static_cast<f32>(m_Config.TileTexels()));
        outParams1 = glm::vec4(static_cast<f32>(m_Config.CacheTexels()), static_cast<f32>(m_Config.MaxMip()),
                               static_cast<f32>(m_FeedbackDims.x), static_cast<f32>(m_FeedbackDims.y));

        f32 downscaleLog2 = 0.0f;
        for (u32 v = m_Config.FeedbackDownscale; v > 1u; v >>= 1u)
        {
            downscaleLog2 += 1.0f;
        }
        outParams2 = glm::vec4(m_ShadingReady ? 1.0f : 0.0f, static_cast<f32>(m_FeedbackFrame),
                               static_cast<f32>(m_Config.FeedbackDownscale), downscaleLog2);
        outParams3 = glm::vec4(static_cast<f32>(m_Config.EffectiveSectorsWide()),
                               m_Config.TrilinearEnabled ? 1.0f : 0.0f, 0.0f, 0.0f);
    }

    void TerrainVirtualTexture::FillShaderSectorTable(std::span<glm::vec4> outSectors) const
    {
        // A short span is silent by construction: the clamp below would just
        // drop the tail sectors, and a dropped sector decodes as unready, so
        // its pixels take the splat path and the frame looks merely coarse
        // rather than wrong. Fail loudly in debug instead — the only way to get
        // here is a UBO array that no longer matches kTerrainVTMaxSectors.
        OLO_CORE_ASSERT(outSectors.size() >= 2u * m_Sectors.size(),
                        "TerrainVirtualTexture: sector-table span too small for the configured sector count");

        // Zero first: entries past the configured sector count decode as
        // unready, so a shader indexing a stale slot falls back to the splat
        // path rather than sampling through a dead rect.
        std::ranges::fill(outSectors, glm::vec4(0.0f));

        const f32 atlasPages = static_cast<f32>(m_Config.VirtualPagesWide);
        const sizet count = std::min(m_Sectors.size(), outSectors.size() / 2u);
        for (sizet i = 0; i < count; ++i)
        {
            const SectorImage& sector = m_Sectors[i];
            if (!sector.m_Snapshot.IsAllocated())
            {
                continue;
            }
            const f32 sizePages = static_cast<f32>(sector.m_Snapshot.m_SizePages);
            // DerivScale is texels across the image CONTENT — sizePages *
            // PageTexels, and deliberately not the bordered tile size: the
            // reference subtracts its border here a second time and ships a
            // ~0.09-mip bias for it.
            outSectors[2u * i] = glm::vec4(static_cast<f32>(sector.m_Snapshot.m_OriginX) / atlasPages,
                                           static_cast<f32>(sector.m_Snapshot.m_OriginY) / atlasPages,
                                           sizePages / atlasPages, sizePages * static_cast<f32>(m_Config.PageTexels));
            outSectors[2u * i + 1u] =
                glm::vec4(static_cast<f32>(sector.m_Snapshot.MaxMip()), sector.m_Ready ? 1.0f : 0.0f, 0.0f, 0.0f);
        }
    }

    glm::uvec2 TerrainVirtualTexture::TileCoord(u32 tileIndex) const
    {
        // Delegates rather than duplicates: the delta and its equivalence test
        // reach the same arithmetic through VTTileCoord, and a second copy here
        // is a second thing to get wrong.
        return VTTileCoord(m_Config, tileIndex);
    }

    void TerrainVirtualTexture::EnsureTimingQueries()
    {
        if (m_TimingQueriesReady || !RenderCommand::IsDeviceAvailable())
        {
            return;
        }

        std::array<RHI::ResourceHandle, kTimingSlots * 2u> queries{};
        RenderCommand::CreateQueries(RHI::QueryType::Timestamp, queries);

        // Validated BEFORE anything is adopted, and released out of the array
        // rather than out of the slots. Timing is instrumentation, not a feature
        // — a backend that cannot hand out queries just leaves the GPU-ms fields
        // at zero — but a PARTIAL failure would otherwise strand every handle
        // past the first bad one, which no later call can reach.
        const bool allValid = std::ranges::all_of(queries, [](const RHI::ResourceHandle& q)
                                                  { return q.IsValid(); });
        if (!allValid)
        {
            std::array<RHI::ResourceHandle, kTimingSlots * 2u> created{};
            u32 count = 0;
            for (const RHI::ResourceHandle& query : queries)
            {
                if (query.IsValid())
                {
                    created[count++] = query;
                }
            }
            if (count > 0)
            {
                RenderCommand::DeleteQueries(std::span<const RHI::ResourceHandle>(created.data(), count));
            }
            return;
        }

        for (u32 slot = 0; slot < kTimingSlots; ++slot)
        {
            m_TimingSlots[slot].m_Begin = queries[slot * 2u];
            m_TimingSlots[slot].m_End = queries[slot * 2u + 1u];
            m_TimingSlots[slot].m_Pending = false;
        }
        m_NextTimingSlot = 0;
        m_TimingQueriesReady = true;
    }

    void TerrainVirtualTexture::DestroyTimingQueries()
    {
        if (RenderCommand::IsDeviceAvailable())
        {
            std::array<RHI::ResourceHandle, kTimingSlots * 2u> queries{};
            u32 count = 0;
            for (const auto& slot : m_TimingSlots)
            {
                if (slot.m_Begin.IsValid())
                {
                    queries[count++] = slot.m_Begin;
                }
                if (slot.m_End.IsValid())
                {
                    queries[count++] = slot.m_End;
                }
            }
            if (count > 0)
            {
                RenderCommand::DeleteQueries(std::span<const RHI::ResourceHandle>(queries.data(), count));
            }
        }
        m_TimingSlots = {};
        m_NextTimingSlot = 0;
        m_TimingQueriesReady = false;
    }

    void TerrainVirtualTexture::PollIndirectionTiming()
    {
        if (!m_TimingQueriesReady)
        {
            return;
        }

        // A poll, never a wait — the same rule the feedback readback lives by.
        // A slot the GPU has not finished is simply left for a later frame.
        for (auto& slot : m_TimingSlots)
        {
            if (!slot.m_Pending || !RenderCommand::IsQueryResultAvailable(slot.m_End))
            {
                continue;
            }
            const u64 begin = RenderCommand::GetQueryResultU64(slot.m_Begin);
            const u64 end = RenderCommand::GetQueryResultU64(slot.m_End);
            slot.m_Pending = false;
            // Timestamps are NANOSECONDS on both backends (RHI::QueryType docs).
            // The guard is not paranoia: a driver that reorders the two stamps
            // would otherwise wrap the unsigned subtraction into a plausible
            // multi-second reading.
            if (end >= begin)
            {
                const f64 ms = static_cast<f64>(end - begin) / 1.0e6;
                f64& best = slot.m_WasFullRebuild ? m_Stats.m_IndirectionRebuildGpuMs
                                                  : m_Stats.m_IndirectionDeltaGpuMs;
                // Minimum, not latest — see the field's note. `!(best > 0.0)` is
                // "no sample yet"; an equality test against 0.0 is forbidden here
                // (cpp-coding-quality.md §2) and would read worse anyway.
                if (!(best > 0.0) || ms < best)
                {
                    best = ms;
                }
            }
        }
    }

    u32 TerrainVirtualTexture::BeginIndirectionTiming()
    {
        if (!m_TimingQueriesReady)
        {
            return kTimingSlots;
        }
        const u32 slot = m_NextTimingSlot;
        if (m_TimingSlots[slot].m_Pending)
        {
            // The GPU is more than kTimingSlots publishes behind. Skip the
            // measurement rather than stall for it; the number is diagnostics.
            return kTimingSlots;
        }
        RenderCommand::WriteTimestamp(m_TimingSlots[slot].m_Begin);
        return slot;
    }

    void TerrainVirtualTexture::EndIndirectionTiming(u32 slot, bool wasFullRebuild)
    {
        if (slot >= kTimingSlots)
        {
            return;
        }
        RenderCommand::WriteTimestamp(m_TimingSlots[slot].m_End);
        m_TimingSlots[slot].m_Pending = true;
        m_TimingSlots[slot].m_WasFullRebuild = wasFullRebuild;
        m_NextTimingSlot = (slot + 1u) % kTimingSlots;
    }

    bool TerrainVirtualTexture::EnsureShaders()
    {
        if (m_ShadersLoaded)
        {
            return true;
        }
        if (m_ShaderLoadFailed)
        {
            return false;
        }

        m_TileBakeShader = ComputeShader::Create("assets/shaders/compute/TerrainVTTileBake.comp");
        m_IndirectionClearShader = ComputeShader::Create("assets/shaders/compute/TerrainVTIndirectionClear.comp");
        m_IndirectionWriteShader = ComputeShader::Create("assets/shaders/compute/TerrainVTIndirectionWrite.comp");
        m_IndirectionFillShader = ComputeShader::Create("assets/shaders/compute/TerrainVTIndirectionFill.comp");
        if (m_Config.CompressedCache)
        {
            m_CompressShader = ComputeShader::Create("assets/shaders/compute/TerrainVTCompressBC7.comp");
        }

        const bool ok = m_TileBakeShader && m_TileBakeShader->IsValid() && m_IndirectionClearShader &&
                        m_IndirectionClearShader->IsValid() && m_IndirectionWriteShader &&
                        m_IndirectionWriteShader->IsValid() && m_IndirectionFillShader &&
                        m_IndirectionFillShader->IsValid() &&
                        (!m_Config.CompressedCache || (m_CompressShader && m_CompressShader->IsValid()));
        if (!ok)
        {
            // Non-fatal, same contract as TerrainGPUQuadtree: the caller keeps
            // the splat path, which still produces a correct frame.
            OLO_CORE_ERROR("TerrainVirtualTexture: compute shader load failed — falling back to the splat path");
            m_ShaderLoadFailed = true;
            return false;
        }
        m_ShadersLoaded = true;
        return true;
    }

    bool TerrainVirtualTexture::EnsureFeedbackResources(u32 viewportWidth, u32 viewportHeight)
    {
        const u32 downscale = m_Config.FeedbackDownscale;
        const glm::uvec2 dims(std::max(1u, DivRoundUp(viewportWidth, downscale)),
                              std::max(1u, DivRoundUp(viewportHeight, downscale)));
        if (m_FeedbackBuffer && dims == m_FeedbackDims)
        {
            return true;
        }

        // A resize orphans whatever is in flight: the ring slots are sized for
        // the OLD dimensions, and reading one into a buffer sized for the new
        // ones would decode garbage as page requests.
        for (auto& pending : m_PendingAnalyses)
        {
            (void)pending.m_Task.Wait();
        }
        m_PendingAnalyses.clear();
        DestroyReadbackSlots();

        m_FeedbackDims = dims;
        m_FeedbackWords = dims.x * dims.y;
        const u32 bytes = m_FeedbackWords * static_cast<u32>(sizeof(u32));

        m_FeedbackBuffer =
            StorageBuffer::Create(bytes, SBL::SSBO_TERRAIN_VT_FEEDBACK, StorageBufferUsage::DynamicCopy);
        if (!m_FeedbackBuffer)
        {
            OLO_CORE_ERROR("TerrainVirtualTexture: feedback buffer allocation failed ({} bytes)", bytes);
            m_FeedbackDims = glm::uvec2(0u);
            m_FeedbackWords = 0;
            return false;
        }
        m_FeedbackBuffer->ClearData();

        for (auto& slot : m_ReadbackSlots)
        {
            slot.m_Buffer = RenderCommand::CreateBufferHandle();
            // DeviceToHost, not a persistent mapping: the RHI's persistent
            // mapping is WRITE-only (see GPUCacheBacking), so a readback has to
            // go through a device-to-host buffer and glGetBufferSubData. What
            // keeps that from stalling is the FENCE, not the mapping.
            RenderCommand::AllocateBufferStorage(slot.m_Buffer, bytes, RHI::MemoryResidency::DeviceToHost);
        }
        m_NextReadbackSlot = 0;
        return true;
    }

    void TerrainVirtualTexture::CaptureFeedback()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_FeedbackBuffer)
        {
            return;
        }

        // The terrain fragment stage wrote this buffer during the PREVIOUS
        // frame's draws; make those writes visible to the copy below.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        ReadbackSlot& slot = m_ReadbackSlots[m_NextReadbackSlot];
        if (!slot.m_Pending && slot.m_Buffer.IsValid())
        {
            const u32 bytes = m_FeedbackWords * static_cast<u32>(sizeof(u32));
            RenderCommand::CopyBufferSubData(m_FeedbackBuffer->GetRHIHandle(), slot.m_Buffer, 0, 0, bytes);
            slot.m_Fence = RenderCommand::CreateFence();
            slot.m_Pending = true;
            m_NextReadbackSlot = (m_NextReadbackSlot + 1u) % kReadbackSlots;
        }
        // A full ring means every slot is still executing — skip the capture and
        // keep the newest frame instead of blocking on the oldest.

        // Unconditional, including on the skipped-capture path: a feedback word
        // is only meaningful for the frame that wrote it, and leaving last
        // frame's words in place would make a page the camera has turned away
        // from look permanently requested.
        m_FeedbackBuffer->ClearData();

        m_FeedbackFrame = (m_FeedbackFrame + 1u) % (m_Config.FeedbackDownscale * m_Config.FeedbackDownscale);
    }

    void TerrainVirtualTexture::PollReadback()
    {
        OLO_PROFILE_FUNCTION();

        u32 inFlight = 0;
        // OLDEST FIRST, not array order. `m_NextReadbackSlot` is the slot the
        // next capture will use, so it is also the oldest one still in flight;
        // walking from there wraps through the ring in ISSUE order. That matters
        // because RetireAnalysis takes the LAST completed analysis as the current
        // one — poll two slots in array order and a wrapped ring hands it the
        // older feedback frame, i.e. a camera pose that has already moved on.
        for (u32 offset = 0; offset < kReadbackSlots; ++offset)
        {
            ReadbackSlot& slot = m_ReadbackSlots[(m_NextReadbackSlot + offset) % kReadbackSlots];
            if (!slot.m_Pending)
            {
                continue;
            }
            // THE WHOLE POINT: ask, never wait. IsFenceSignaled is a poll, so a
            // slot the GPU has not finished is simply left for a later frame.
            // There is no ClientWaitFence anywhere in this class, and adding one
            // would reintroduce exactly the mid-frame stall this design exists to
            // avoid.
            if (!RenderCommand::IsFenceSignaled(slot.m_Fence))
            {
                ++inFlight;
                continue;
            }

            auto feedback = std::make_shared<std::vector<u32>>(m_FeedbackWords);
            RenderCommand::ReadBufferSubData(slot.m_Buffer, 0, m_FeedbackWords * sizeof(u32), feedback->data());

            RenderCommand::DestroyFence(slot.m_Fence);
            slot.m_Fence = 0;
            slot.m_Pending = false;

            auto analyzer = std::make_shared<VTFeedbackAnalyzer>();
            // Snapshot the sector table AT LAUNCH: the words in this slot were
            // written against a table up to kReadbackSlots frames old, and the
            // live one may have resized an image since. The analysis judges the
            // words against what it can (this copy); servicing re-validates
            // every surviving request against the live table.
            auto sectors = SnapshotSectors();
            const u32 atlasMaxMip = m_Config.MaxMip();
            PendingAnalysis pending;
            pending.m_Feedback = feedback;
            pending.m_Analyzer = analyzer;
            pending.m_Sequence = m_NextAnalysisSequence++;
            pending.m_Task = Tasks::Launch(
                "TerrainVTFeedbackAnalyze",
                [feedback, analyzer, sectors, atlasMaxMip]() -> bool
                {
                    analyzer->Analyze(*feedback, *sectors, atlasMaxMip);
                    return true;
                },
                Tasks::ETaskPriority::BackgroundNormal);
            m_PendingAnalyses.push_back(std::move(pending));
        }
        m_Stats.m_ReadbackSlotsInFlight = inFlight;
    }

    void TerrainVirtualTexture::RetireAnalysis()
    {
        OLO_PROFILE_FUNCTION();

        for (auto it = m_PendingAnalyses.begin(); it != m_PendingAnalyses.end();)
        {
            if (!it->m_Task.IsCompleted())
            {
                ++it;
                continue;
            }
            // THE NEWEST CAPTURE WINS, and the sequence number is what decides
            // that rather than position in this vector. Analyses are LAUNCHED in
            // capture order but complete in whatever order the task scheduler
            // finishes them, so a slow older one can land on a later frame than a
            // fast newer one — and without this guard it would overwrite the
            // newer request list with a camera pose that has already moved on,
            // spending a frame of bake budget on stale pages and making the
            // stats counters go backwards.
            //
            // A superseded analysis is still ERASED, just not adopted: it has
            // completed, so holding it would only leak the feedback copy.
            if (it->m_Sequence > m_AdoptedAnalysisSequence)
            {
                m_AdoptedAnalysisSequence = it->m_Sequence;
                m_Requests = it->m_Analyzer->GetRequests();
                m_SectorFeedback = it->m_Analyzer->GetSectorFeedback();
                m_HasSectorFeedback = true;
                m_Stats.m_PagesRequested = static_cast<u32>(m_Requests.size());
                m_Stats.m_FeedbackTexelsWritten = it->m_Analyzer->GetWrittenTexelCount();
                m_Stats.m_StaleFeedbackTexels = it->m_Analyzer->GetStaleTexelCount();
            }
            it = m_PendingAnalyses.erase(it);
        }
    }

    std::shared_ptr<std::vector<VTSectorSnapshot>> TerrainVirtualTexture::SnapshotSectors() const
    {
        auto snapshot = std::make_shared<std::vector<VTSectorSnapshot>>();
        snapshot->reserve(m_Sectors.size());
        for (const SectorImage& sector : m_Sectors)
        {
            snapshot->push_back(sector.m_Snapshot);
        }
        return snapshot;
    }

    u32 TerrainVirtualTexture::FindOwningSector(u32 pageKey) const
    {
        for (u32 i = 0; i < m_Sectors.size(); ++i)
        {
            if (m_Sectors[i].m_Snapshot.Contains(pageKey))
            {
                return i;
            }
        }
        return kVTMaxSectorCount;
    }

    void TerrainVirtualTexture::ResizeSectorImage(u32 sectorIndex, u32 newSize)
    {
        SectorImage& sector = m_Sectors[sectorIndex];
        if (sector.m_Snapshot.m_SizePages == newSize)
        {
            return;
        }

        // Allocate-new-THEN-free-old (the reference's order): the two rects
        // coexist for a moment, so a full atlas degrades to keeping the old
        // image rather than losing it — freeing first would let another
        // sector's same-frame allocation take the space and strand this one.
        const u32 newNode = m_AtlasAllocator.Allocate(newSize);
        if (newNode == AtlasAllocator::kInvalidNode)
        {
            ++m_Stats.m_ImageAllocFailures;
            return;
        }
        const AtlasAllocator::Region region = m_AtlasAllocator.GetRegion(newNode);

        const VTSectorSnapshot oldSnapshot = sector.m_Snapshot;
        VTSectorSnapshot newSnapshot;
        newSnapshot.m_OriginX = static_cast<u16>(region.X);
        newSnapshot.m_OriginY = static_cast<u16>(region.Y);
        newSnapshot.m_SizePages = static_cast<u16>(newSize);

        if (oldSnapshot.IsAllocated())
        {
            // REMAP, not rebake: every resident page whose density exists in
            // the new pyramid keeps its physical tile — VTRemapPageKey is a
            // mip-index shift plus origin translation, MoveObject renames it
            // in the cache without touching the LRU chain's payload, and the
            // delta records the texel move. Only a shrink's finest level has
            // nowhere to go.
            //
            // Collected first: the walk mutates m_Resident.
            std::vector<std::pair<u32, u32>> owned;
            for (const auto& [key, tile] : m_Resident)
            {
                if (oldSnapshot.Contains(key))
                {
                    owned.emplace_back(key, tile);
                }
            }
            for (const auto& [key, tile] : owned)
            {
                m_Resident.erase(key);
                // The old texel's unmap is recorded by hand for BOTH outcomes:
                // MoveObject and DeallocateObject are explicit calls, and
                // neither fires the eviction listener (only an LRU eviction
                // does) — the same rule Invalidate() lives by.
                VTRecordEviction(m_IndirectionDelta, key);
                const std::optional<u32> newKey = VTRemapPageKey(key, oldSnapshot, newSnapshot);
                if (newKey.has_value())
                {
                    m_PageCache.MoveObject(key, *newKey);
                    m_Resident[*newKey] = tile;
                    VTRecordMapping(m_IndirectionDelta, m_Config, *newKey, tile);
                    ++m_Stats.m_PagesRemappedTotal;
                }
                else
                {
                    m_PageCache.DeallocateObject(key);
                    ++m_Stats.m_PagesDroppedOnShrink;
                }
            }
            m_IndirectionDirty = true;
            m_AtlasAllocator.Free(sector.m_AllocNode);
            ++m_Stats.m_ImageResizesTotal;
        }

        sector.m_AllocNode = newNode;
        sector.m_Snapshot = newSnapshot;
    }

    void TerrainVirtualTexture::ApplyAdaptiveSizing()
    {
        OLO_PROFILE_FUNCTION();

        // One policy step per ADOPTED analysis, not per frame: the streak and
        // cooldown constants are counted in analyses, so re-running the policy
        // on a frame with no new feedback would just re-walk the same
        // aggregates.
        if (!m_Config.AdaptiveEnabled)
        {
            // Drop what was adopted while adaptivity was off. RetireAnalysis
            // keeps setting the flag regardless of the toggle, so leaving it
            // set would let a later tick-on consume aggregates from whenever
            // analyses last ran — a camera pose that no longer exists — and
            // resize from it before the first fresh analysis lands.
            m_HasSectorFeedback = false;
            return;
        }
        if (!m_HasSectorFeedback)
        {
            return;
        }
        m_HasSectorFeedback = false;

        // Every allocated sector gets a policy step, because the streak and
        // cooldown constants are counted in analyses and only mean anything if
        // all of them tick at the same rate. What is budgeted is the RESIZE:
        // each one remaps every resident page of the image and dirties the
        // indirection map. A sector that wants a resize it has no budget for
        // keeps its streak (VTDesiredImageSize commits nothing when it is told
        // it cannot resize) and gets it on a later analysis; the round-robin
        // cursor resumes past the last sector served so a low-index sector
        // that keeps wanting one cannot hold the budget forever.
        u32 resizes = 0;
        u32 served = 0;
        const u32 sectorCount = static_cast<u32>(m_Sectors.size());
        for (u32 step = 0; step < sectorCount; ++step)
        {
            const u32 index = (m_NextSizingSector + step) % sectorCount;
            SectorImage& sector = m_Sectors[index];
            if (!sector.m_Snapshot.IsAllocated())
            {
                continue;
            }
            const VTSectorFeedback feedback =
                index < m_SectorFeedback.size() ? m_SectorFeedback[index] : VTSectorFeedback{};
            const u32 wanted = VTDesiredImageSize(
                sector.m_Snapshot.m_SizePages, feedback, sector.m_Sizing, m_Config.EffectiveMinImagePagesWide(),
                m_Config.EffectiveMaxImagePagesWide(), resizes < kVTMaxResizesPerFrame);
            if (wanted != sector.m_Snapshot.m_SizePages)
            {
                ResizeSectorImage(index, wanted);
                ++resizes;
                served = step + 1u;
            }
        }
        m_NextSizingSector = (m_NextSizingSector + std::max(served, 1u)) % std::max(sectorCount, 1u);
    }

    void TerrainVirtualTexture::UpdateSectorReadiness()
    {
        u32 ready = 0;
        u32 atlasPages = 0;
        for (SectorImage& sector : m_Sectors)
        {
            // Ready = the sector's own fallback terminator is resident AND the
            // indirection map the shader will read reflects it. While the
            // publish is deferred (a shader still loading), advertising ready
            // would sample a map that still points at the pre-change state.
            sector.m_Ready = sector.m_Snapshot.IsAllocated() && !m_IndirectionDirty &&
                             m_Resident.contains(sector.m_Snapshot.PinKey());
            ready += sector.m_Ready ? 1u : 0u;
            atlasPages += static_cast<u32>(sector.m_Snapshot.m_SizePages) * sector.m_Snapshot.m_SizePages;
        }
        m_Stats.m_SectorsReady = ready;
        m_Stats.m_AtlasPagesAllocated = atlasPages;
    }

    void TerrainVirtualTexture::ServiceRequests()
    {
        OLO_PROFILE_FUNCTION();

        m_BakeList.clear();
        m_Stats.m_TilesBakedThisFrame = 0;
        m_Stats.m_BudgetStarvedRequests = 0;
        m_Stats.m_WorkingSetExceedsCache = false;

        if (m_Requests.empty())
        {
            return;
        }

        // Filter against the LIVE sector table, every frame: the request list
        // persists across frames and the analysis judged it against a
        // snapshot, so a resize in between leaves entries addressing pages no
        // image owns any more. Allocating one would bake terrain content into
        // whatever image now occupies that atlas space — plausible pixels,
        // wrong place.
        std::erase_if(m_Requests,
                      [this](const VTPageRequest& request)
                      { return FindOwningSector(request.m_PageKey) >= m_Sectors.size(); });
        if (m_Requests.empty())
        {
            return;
        }

        // The policy itself lives in TerrainVirtualTextureTypes.h so the headless
        // test drives THIS function rather than a transcription of it — see the
        // note there for why both the touch ORDER and the allocation CAP are
        // load-bearing, and what each one prevents.
        const VTServiceOutcome outcome = VTServicePageRequests(m_PageCache, m_Requests, m_Config.CacheTileCount(),
                                                               m_Config.MaxTileBakesPerFrame);

        for (const auto& [pageKey, tile] : outcome.m_Mapped)
        {
            m_Resident[pageKey] = tile;
            const glm::uvec2 coord = TileCoord(tile);
            VTBakeRequest request{ pageKey, coord.x, coord.y, 0u, glm::vec4(0.0f) };
            // The page's terrain footprint depends on which sector owns it and
            // how big that image currently is — resolved here, where the live
            // table is, so the bake kernel just samples the rect it is handed.
            const u32 sectorIndex = FindOwningSector(pageKey);
            OLO_CORE_ASSERT(sectorIndex < m_Sectors.size(),
                            "TerrainVirtualTexture: mapped a page the filter above should have dropped");
            request.m_UVRect =
                VTPageTerrainUVRect(m_Config, m_Sectors[sectorIndex].m_Snapshot, sectorIndex, pageKey);
            m_BakeList.push_back(request);
            // Recorded AFTER the allocation that may have evicted somebody: the
            // listener above already wrote that page's unmap, so the delta ends
            // the frame with both halves of the swap in the right order.
            VTRecordMapping(m_IndirectionDelta, m_Config, pageKey, tile);
            m_IndirectionDirty = true;
        }

        // The one outcome the policy reports by stopping early rather than by a
        // counter: the cache had nothing evictable at all. Warn once, because the
        // fix (raise CacheTilesWide or lower VirtualPagesWide) is a one-time
        // decision and the condition persists every frame.
        // Misses existed, none were deferred by the budget, and none mapped —
        // which leaves only "AllocatePages failed". A budget-limited frame
        // reports its misses as deferred, and a frame whose working set exceeds
        // the cache reports m_WorkingSetExceedsCache, so neither lands here.
        const bool allocationStalled =
            outcome.m_Mapped.empty() && outcome.m_Deferred == 0u && outcome.m_Touched < m_Requests.size();
        if (allocationStalled && !m_AllocationWarned)
        {
            m_AllocationWarned = true;
            OLO_CORE_WARN("TerrainVirtualTexture: no page could be mapped with {} of {} tiles resident — the "
                          "cache has nothing evictable. Raise CacheTilesWide or lower VirtualPagesWide.",
                          m_Resident.size(), m_Config.CacheTileCount());
        }

        m_Stats.m_BudgetStarvedRequests = outcome.m_Deferred;
        m_Stats.m_WorkingSetExceedsCache = outcome.m_WorkingSetExceedsCache;
        m_Stats.m_TilesBakedThisFrame = static_cast<u32>(m_BakeList.size());
        m_Stats.m_TilesBakedTotal += m_Stats.m_TilesBakedThisFrame;
        m_Stats.m_ResidentTiles = static_cast<u32>(m_Resident.size());
    }

    void TerrainVirtualTexture::BakeTiles(const FrameInputs& inputs)
    {
        OLO_PROFILE_FUNCTION();

        if (m_BakeList.empty() || !m_TileBakeShader || !m_BakeBuffer)
        {
            return;
        }

        const TerrainMaterial& material = *inputs.m_Material;

        // IsBuilt() only tests the ALBEDO array, but all three are dereferenced
        // below and each Texture2DArray::Create can fail on its own. Relying on
        // the caller's gate would turn a failed normal/ARM allocation into a null
        // dereference here rather than a skipped bake.
        const Ref<Texture2DArray> albedoArray = material.GetAlbedoArray();
        const Ref<Texture2DArray> normalArray = material.GetNormalArray();
        const Ref<Texture2DArray> armArray = material.GetARMArray();
        if (!albedoArray || !normalArray || !armArray)
        {
            return;
        }

        const u32 layerCount = material.GetLayerCount();

        VTBakeHeader header;
        header.m_Config0 = glm::uvec4(m_Config.VirtualPagesWide, m_Config.PageTexels, m_Config.BorderTexels,
                                      static_cast<u32>(m_BakeList.size()));
        header.m_Config1 = glm::uvec4(layerCount, m_Config.TileTexels(), material.GetLayerResolution(), 0u);
        header.m_World = glm::vec4(inputs.m_WorldSizeX, inputs.m_WorldSizeZ, inputs.m_HeightScale,
                                   inputs.m_TriplanarSharpness);
        const u32 heightmapResolution = std::max(1u, inputs.m_HeightmapResolution);
        header.m_Texel = glm::vec4(1.0f / static_cast<f32>(heightmapResolution), 0.0f, 0.0f, 0.0f);
        header.m_Model = inputs.m_Model;
        for (u32 i = 0; i < std::min(layerCount, 4u); ++i)
        {
            header.m_Tiling0[static_cast<i32>(i)] = material.GetLayer(i).TilingScale;
            header.m_Sharp0[static_cast<i32>(i)] = material.GetLayer(i).HeightBlendSharpness;
        }
        for (u32 i = 4; i < std::min(layerCount, 8u); ++i)
        {
            header.m_Tiling1[static_cast<i32>(i - 4u)] = material.GetLayer(i).TilingScale;
            header.m_Sharp1[static_cast<i32>(i - 4u)] = material.GetLayer(i).HeightBlendSharpness;
        }

        const sizet requestBytes = m_BakeList.size() * sizeof(VTBakeRequest);
        m_UploadScratch.resize(sizeof(VTBakeHeader) + requestBytes);
        std::memcpy(m_UploadScratch.data(), &header, sizeof(VTBakeHeader));
        std::memcpy(m_UploadScratch.data() + sizeof(VTBakeHeader), m_BakeList.data(), requestBytes);
        const bool compressed = m_Config.CompressedCache;
        if (compressed)
        {
            // The kernel's destination addressing is entirely the request's
            // TileX/TileY, so redirecting the batch into the scratch strip is
            // a coordinate patch on the UPLOAD copy: slot i at (i, 0).
            // m_BakeList keeps the real cache coordinates — that is what the
            // per-tile copies in CompressAndCopyTiles aim at.
            auto* requests = reinterpret_cast<VTBakeRequest*>(m_UploadScratch.data() + sizeof(VTBakeHeader));
            for (u32 i = 0; i < static_cast<u32>(m_BakeList.size()); ++i)
            {
                requests[i].m_TileX = i;
                requests[i].m_TileY = 0u;
            }
        }
        m_BakeBuffer->SetData(m_UploadScratch.data(), static_cast<u32>(m_UploadScratch.size()), 0);

        // BIND THE PROGRAM FIRST, then the resources.
        //
        // Every bind below goes through the HeapBinding seam rather than
        // RenderCommand directly — that is the sanctioned spelling (issue #691
        // the RHI boundary ratchet counts raw facade bind sites), and it
        // forks on `Shader::IsBoundProgramBindless()` for the program IN FLIGHT.
        // These kernels declare their inputs slot-based, so the fork must be
        // allowed to see a VT program and take the fallback, which issues a real
        // bind. Called with some other program bound, a bindless answer would
        // stage an offset and bind NOTHING — the bake would composite from
        // whatever was left in those slots.
        //
        // No FlushOffsets(): nothing is staged on the fallback path. Converting
        // these kernels to the bindless route later means adding one before each
        // dispatch, and a per-iteration one in PublishIndirection's loops.
        m_TileBakeShader->Bind();

        using enum RHI::HeapSlotLifetime;
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_HEIGHTMAP, inputs.m_Heightmap, Persistent);
        if (const auto splat0 = material.GetSplatmap(0))
        {
            HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_SPLATMAP, splat0->GetRHIHandle(), Persistent);
        }
        if (const auto splat1 = material.GetSplatmap(1))
        {
            HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_SPLATMAP_1, splat1->GetRHIHandle(), Persistent);
        }
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_ALBEDO_ARRAY, albedoArray->GetRHIHandle(), Persistent,
                                         {}, RHI::NullSamplerKind::Texture2DArray);
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_NORMAL_ARRAY, normalArray->GetRHIHandle(), Persistent,
                                         {}, RHI::NullSamplerKind::Texture2DArray);
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_ARM_ARRAY, armArray->GetRHIHandle(), Persistent, {},
                                         RHI::NullSamplerKind::Texture2DArray);

        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_VT_BAKE, m_BakeBuffer->GetRHIHandle());
        const RHI::ResourceHandle bakeTarget =
            compressed ? m_ScratchTexture->GetRHIHandle() : m_CacheTexture->GetRHIHandle();
        HeapBinding::BindImageOrOffset(kImageUnitTarget, bakeTarget, 0, /*layered*/ true, 0,
                                       RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);

        const u32 groups = DivRoundUp(m_Config.TileTexels(), kTileBakeGroupSize);
        RenderCommand::DispatchCompute(groups, groups, static_cast<u32>(m_BakeList.size()));
        if (compressed)
        {
            // The compress kernel imageLoads the scratch the dispatch above
            // just stored; the fetch/copy visibility for the cache itself is
            // CompressAndCopyTiles' business.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
            CompressAndCopyTiles();
        }
        else
        {
            // The lit passes SAMPLE the cache, so the barrier has to cover
            // texture fetch, not just further image access.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        }
    }

    void TerrainVirtualTexture::CompressAndCopyTiles()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_CompressShader || !m_ScratchTexture || !m_StagingTexture)
        {
            return;
        }

        using enum RHI::HeapSlotLifetime;
        const u32 tileTexels = m_Config.TileTexels();
        const u32 tileBlocks = tileTexels / 4u;
        const u32 count = static_cast<u32>(m_BakeList.size());

        // Program first, then resources — same seam rules as the bake. The
        // kernel reads the bake SSBO's header (still bound) for the batch
        // count and tile size.
        m_CompressShader->Bind();
        HeapBinding::BindImageOrOffset(kImageUnitTarget, m_ScratchTexture->GetRHIHandle(), 0, /*layered*/ true, 0,
                                       RHI::Access::StorageRead, RHI::Format::RGBA8UNorm, Persistent);
        HeapBinding::BindImageOrOffset(kImageUnitCoarser, m_StagingTexture->GetRHIHandle(), 0, /*layered*/ true, 0,
                                       RHI::Access::StorageWrite, RHI::Format::RGBA32UInt, Persistent);

        // One invocation per 4x4 block; z = slot * 2 + layer.
        const u32 groups = DivRoundUp(tileBlocks, kTileBakeGroupSize);
        RenderCommand::DispatchCompute(groups, groups, count * 2u);
        // The copies below READ the staging image the dispatch just stored.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureUpdate);

        // One block-compatible copy per (tile, layer): one RGBA32UI staging
        // texel is one 16-byte BC7 block, so width/height are in BLOCKS while
        // the destination offsets are in cache TEXELS — tileTexels is a
        // multiple of 4, so every destination offset stays block-aligned.
        const RHI::ResourceHandle staging = m_StagingTexture->GetRHIHandle();
        const RHI::ResourceHandle cache = m_CacheTexture->GetRHIHandle();
        // Spelled in full: a using-enum here would put an enumerator named
        // Texture2DArray in scope beside the class of the same name.
        constexpr auto kArrayTarget = RendererAPI::TextureTargetType::Texture2DArray;
        for (u32 i = 0; i < count; ++i)
        {
            const VTBakeRequest& request = m_BakeList[i];
            for (u32 layer = 0; layer < 2u; ++layer)
            {
                RenderCommand::CopyImageSubDataRegion(
                    staging, kArrayTarget, 0, static_cast<i32>(i * tileBlocks), 0, static_cast<i32>(layer), cache,
                    kArrayTarget, 0, static_cast<i32>(request.m_TileX * tileTexels),
                    static_cast<i32>(request.m_TileY * tileTexels), static_cast<i32>(layer), tileBlocks, tileBlocks);
            }
        }
        // The lit passes SAMPLE the cache the copies just wrote.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch);

        m_Stats.m_TilesCompressedTotal += count;
    }

    void TerrainVirtualTexture::PublishIndirection()
    {
        OLO_PROFILE_FUNCTION();

        // Outside the dirty check on purpose: a publish's timestamps resolve
        // several frames after it was issued, so the poll has to run on frames
        // that publish nothing.
        PollIndirectionTiming();

        if (!m_IndirectionDirty || !m_IndirectionClearShader || !m_IndirectionWriteShader ||
            !m_IndirectionFillShader || !m_IndirectionUpdateBuffer)
        {
            return;
        }

        using enum RHI::HeapSlotLifetime;
        const u32 mipCount = m_Config.MipCount();
        const RHI::ResourceHandle indirection = m_IndirectionTexture->GetRHIHandle();

        // The whole-map rebuild, kept as the fallback for the cases a
        // delta cannot express. Neither of the two real ones is "the map is
        // stale" — a stale map is precisely what a delta IS for — they are both
        // "the map's contents are not known":
        //
        //  - freshly created texture storage, whose bytes are undefined until
        //    something writes them (Configure sets the flag);
        //  - a delta that outgrew its upload buffer, which needs frame after
        //    frame of residency changes with no publish in between (a shader
        //    still loading). Uploading it in halves would leave the map
        //    self-inconsistent between the two dispatches.
        //
        // The third trigger is the A/B lever, which is not a fallback at all —
        // it exists so the path this slice replaced can still be measured and
        // bisected against. It is applied first, so it wins over the capacity
        // check rather than racing it.
        //
        // Expressing the rebuild AS a delta — every resident page mapped, every
        // level's fill rect full — rather than as a second code path is what
        // keeps the two in step: there is one write loop and one fill loop below,
        // and all the flag changes is the clear pass and the rectangles.
        m_IndirectionNeedsFullRebuild = m_IndirectionNeedsFullRebuild || Levers::TerrainVtFullRebuild();
        if (m_IndirectionDelta.Size() > m_IndirectionUpdateCapacity)
        {
            OLO_CORE_WARN("TerrainVirtualTexture: {} indirection updates accumulated past the {}-entry upload "
                          "buffer — rebuilding the map instead",
                          m_IndirectionDelta.Size(), m_IndirectionUpdateCapacity);
            m_IndirectionNeedsFullRebuild = true;
        }
        if (m_IndirectionNeedsFullRebuild)
        {
            m_IndirectionDelta.Reset(m_Config);
            for (const auto& [key, tile] : m_Resident)
            {
                VTRecordMapping(m_IndirectionDelta, m_Config, key, tile);
            }
            m_IndirectionDelta.MarkEverythingDirty();
        }

        m_IndirectionDelta.Finalize();
        const bool fullRebuild = m_IndirectionDelta.WantsFullRebuild();

        if (m_IndirectionDelta.IsEmpty() && !fullRebuild)
        {
            // Dirty with nothing to write: Invalidate() on an already-empty
            // resident set is the reachable case. The map is already correct.
            m_IndirectionDelta.Reset(m_Config);
            m_IndirectionDirty = false;
            return;
        }

        const u32 timingSlot = BeginIndirectionTiming();

        // One upload for the whole list; each mip's dispatch reads its own
        // [base, count) window out of it.
        if (!m_IndirectionDelta.IsEmpty())
        {
            const auto& updates = m_IndirectionDelta.GetUpdates();
            m_IndirectionUpdateBuffer->SetData(updates.data(),
                                               static_cast<u32>(updates.size() * sizeof(VTIndirectionUpdate)),
                                               static_cast<u32>(sizeof(VTIndirectionHeader)));
        }
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_VT_INDIRECTION, m_IndirectionUpdateBuffer->GetRHIHandle());

        // Pass 1 — clear every mip, on a full rebuild ONLY. On the delta path an
        // evicted page is an explicit all-zero ENTRY in the list below, which is
        // the same texel this pass would have written; that equivalence is what
        // lets the clear go away rather than merely shrink.
        u32 texelsWritten = 0;
        if (fullRebuild)
        {
            m_IndirectionClearShader->Bind();
            for (u32 mip = 0; mip < mipCount; ++mip)
            {
                const u32 size = m_Config.VirtualPagesWide >> mip;
                HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, mip, /*layered*/ false, 0,
                                               RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);
                const u32 groups = DivRoundUp(size, kIndirectionClearGroupSize);
                RenderCommand::DispatchCompute(groups, groups, 1);
                // Counted, because it is a texel this publish wrote. Leaving it
                // out made the reported cost of the path this slice replaced
                // roughly half of what it is.
                texelsWritten += size * size;
            }
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
        }

        // Pass 2 — stamp the changed texels, one dispatch per non-empty mip.
        m_IndirectionWriteShader->Bind();
        for (u32 mip = 0; mip < mipCount; ++mip)
        {
            const u32 count = m_IndirectionDelta.GetMipCountAt(mip);
            if (count == 0u)
            {
                continue;
            }
            // A 16-byte CPU write between two dispatches that read the same
            // buffer. GL orders it correctly (the earlier dispatch still sees the
            // old bytes) and the Vulkan backend gives the same guarantee through
            // #691's write snapshots — see
            // docs/agent-rules/vulkan-command-ordered-buffer-writes.md. The
            // alternative, a bound RANGE per mip, needs a glBindBufferRange the
            // RHI facade does not expose.
            const glm::uvec4 writeParams(m_IndirectionDelta.GetMipBase(mip), count, 0u, 0u);
            m_IndirectionUpdateBuffer->SetData(&writeParams, static_cast<u32>(sizeof(writeParams)),
                                               kWriteParamsOffset);

            HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, mip, /*layered*/ false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);
            RenderCommand::DispatchCompute(DivRoundUp(count, kIndirectionWriteGroupSize), 1, 1);
            texelsWritten += count;
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);

        // Pass 3 — propagate coarse to fine. STRICTLY top-down and one dispatch
        // per level with a barrier between: level m reads level m+1's OUTPUT, so
        // batching them would let a level inherit a half-written parent.
        //
        // The incremental change is the EXTENT, not the order: each level runs over the
        // descendants of what changed above it plus what changed at it, and a
        // level with an empty rect is skipped outright. The top-down walk is what
        // makes one rect per level enough — level m+1 is already repaired by the
        // time level m reads it.
        u32 texelsFilled = 0;
        m_IndirectionFillShader->Bind();
        for (i32 mip = static_cast<i32>(mipCount) - 2; mip >= 0; --mip)
        {
            const u32 level = static_cast<u32>(mip);
            const VTIndirectionDelta::Rect& rect = m_IndirectionDelta.GetFillRect(level);
            if (rect.IsEmpty())
            {
                continue;
            }
            const glm::uvec4 fillParams(rect.m_X, rect.m_Y, rect.m_Width, rect.m_Height);
            m_IndirectionUpdateBuffer->SetData(&fillParams, static_cast<u32>(sizeof(fillParams)),
                                               kFillParamsOffset);

            HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, level, /*layered*/ false, 0,
                                           RHI::Access::StorageReadWrite, RHI::Format::RGBA8UNorm, Persistent);
            HeapBinding::BindImageOrOffset(kImageUnitCoarser, indirection, level + 1u, /*layered*/ false, 0,
                                           RHI::Access::StorageRead, RHI::Format::RGBA8UNorm, Persistent);
            RenderCommand::DispatchCompute(DivRoundUp(rect.m_Width, kIndirectionFillGroupSize),
                                           DivRoundUp(rect.m_Height, kIndirectionFillGroupSize), 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
            texelsFilled += rect.m_Width * rect.m_Height;
        }

        // The terrain fragment stage texelFetches this texture.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch);
        EndIndirectionTiming(timingSlot, fullRebuild);

        m_Stats.m_IndirectionTexelsWritten = texelsWritten;
        m_Stats.m_IndirectionTexelsFilled = texelsFilled;
        ++m_Stats.m_IndirectionPublishes;
        if (fullRebuild)
        {
            ++m_Stats.m_IndirectionFullRebuilds;
        }

        m_IndirectionDelta.Reset(m_Config);
        m_IndirectionNeedsFullRebuild = false;
        m_IndirectionDirty = false;
    }

    bool TerrainVirtualTexture::Update(const FrameInputs& inputs)
    {
        OLO_PROFILE_FUNCTION();

        m_ShadingReady = false;
        // The denominator for "how often does a publish actually happen", which
        // is half of what decides whether the publish cost matters at all.
        // Counted before the early-outs so it means "frames the loop ran".
        ++m_Stats.m_FramesUpdated;

        if (!m_Created || inputs.m_Material == nullptr || !inputs.m_Material->IsBuilt() ||
            inputs.m_Material->GetLayerCount() == 0u || !inputs.m_Heightmap.IsValid())
        {
            return false;
        }
        if (!EnsureShaders())
        {
            return false;
        }
        if (!EnsureFeedbackResources(inputs.m_ViewportWidth, inputs.m_ViewportHeight))
        {
            return false;
        }

        // The baked content is world-anchored (the triplanar projection reads
        // absolute world position), so a terrain that moved invalidates every
        // resident tile. Cheaper to notice here than to debug as "the terrain
        // texture did not follow the terrain".
        if (!m_HasBakedModel || MatricesDiffer(m_BakedModel, inputs.m_Model))
        {
            if (m_HasBakedModel)
            {
                Invalidate();
            }
            m_BakedModel = inputs.m_Model;
            m_HasBakedModel = true;
        }

        CaptureFeedback();
        PollReadback();
        RetireAnalysis();
        ApplyAdaptiveSizing();
        ServiceRequests();
        BakeTiles(inputs);
        PublishIndirection();
        UpdateSectorReadiness();

        // Shading is safe once at least one sector's fallback terminator is
        // resident AND published — the sector table gates the rest per pixel:
        // an unready sector's pixels take the splat path, so a half-converged
        // frame is a mixed frame, never a wrong one. Before slice 3 this was
        // the single global pin; the per-sector pins now play that role each
        // inside their own image. Derived from the counter
        // UpdateSectorReadiness just produced, so the stats tool and this gate
        // cannot disagree.
        m_ShadingReady = m_Stats.m_SectorsReady > 0u;
        return m_ShadingReady;
    }
} // namespace OloEngine
