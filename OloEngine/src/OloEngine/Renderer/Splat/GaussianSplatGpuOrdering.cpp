#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Splat/GaussianSplatGpuOrdering.h"

#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUPrefixSum.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace OloEngine::GaussianSplat
{
    namespace
    {
        // Binding points. These sit in the engine's production SSBO range on
        // purpose-free terms: the prototype binds them itself, outside the
        // render graph, exactly as the rasteriser does, and unbinds afterwards
        // so no later pass inherits them (testing-architecture.md 6.4).
        constexpr u32 kBindingSplats = 0;
        constexpr u32 kBindingOrder = 1;
        constexpr u32 kBindingKeys = 2;
        constexpr u32 kBindingStats = 3;
        constexpr u32 kBindingIndirect = 4;
        constexpr u32 kBindingOrderOut = 5;
        constexpr u32 kBindingKeysOut = 6;
        constexpr u32 kBindingHistogram = 9;
        constexpr u32 kBindingCullUbo = 7;
        constexpr u32 kBindingSortUbo = 8;

        // The radix tile. Must equal kThreads * kElementsPerThread in
        // SplatSpike_RadixHistogram.comp and SplatSpike_RadixScatter.comp.
        //
        // 2048 RATHER THAN A SMALLER TILE FOR TWO SEPARATE REASONS, both worth
        // stating because a future reader will want to shrink it for occupancy.
        // First, the scatter's shared-memory cost is the tile (16 KiB of keys
        // and payloads plus 4 KiB of scan and bin state) while its barrier cost
        // is per tile and NOT per element, so a bigger tile amortises the eight
        // one-bit splits over more work. Second, the transposed histogram is
        // 256 * (count / kSortTile) elements and has to fit
        // GPUPrefixSum::kMaxElements; the static_assert below pins that this
        // tile size makes kMaxSplats fit, and halving the tile would break it.
        constexpr u32 kSortTile = 2048;
        constexpr u32 kRadixBins = 256;
        // Four 8-bit digits over the 32-bit key. EVEN BY REQUIREMENT, not by
        // coincidence: the scatter ping-pongs between the key/order buffers and
        // their scratch twins, so an even number of passes is what leaves the
        // result in the buffers the draw and the readback read.
        constexpr u32 kRadixPasses = 4;
        static_assert(kRadixPasses % 2 == 0, "the radix ping-pong lands in the scratch buffers on an odd pass count");

        // The bitonic control's tile: `kTile` in SplatSpike_BitonicSort.comp.
        constexpr u32 kBitonicTile = 512;
        constexpr u32 kWorkGroupSize = 256;

        constexpr u32 kStatSlots = 5; // Drawn, BehindNearPlane, FrustumCulled, TooSmall, TooFaint

        // std140. Mirrored by SplatCullUniforms in SplatSpike_Cull.comp, and a
        // PREFIX of it is what SplatSpike_Gaussian.glsl reads as
        // SplatViewUniforms -- the first three members are byte-identical, so
        // one buffer feeds both and the draw cannot disagree with the cull
        // about which camera it is.
        struct CullUniforms
        {
            glm::mat4 View{ 1.0f };
            glm::mat4 Projection{ 1.0f };
            glm::vec4 ViewportFocal{ 0.0f };
            std::array<glm::vec4, 6> Planes{};
            glm::vec4 CullParams{ 0.0f };
            glm::uvec4 Counts{ 0u };
        };
        static_assert(sizeof(CullUniforms) == 272);

        // One buffer serves both sorts, because they need the same number of
        // words and never run in the same dispatch. Radix: (count, shift,
        // numTiles, unused). Bitonic: (count, k, j, localMode).
        struct SortUniforms
        {
            glm::uvec4 SortParams{ 0u };
        };

        // Gribb/Hartmann, normalised, normals pointing into the frustum -- the
        // same extraction GaussianSplatView.cpp does, kept textually close so
        // the parity test is testing the dispatch and not two different maths.
        [[nodiscard]] std::array<glm::vec4, 6> ExtractPlanes(const glm::mat4& viewProjection)
        {
            const glm::mat4& m = viewProjection;
            const auto row = [&m](int i)
            { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
            const glm::vec4 r3 = row(3);
            const std::array<glm::vec4, 6> raw{ r3 + row(0), r3 - row(0), r3 + row(1),
                                                r3 - row(1), r3 + row(2), r3 - row(2) };
            std::array<glm::vec4, 6> planes{};
            for (sizet i = 0; i < raw.size(); ++i)
            {
                const glm::vec3 n(raw[i]);
                const f32 length = glm::length(n);
                if (length > 1e-8f)
                    planes[i] = raw[i] / length;
                else
                    planes[i] = glm::vec4(0.0f, 0.0f, 1.0f, std::numeric_limits<f32>::max());
            }
            return planes;
        }
    } // namespace

    // The radix's own ceiling, checked here rather than trusted. The scan has to
    // cover one entry per (bin, tile), and GPUPrefixSum caps a single scan at
    // kMaxElements; at kSortTile = 2048 the largest cloud this pass accepts
    // needs 8,388,608 of them against a limit of 16,776,960. Shrink kSortTile
    // and this stops holding before anything else does.
    static_assert(static_cast<u64>(kRadixBins) * ((GpuViewOrdering::kMaxSplats + kSortTile - 1) / kSortTile) <=
                      static_cast<u64>(GPUPrefixSum::kMaxElements),
                  "the transposed radix histogram at kMaxSplats does not fit one GPUPrefixSum scan");

    auto GpuViewOrdering::PaddedCapacityFor(u32 count) -> u32
    {
        OLO_CORE_ASSERT(count <= kMaxSplats, "GpuViewOrdering::PaddedCapacityFor above kMaxSplats");
        const u32 clamped = std::min(count, kMaxSplats);
        const u32 tiles = std::max(1u, (clamped + kSortTile - 1) / kSortTile);
        return tiles * kSortTile;
    }

    auto GpuViewOrdering::BitonicPaddedCapacityFor(u32 count) -> u32
    {
        OLO_CORE_ASSERT(count <= kMaxSplats, "GpuViewOrdering::BitonicPaddedCapacityFor above kMaxSplats");
        const u32 atLeastATile = std::max(std::min(count, kMaxSplats), kBitonicTile);
        return std::bit_ceil(atLeastATile);
    }

    GpuViewOrdering::~GpuViewOrdering()
    {
        ReleaseBindings();
    }

    void GpuViewOrdering::ReleaseBindings() const
    {
        // Leave no buffer of ours on a shared binding point for whatever runs
        // next in this process (testing-architecture.md 6.4).
        for (const Ref<StorageBuffer>& buffer : { m_SplatBuffer, m_OrderBuffer, m_KeyBuffer, m_OrderScratch,
                                                  m_KeyScratch, m_HistogramBuffer, m_StatsBuffer, m_IndirectBuffer })
        {
            if (buffer)
                buffer->Unbind();
        }

        // The UBOs matter MORE than the SSBOs here and were missed the first
        // time. Bindings 7 and 8 are UBO_USER_0 and UBO_USER_1 -- the
        // post-process and motion-blur slots -- so leaving a destroyed buffer
        // on them hands the next pass in the process a dangling binding.
        if (m_CullUniforms)
            m_CullUniforms->Unbind();
        if (m_SortUniforms)
            m_SortUniforms->Unbind();
    }

    auto GpuViewOrdering::Initialize() -> bool
    {
        OLO_PROFILE_FUNCTION();

        m_CullShader = ComputeShader::Create("assets/shaders/tests/SplatSpike_Cull.comp");
        m_HistogramShader = ComputeShader::Create("assets/shaders/tests/SplatSpike_RadixHistogram.comp");
        m_ScatterShader = ComputeShader::Create("assets/shaders/tests/SplatSpike_RadixScatter.comp");
        m_Ready = m_CullShader && m_CullShader->IsValid() && m_HistogramShader && m_HistogramShader->IsValid() &&
                  m_ScatterShader && m_ScatterShader->IsValid();

        if (m_Ready)
        {
            // The scan is consumed, not owned: #713 already tests it, and a
            // second device-level scan in the engine would be a second thing to
            // keep correct for no benefit.
            m_PrefixSum = Ref<GPUPrefixSum>::Create();
            m_PrefixSum->EnsureInitialised();
            m_Ready = m_PrefixSum->IsAvailable();
            if (!m_Ready)
                OLO_CORE_ERROR("GpuViewOrdering::Initialize: GPUPrefixSum is unavailable, so the radix sort has no scan");
        }

        if (m_Ready)
        {
            m_CullUniforms = UniformBuffer::Create(static_cast<u32>(sizeof(CullUniforms)), kBindingCullUbo);
            m_SortUniforms = UniformBuffer::Create(static_cast<u32>(sizeof(SortUniforms)), kBindingSortUbo);
            m_Ready = m_CullUniforms && m_SortUniforms;
        }
        return m_Ready;
    }

    auto GpuViewOrdering::SetSortAlgorithm(SortAlgorithm algorithm) -> bool
    {
        OLO_CORE_ASSERT(m_Ready, "GpuViewOrdering::SetSortAlgorithm before a successful Initialize");

        if (algorithm == SortAlgorithm::Bitonic && !m_BitonicShader)
        {
            m_BitonicShader = ComputeShader::Create("assets/shaders/tests/SplatSpike_BitonicSort.comp");
            if (!m_BitonicShader || !m_BitonicShader->IsValid())
            {
                OLO_CORE_ERROR("GpuViewOrdering: SplatSpike_BitonicSort.comp failed to compile; the A/B control is unavailable");
                m_BitonicShader = nullptr;
                return false;
            }
        }

        if (algorithm != m_Algorithm)
        {
            m_Algorithm = algorithm;
            // The two sorts pad differently, so every buffer sized from the
            // padding has to follow. The splat records do not, which is the
            // whole reason this is not just `SetCloud` again: an A/B that
            // re-uploaded 96 MB of records between samples would be measuring
            // the upload.
            if (m_SplatBuffer)
                ResizeSortBuffers();
        }
        return true;
    }

    void GpuViewOrdering::ResetIndirectCommand()
    {
        // { count = 6 vertices, instanceCount = 0, first = 0, baseInstance = 0 }.
        // instanceCount is the only field the GPU writes.
        //
        // SIX VERTICES, NOT A FOUR-VERTEX STRIP. The quad is two triangles
        // because RendererAPI::DrawArraysIndirect draws GL_TRIANGLES, and
        // adapting the shader to the facade is the right way round: reaching
        // for a raw glDrawArraysIndirect to keep a strip is what the RHI
        // boundary ratchet exists to stop (issue #691, ADR 0011).
        //
        // CALLED WHENEVER THE ORDER BUFFER IS INVALIDATED, not only per frame.
        // A fresh StorageBuffer is uninitialised, so `SetCloud` followed
        // straight by `DrawIndirect` used to read an instance count that was
        // whatever the driver handed back, and `SetSortAlgorithm` reallocates
        // the order buffer under a command that still holds the LAST frame's
        // count. Zeroing it here makes the failure mode "draws nothing", which
        // is visible, instead of "draws the right number of garbage indices",
        // which renders a plausible frame.
        if (!m_IndirectBuffer)
            return;
        const std::array<u32, 4> indirect{ 6u, 0u, 0u, 0u };
        m_IndirectBuffer->SetData(indirect.data(), static_cast<u32>(indirect.size() * sizeof(u32)));
    }

    void GpuViewOrdering::ResizeSortBuffers()
    {
        m_PaddedCapacity = (m_Algorithm == SortAlgorithm::Radix) ? PaddedCapacityFor(m_SplatCount)
                                                                 : BitonicPaddedCapacityFor(m_SplatCount);

        const u32 slotBytes = m_PaddedCapacity * static_cast<u32>(sizeof(u32));
        m_OrderBuffer = StorageBuffer::Create(slotBytes, kBindingOrder, StorageBufferUsage::DynamicCopy);
        m_KeyBuffer = StorageBuffer::Create(slotBytes, kBindingKeys, StorageBufferUsage::DynamicCopy);

        if (m_Algorithm == SortAlgorithm::Radix)
        {
            m_OrderScratch = StorageBuffer::Create(slotBytes, kBindingOrderOut, StorageBufferUsage::DynamicCopy);
            m_KeyScratch = StorageBuffer::Create(slotBytes, kBindingKeysOut, StorageBufferUsage::DynamicCopy);
            const u32 histogramBytes =
                kRadixBins * (m_PaddedCapacity / kSortTile) * static_cast<u32>(sizeof(u32));
            m_HistogramBuffer =
                StorageBuffer::Create(histogramBytes, kBindingHistogram, StorageBufferUsage::DynamicCopy);
        }
        else
        {
            // Released rather than kept: the bitonic control is only ever the
            // second half of an A/B, and 3 x 16 MiB of idle scratch during it
            // would sit on the very budget the measurement is about.
            m_OrderScratch = nullptr;
            m_KeyScratch = nullptr;
            m_HistogramBuffer = nullptr;
        }

        // The order buffer just became a fresh, uninitialised allocation, so
        // whatever instance count the indirect command still carries now points
        // into it. See ResetIndirectCommand.
        ResetIndirectCommand();
    }

    void GpuViewOrdering::SetCloud(const SplatCloud& cloud)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(m_Ready, "GpuViewOrdering::SetCloud before a successful Initialize");

        // REJECTED BEFORE ANYTHING IS NARROWED. Every size below is a u32 and
        // each overflows at a different count (see kMaxSplats); a cloud past
        // the ceiling would otherwise allocate a wrapped buffer, or hang the
        // sort loop, rather than fail.
        if (cloud.Count() > kMaxSplats)
        {
            OLO_CORE_ERROR("GpuViewOrdering::SetCloud refused a cloud of {} splats; the ceiling is {}",
                           cloud.Count(), kMaxSplats);
            m_SplatCount = 0;
            m_PaddedCapacity = 0;
            m_SplatBuffer = nullptr;
            m_OrderBuffer = nullptr;
            m_KeyBuffer = nullptr;
            m_OrderScratch = nullptr;
            m_KeyScratch = nullptr;
            m_HistogramBuffer = nullptr;
            m_StatsBuffer = nullptr;
            m_IndirectBuffer = nullptr;
            return;
        }

        m_SplatCount = cloud.Count();

        const u32 splatBytes = std::max<u32>(static_cast<u32>(cloud.GpuBytes()), sizeof(GpuSplat));
        m_SplatBuffer = StorageBuffer::Create(splatBytes, kBindingSplats, StorageBufferUsage::DynamicDraw);
        if (m_SplatCount > 0)
            m_SplatBuffer->SetData(cloud.Splats().data(), static_cast<u32>(cloud.GpuBytes()));

        ResizeSortBuffers();

        m_StatsBuffer =
            StorageBuffer::Create(kStatSlots * static_cast<u32>(sizeof(u32)), kBindingStats, StorageBufferUsage::DynamicCopy);
        m_IndirectBuffer =
            StorageBuffer::Create(4 * static_cast<u32>(sizeof(u32)), kBindingIndirect, StorageBufferUsage::DynamicCopy);
        ResetIndirectCommand();
    }

    void GpuViewOrdering::BuildOrdering(const glm::mat4& view,
                                        const glm::mat4& projection,
                                        const glm::vec2& viewportPixels,
                                        const ViewSettings& settings)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(m_Ready && m_SplatBuffer, "GpuViewOrdering::BuildOrdering before SetCloud");

        m_Dispatches = DispatchCounts{};
        if (!m_SplatBuffer)
            return; // SetCloud refused the cloud; nothing to order.

        // THE BUDGET IS NOT IMPLEMENTED HERE, AND THAT IS DELIBERATE -- see
        // ADR 0018 section 3.4: selecting the top N splats destroys the diffuse
        // mass, and LOD level selection replaces it. Silently ignoring the field
        // would make the CPU and GPU paths disagree exactly where the header
        // promises they cannot, so a caller that sets a budget this pass would
        // actually change is told.
        if (settings.MaxSplats > 0 && settings.MaxSplats < m_SplatCount)
        {
            OLO_CORE_WARN("GpuViewOrdering: ViewSettings::MaxSplats ({}) is ignored by the GPU ordering pass; "
                          "pick a SplatLodChain level instead (ADR 0018 section 3.4)",
                          settings.MaxSplats);
        }

        CullUniforms uniforms;
        uniforms.View = view;
        uniforms.Projection = projection;
        uniforms.ViewportFocal = glm::vec4(viewportPixels.x, viewportPixels.y,
                                           0.5f * viewportPixels.x * projection[0][0],
                                           0.5f * viewportPixels.y * std::abs(projection[1][1]));
        uniforms.Planes = ExtractPlanes(projection * view);
        uniforms.CullParams =
            glm::vec4(0.0f, settings.NearClip, settings.MinScreenExtentPixels, settings.MinAlpha);
        uniforms.Counts = glm::uvec4(m_SplatCount, m_PaddedCapacity, 0u, 0u);
        m_CullUniforms->SetData(&uniforms, static_cast<u32>(sizeof(uniforms)));
        // BOUND EXPLICITLY, not just written. A UniformBuffer claims its
        // binding point when it is CREATED, so any other buffer created later
        // on the same point owns it and this one is written into a slot nothing
        // reads. That is not hypothetical: the evidence test owns its own
        // camera UBO on binding 7, and without this the GPU-ordered frame drew
        // through whichever buffer happened to be constructed last.
        m_CullUniforms->Bind();

        m_StatsBuffer->ClearData();

        ResetIndirectCommand();

        m_SplatBuffer->Bind();
        m_OrderBuffer->Bind();
        m_KeyBuffer->Bind();
        m_StatsBuffer->Bind();
        m_IndirectBuffer->Bind();

        m_CullShader->Bind();
        const u32 cullGroups = (m_PaddedCapacity + kWorkGroupSize - 1) / kWorkGroupSize;
        RenderCommand::DispatchCompute(cullGroups, 1, 1);
        m_Dispatches.Cull = 1;
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        if (m_Algorithm == SortAlgorithm::Radix)
            SortRadix();
        else
            SortBitonic();

        // The order buffer is about to be read by a vertex shader and the
        // indirect buffer by the command processor, neither of which the
        // storage barrier above covers.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
    }

    void GpuViewOrdering::SortRadix()
    {
        OLO_PROFILE_FUNCTION();

        // Four 8-bit LSD passes over the complemented depth key, ping-ponging
        // between the live buffers and their scratch twins. Each pass is a tile
        // histogram, one GPUPrefixSum exclusive scan of it, and a scatter.
        //
        // NO PASS IS SKIPPED AS UNIFORM, which the CPU reference does do. It
        // can: it has already read the histogram. Here the histogram only ever
        // exists on the device, so "is this digit constant?" costs the readback
        // this whole class is built to avoid -- and a fixed four passes is a
        // fixed cost, which is the property the issue is about.
        const u32 numTiles = m_PaddedCapacity / kSortTile;
        OLO_CORE_ASSERT(numTiles * kSortTile == m_PaddedCapacity,
                        "the radix sort needs a whole number of tiles; PaddedCapacityFor guarantees it");

        Ref<StorageBuffer> sourceKeys = m_KeyBuffer;
        Ref<StorageBuffer> sourceOrder = m_OrderBuffer;
        Ref<StorageBuffer> destinationKeys = m_KeyScratch;
        Ref<StorageBuffer> destinationOrder = m_OrderScratch;
        bool scanned = true;

        for (u32 pass = 0; pass < kRadixPasses && scanned; ++pass)
        {
            SortUniforms sortUniforms;
            sortUniforms.SortParams = glm::uvec4(m_PaddedCapacity, pass * 8u, numTiles, 0u);
            m_SortUniforms->SetData(&sortUniforms, static_cast<u32>(sizeof(sortUniforms)));
            m_SortUniforms->Bind();

            // Bound by handle rather than by `Bind()`, because which OBJECT
            // plays "source" swaps every pass while the shaders' binding points
            // do not.
            RenderCommand::BindStorageBuffer(kBindingKeys, sourceKeys->GetRHIHandle());
            RenderCommand::BindStorageBuffer(kBindingOrder, sourceOrder->GetRHIHandle());
            RenderCommand::BindStorageBuffer(kBindingKeysOut, destinationKeys->GetRHIHandle());
            RenderCommand::BindStorageBuffer(kBindingOrderOut, destinationOrder->GetRHIHandle());
            RenderCommand::BindStorageBuffer(kBindingHistogram, m_HistogramBuffer->GetRHIHandle());

            m_HistogramShader->Bind();
            RenderCommand::DispatchCompute(numTiles, 1, 1);
            ++m_Dispatches.RadixHistogram;
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            // Scans in place and emits its own barriers. It binds and releases
            // SSBOs 54-56 and UBO 79, none of which this pass uses, so the five
            // bindings above survive it -- but it leaves ITS shader bound, so
            // the scatter has to rebind.
            //
            // A REFUSED SCAN IS NOT SURVIVABLE, so it stops the sort rather
            // than letting three more passes run on an unscanned histogram.
            // That would leave every tile's offsets equal to its raw bin count,
            // and the scatter would pile the whole cloud on top of itself -- a
            // frame that draws and is wrong, which is the failure mode this
            // pass is hardest to debug from.
            scanned = m_PrefixSum->ExclusiveScanInPlace(m_HistogramBuffer, kRadixBins * numTiles);
            if (!scanned)
            {
                OLO_CORE_ERROR("GpuViewOrdering: the radix histogram scan was refused at pass {} of {}; "
                               "the draw order is undefined for this frame",
                               pass, kRadixPasses);
                continue; // fall out of the loop, but through the rebinding below
            }
            ++m_Dispatches.RadixScanCalls;

            m_ScatterShader->Bind();
            RenderCommand::DispatchCompute(numTiles, 1, 1);
            ++m_Dispatches.RadixScatter;
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            std::swap(sourceKeys, destinationKeys);
            std::swap(sourceOrder, destinationOrder);
        }

        OLO_CORE_ASSERT(!scanned || sourceOrder == m_OrderBuffer,
                        "the radix ping-pong did not land back in the order buffer");

        // Put the live buffers back on their own binding points and take the
        // scratch off theirs. Without this the draw would read binding 1 and
        // find the scratch order buffer sitting on it, which is the previous
        // pass's data and renders a plausible, wrong frame.
        m_OrderBuffer->Bind();
        m_KeyBuffer->Bind();
        RenderCommand::BindStorageBuffer(kBindingKeysOut, {});
        RenderCommand::BindStorageBuffer(kBindingOrderOut, {});
        RenderCommand::BindStorageBuffer(kBindingHistogram, {});
    }

    void GpuViewOrdering::SortBitonic()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(m_BitonicShader, "GpuViewOrdering::SortBitonic without the control shader");

        // The A/B control, kept from #1038. Every step whose stride is at least
        // the tile width has to go through global memory; the rest of that k's
        // steps are finished inside one workgroup, which is what keeps the
        // dispatch count at 2 log N instead of log^2 N.
        m_BitonicShader->Bind();
        const u32 localTiles = m_PaddedCapacity / kBitonicTile;
        for (u32 k = 2; k <= m_PaddedCapacity; k <<= 1)
        {
            u32 j = k >> 1;
            for (; j > (kBitonicTile >> 1); j >>= 1)
            {
                SortUniforms sortUniforms;
                sortUniforms.SortParams = glm::uvec4(m_PaddedCapacity, k, j, 0u);
                m_SortUniforms->SetData(&sortUniforms, static_cast<u32>(sizeof(sortUniforms)));
                m_SortUniforms->Bind();

                const u32 pairs = m_PaddedCapacity >> 1;
                RenderCommand::DispatchCompute((pairs + kWorkGroupSize - 1) / kWorkGroupSize, 1, 1);
                ++m_Dispatches.SortGlobal;
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }

            SortUniforms sortUniforms;
            sortUniforms.SortParams = glm::uvec4(m_PaddedCapacity, k, j, 1u);
            m_SortUniforms->SetData(&sortUniforms, static_cast<u32>(sizeof(sortUniforms)));
            m_SortUniforms->Bind();
            RenderCommand::DispatchCompute(localTiles, 1, 1);
            ++m_Dispatches.SortLocal;
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
        }
    }

    void GpuViewOrdering::DrawIndirect()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(m_Ready && m_IndirectBuffer, "GpuViewOrdering::DrawIndirect before BuildOrdering");

        // A draw needs SOME vertex array bound even when every vertex is
        // synthesised from gl_VertexIndex, so the prototype keeps an empty one
        // of its own rather than borrowing the caller's.
        if (!m_EmptyVertexArray)
            m_EmptyVertexArray = VertexArray::Create();

        m_SplatBuffer->Bind();
        m_OrderBuffer->Bind();
        // The draw shader reads the first three members of this same buffer as
        // its camera, so binding it here is what stops the draw and the cull
        // disagreeing about the view.
        m_CullUniforms->Bind();
        RenderCommand::DrawArraysIndirect(m_EmptyVertexArray, m_IndirectBuffer->GetRHIHandle());
    }

    void GpuViewOrdering::ReadbackOrdering(ViewOrdering& out) const
    {
        OLO_PROFILE_FUNCTION();

        out.Indices.clear();
        out.Stats = ViewStats{};
        if (!m_Ready || !m_OrderBuffer)
            return;

        // BufferUpdate, not just ShaderStorage. `GetData` reads through
        // glGetNamedBufferSubData, and GL only orders shader writes against a
        // client-side read behind GL_BUFFER_UPDATE_BARRIER_BIT; the frame path
        // does not need it, so it is issued here rather than in BuildOrdering.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        std::array<u32, kStatSlots> stats{};
        m_StatsBuffer->GetData(stats.data(), static_cast<u32>(stats.size() * sizeof(u32)));

        out.Stats.Total = m_SplatCount;
        out.Stats.Drawn = stats[0];
        out.Stats.BehindNearPlane = stats[1];
        out.Stats.FrustumCulled = stats[2];
        out.Stats.TooSmall = stats[3];
        out.Stats.TooFaint = stats[4];

        std::vector<u32> padded(m_PaddedCapacity);
        m_OrderBuffer->GetData(padded.data(), m_PaddedCapacity * static_cast<u32>(sizeof(u32)));

        // Survivors occupy the front of the sorted array; everything the cull
        // rejected carries the maximum key and sorted behind them.
        const u32 drawn = std::min(out.Stats.Drawn, m_PaddedCapacity);
        out.Indices.assign(padded.begin(), padded.begin() + static_cast<std::ptrdiff_t>(drawn));
    }
} // namespace OloEngine::GaussianSplat
