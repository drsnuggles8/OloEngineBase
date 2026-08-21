#include "OloEnginePCH.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"

#include "OloEngine/Core/DebugLevers.h"
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

        Texture2DArraySpecification cacheSpec;
        cacheSpec.Width = m_Config.CacheTexels();
        cacheSpec.Height = m_Config.CacheTexels();
        cacheSpec.Layers = 2; // 0 = albedo + AO, 1 = normal.xy + roughness + metallic
        cacheSpec.Format = Texture2DArrayFormat::RGBA8;
        // No mips, and that is the VT contract rather than an omission: a mip
        // chain over an ATLAS blurs neighbouring tiles into each other, which is
        // precisely what the per-tile border exists to avoid. Minification is
        // handled by picking a coarser PAGE, not a coarser texel.
        cacheSpec.GenerateMipmaps = false;
        m_CacheTexture = Texture2DArray::Create(cacheSpec);

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

        if (!m_IndirectionTexture || !m_CacheTexture || !m_BakeBuffer || !m_IndirectionUpdateBuffer)
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
                // in slice 2.** Slice 1 could get away with only setting the
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

        m_Stats = Stats{};
        m_Stats.m_CacheTileCount = m_Config.CacheTileCount();
        const u64 cacheTexels = static_cast<u64>(m_Config.CacheTexels()) * m_Config.CacheTexels();
        m_Stats.m_CacheBytes = cacheTexels * 4ull * cacheSpec.Layers;
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
        m_FeedbackBuffer = nullptr;
        m_BakeBuffer = nullptr;
        m_IndirectionUpdateBuffer = nullptr;

        m_TileBakeShader = nullptr;
        m_IndirectionClearShader = nullptr;
        m_IndirectionWriteShader = nullptr;
        m_IndirectionFillShader = nullptr;
        m_ShadersLoaded = false;
        m_ShaderLoadFailed = false;

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

    void TerrainVirtualTexture::FillShaderParams(glm::vec4& outParams0, glm::vec4& outParams1,
                                                 glm::vec4& outParams2) const
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

        const bool ok = m_TileBakeShader && m_TileBakeShader->IsValid() && m_IndirectionClearShader &&
                        m_IndirectionClearShader->IsValid() && m_IndirectionWriteShader &&
                        m_IndirectionWriteShader->IsValid() && m_IndirectionFillShader &&
                        m_IndirectionFillShader->IsValid();
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
            const u32 maxMip = m_Config.MaxMip();
            PendingAnalysis pending;
            pending.m_Feedback = feedback;
            pending.m_Analyzer = analyzer;
            pending.m_Sequence = m_NextAnalysisSequence++;
            pending.m_Task = Tasks::Launch(
                "TerrainVTFeedbackAnalyze",
                [feedback, analyzer, maxMip]() -> bool
                {
                    analyzer->Analyze(*feedback, maxMip);
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
                m_Stats.m_PagesRequested = static_cast<u32>(m_Requests.size());
                m_Stats.m_FeedbackTexelsWritten = it->m_Analyzer->GetWrittenTexelCount();
            }
            it = m_PendingAnalyses.erase(it);
        }
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
            m_BakeList.push_back(VTBakeRequest{ pageKey, coord.x, coord.y, 0u });
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
        m_BakeBuffer->SetData(m_UploadScratch.data(), static_cast<u32>(m_UploadScratch.size()), 0);

        // BIND THE PROGRAM FIRST, then the resources.
        //
        // Every bind below goes through the HeapBinding seam rather than
        // RenderCommand directly — that is the sanctioned spelling (issue #691
        // Phase 3; the RHI boundary ratchet counts raw facade bind sites), and it
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
        HeapBinding::BindImageOrOffset(kImageUnitTarget, m_CacheTexture->GetRHIHandle(), 0, /*layered*/ true, 0,
                                       RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);

        const u32 groups = DivRoundUp(m_Config.TileTexels(), kTileBakeGroupSize);
        RenderCommand::DispatchCompute(groups, groups, static_cast<u32>(m_BakeList.size()));
        // The lit passes SAMPLE the cache, so the barrier has to cover texture
        // fetch, not just further image access.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
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

        // Slice 1's whole-map rebuild, kept as the fallback for the cases a
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
            // #691 Phase 8's write snapshots — see
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
        // Slice 2's change is the EXTENT, not the order: each level runs over the
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
        ServiceRequests();
        BakeTiles(inputs);
        PublishIndirection();

        // Shading is safe only once the coarsest page is resident: it is the
        // page every unresolved lookup inherits from, so before it exists the
        // fallback chain ends in a zeroed indirection texel that addresses tile
        // (0,0) — which is not "blurry", it is "wrong, plausibly".
        m_ShadingReady = m_Resident.contains(VTMakePageKey(m_Config.MaxMip(), 0u, 0u)) && !m_IndirectionDirty;
        return m_ShadingReady;
    }
} // namespace OloEngine
