#include "OloEnginePCH.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"

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
#include <cstring>
#include <memory>
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

        // std430 header of the indirection update buffer.
        struct VTIndirectionHeader
        {
            glm::uvec4 m_Params{ 0u }; // baseIndex, count, unused, unused
        };
        static_assert(sizeof(VTIndirectionHeader) == 16, "VTIndirectionHeader must match its std430 twin");

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

        // One update per resident tile is the hard ceiling: the map is rebuilt
        // from the resident set, and the cache cannot hold more pages than it
        // has tiles.
        const u32 updateBytes = static_cast<u32>(sizeof(VTIndirectionHeader)) +
                                m_Config.CacheTileCount() * static_cast<u32>(sizeof(VTIndirectionUpdate));
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
                // our own record and flagging the map is all it does.
                m_Resident.erase(victim);
                m_IndirectionDirty = true;
                ++m_Stats.m_EvictionsTotal;
            });

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

        m_PageCache.SetEvictionListener(nullptr);
        m_PageCache.Destroy();
        m_Resident.clear();
        m_BakeList.clear();
        m_Requests.clear();
        m_UpdateScratch.clear();
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
        m_HasFreshRequests = false;
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
        for (const auto& [key, tile] : m_Resident)
        {
            (void)tile;
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
        return glm::uvec2(tileIndex % m_Config.CacheTilesWide, tileIndex / m_Config.CacheTilesWide);
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

        m_HasFreshRequests = false;
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
                m_HasFreshRequests = true;
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
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_ALBEDO_ARRAY, material.GetAlbedoArray()->GetRHIHandle(),
                                         Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_NORMAL_ARRAY, material.GetNormalArray()->GetRHIHandle(),
                                         Persistent, {}, RHI::NullSamplerKind::Texture2DArray);
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_ARM_ARRAY, material.GetARMArray()->GetRHIHandle(),
                                         Persistent, {}, RHI::NullSamplerKind::Texture2DArray);

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

        if (!m_IndirectionDirty || !m_IndirectionClearShader || !m_IndirectionWriteShader ||
            !m_IndirectionFillShader || !m_IndirectionUpdateBuffer)
        {
            return;
        }

        using enum RHI::HeapSlotLifetime;
        const u32 mipCount = m_Config.MipCount();
        const RHI::ResourceHandle indirection = m_IndirectionTexture->GetRHIHandle();

        // Build the update list, mip-major, so each mip's dispatch is a window
        // [base, count) into one uploaded array.
        m_UpdateScratch.clear();
        m_UpdateScratch.reserve(m_Resident.size());
        std::vector<u32> mipOffsets(mipCount + 1u, 0u);
        for (u32 mip = 0; mip < mipCount; ++mip)
        {
            mipOffsets[mip] = static_cast<u32>(m_UpdateScratch.size());
            for (const auto& [key, tile] : m_Resident)
            {
                if (VTPageKeyMip(key) != mip)
                {
                    continue;
                }
                const glm::uvec2 coord = TileCoord(tile);
                VTIndirectionUpdate update;
                update.m_TexelCoord = (VTPageKeyY(key) << 16u) | VTPageKeyX(key);
                update.m_Packed = VTPackIndirection(coord.x, coord.y, mip, /*direct*/ true);
                m_UpdateScratch.push_back(update);
            }
        }
        mipOffsets[mipCount] = static_cast<u32>(m_UpdateScratch.size());

        if (!m_UpdateScratch.empty())
        {
            m_IndirectionUpdateBuffer->SetData(m_UpdateScratch.data(),
                                               static_cast<u32>(m_UpdateScratch.size() * sizeof(VTIndirectionUpdate)),
                                               static_cast<u32>(sizeof(VTIndirectionHeader)));
        }
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_VT_INDIRECTION, m_IndirectionUpdateBuffer->GetRHIHandle());

        // Pass 1 — clear every mip. An entry the write pass does not re-stamp
        // must read as unmapped, or an evicted page's stale physical address
        // survives and the terrain samples a tile some other page now owns.
        m_IndirectionClearShader->Bind();
        for (u32 mip = 0; mip < mipCount; ++mip)
        {
            const u32 size = m_Config.VirtualPagesWide >> mip;
            HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, mip, /*layered*/ false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);
            const u32 groups = DivRoundUp(size, kIndirectionClearGroupSize);
            RenderCommand::DispatchCompute(groups, groups, 1);
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);

        // Pass 2 — stamp the resident pages, one dispatch per non-empty mip.
        m_IndirectionWriteShader->Bind();
        for (u32 mip = 0; mip < mipCount; ++mip)
        {
            const u32 count = mipOffsets[mip + 1u] - mipOffsets[mip];
            if (count == 0u)
            {
                continue;
            }
            // A 16-byte CPU write between two dispatches that read the same
            // buffer. GL orders it correctly (the earlier dispatch still sees
            // the old bytes), and the alternative — a bound RANGE per mip —
            // needs a glBindBufferRange the RHI facade does not expose. It is
            // affordable because the whole rebuild only runs on frames where
            // residency changed, and slice 2's delta updates replace this path
            // outright.
            VTIndirectionHeader header;
            header.m_Params = glm::uvec4(mipOffsets[mip], count, 0u, 0u);
            m_IndirectionUpdateBuffer->SetData(&header, static_cast<u32>(sizeof(header)), 0);

            HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, mip, /*layered*/ false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm, Persistent);
            RenderCommand::DispatchCompute(DivRoundUp(count, kIndirectionWriteGroupSize), 1, 1);
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);

        // Pass 3 — propagate coarse to fine. STRICTLY top-down and one dispatch
        // per level with a barrier between: level m reads level m+1's OUTPUT, so
        // batching them would let a level inherit a half-written parent.
        m_IndirectionFillShader->Bind();
        for (i32 mip = static_cast<i32>(mipCount) - 2; mip >= 0; --mip)
        {
            const u32 level = static_cast<u32>(mip);
            const u32 size = m_Config.VirtualPagesWide >> level;
            HeapBinding::BindImageOrOffset(kImageUnitTarget, indirection, level, /*layered*/ false, 0,
                                           RHI::Access::StorageReadWrite, RHI::Format::RGBA8UNorm, Persistent);
            HeapBinding::BindImageOrOffset(kImageUnitCoarser, indirection, level + 1u, /*layered*/ false, 0,
                                           RHI::Access::StorageRead, RHI::Format::RGBA8UNorm, Persistent);
            const u32 groups = DivRoundUp(size, kIndirectionFillGroupSize);
            RenderCommand::DispatchCompute(groups, groups, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
        }

        // The terrain fragment stage texelFetches this texture.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch);
        m_IndirectionDirty = false;
    }

    bool TerrainVirtualTexture::Update(const FrameInputs& inputs)
    {
        OLO_PROFILE_FUNCTION();

        m_ShadingReady = false;

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
