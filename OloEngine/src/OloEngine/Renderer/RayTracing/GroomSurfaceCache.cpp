#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/GroomSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/GroomProxyDiagnostics.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>

namespace OloEngine::RayTracing
{
    namespace GroomProxyDiagnostics
    {
        namespace
        {
            // Render-thread-only, and deliberately a plain global rather
            // than anything atomic: this whole subsystem is render-thread
            // only, and an atomic here would imply a second writer that
            // does not exist.
            //
            // SEEDED FROM THE ENVIRONMENT ONCE, so the A/B is also runnable
            // across two launches when clicking the panel is not available —
            // an unattended session drives the editor through MCP, and the
            // statistics dock is not always reachable at a high DPI scale.
            // Read once rather than per frame: a lever that could change
            // under a running comparison is not a control.
            bool ReadDisabledFromEnvironment()
            {
                const char* value = std::getenv("OLO_GROOM_RT_PROXIES");
                return value != nullptr && value[0] == '0' && value[1] == '\0';
            }
            bool s_Disabled = ReadDisabledFromEnvironment();
            std::optional<GroomProxyTier> s_ForcedTier;
        } // namespace

        void SetDisabled(bool disabled)
        {
            s_Disabled = disabled;
        }
        bool GetDisabled()
        {
            return s_Disabled;
        }
        void SetForcedTier(std::optional<GroomProxyTier> tier)
        {
            s_ForcedTier = tier;
        }
        std::optional<GroomProxyTier> GetForcedTier()
        {
            return s_ForcedTier;
        }
    } // namespace GroomProxyDiagnostics

    namespace
    {
        struct Hash
        {
            u64 Value = 1469598103934665603ull;
            template<typename T>
            void Mix(const T& value)
            {
                for (const std::byte byte : std::as_bytes(std::span(&value, 1u)))
                {
                    Value ^= std::to_integer<u8>(byte);
                    Value *= 1099511628211ull;
                }
            }
        };

        // The same predicate GroomRenderPass::IsDeformed applies, and the same
        // reason for the length test rather than a non-empty test: the build
        // indexes the transform array by curve, so a short array reads past its
        // end on the first strand past the boundary.
        [[nodiscard]] bool IsDeformed(const GroomStrandRequest& request) noexcept
        {
            return request.Groom && request.Binding &&
                   static_cast<sizet>(request.RootTransforms.Num()) == request.Groom->GetCurveCount();
        }

        [[nodiscard]] bool IsFiniteTransform(const glm::mat4& matrix) noexcept
        {
            for (u32 column = 0; column < 4u; ++column)
            {
                for (u32 row = 0; row < 4u; ++row)
                {
                    if (!std::isfinite(matrix[column][row]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace

    GroomSurfaceCache::GroomSurfaceCache() = default;
    GroomSurfaceCache::~GroomSurfaceCache() = default;

    void GroomSurfaceCache::SetEnabled(bool enabled)
    {
        if (m_Enabled && !enabled)
        {
            Shutdown();
        }
        m_Enabled = enabled;
    }

    u64 GroomSurfaceCache::GetCpuBytes() const
    {
        u64 bytes = static_cast<u64>(m_StrandVertices.capacity()) * sizeof(GroomStrandVertex) +
                    static_cast<u64>(m_ProxySegments.capacity()) * sizeof(GroomProxySegment) +
                    static_cast<u64>(m_StrandIndices.capacity()) * sizeof(u32) +
                    static_cast<u64>(m_ProxyVertices.capacity()) * sizeof(Vertex) +
                    static_cast<u64>(m_ProxyIndices.capacity()) * sizeof(u32) +
                    static_cast<u64>(m_RootScratch.GetAllocatedSize());
        for (const auto& [key, entry] : m_Entries)
        {
            static_cast<void>(key);
            bytes += static_cast<u64>(entry.Rest.capacity()) * sizeof(GroomStrandVertex) +
                     static_cast<u64>(entry.RootCurves.capacity()) * sizeof(u32) + entry.DeformCpu.GetCpuBytes();
        }
        return bytes;
    }

    void GroomSurfaceCache::Shutdown()
    {
        m_Entries.clear();
        m_TierState.clear();
        m_StrandVertices.clear();
        m_StrandVertices.shrink_to_fit();
        m_ProxySegments.clear();
        m_ProxySegments.shrink_to_fit();
        m_StrandIndices.clear();
        m_StrandIndices.shrink_to_fit();
        m_ProxyVertices.clear();
        m_ProxyVertices.shrink_to_fit();
        m_ProxyIndices.clear();
        m_ProxyIndices.shrink_to_fit();
        m_RootScratch.Reset();
        m_SkinScratch.Release();
        m_GpuJobs.Reset();
        m_GpuShader.Reset();
        m_GpuParams.Reset();
        m_GpuShaderFailed = false;
        m_Stats.Reset();
        m_Enabled = false;
    }

    namespace
    {
        // std140 twin of GroomProxyDeformToBuffer.comp's RayTracingGroomProxyParams.
        struct GpuProxyParams
        {
            glm::uvec2 Rest{ 0u };
            glm::uvec2 Output{ 0u };
            u32 SegmentCount = 0u;
            u32 Ribbons = 0u;
            f32 WidthScale = 1.0f;
            u32 Padding = 0u;
            glm::ivec4 DeformModes{ 0 };
            glm::ivec4 DeformBases{ 0 };
        };
        static_assert(sizeof(GpuProxyParams) == 64u);

        glm::uvec2 SplitAddress(u64 address)
        {
            return { static_cast<u32>(address), static_cast<u32>(address >> 32u) };
        }
    } // namespace

    bool GroomSurfaceCache::GpuBuildAvailable()
    {
        if (RendererAPI::GetAPI() != RendererAPI::API::Vulkan || Levers::GroomProxyOnCpu() || m_GpuShaderFailed)
            return false;
        if (!m_GpuShader)
        {
            m_GpuShader = ComputeShader::Create("assets/shaders/GroomProxyDeformToBuffer.comp");
            if (!m_GpuShader || !m_GpuShader->IsValid())
            {
                // Said once: the CPU build stays, the shader's own log names why.
                OLO_CORE_WARN("[RayTracing] GroomProxyDeformToBuffer.comp did not compile; coat proxies are built on "
                              "the CPU");
                m_GpuShader.Reset();
                m_GpuShaderFailed = true;
                return false;
            }
        }
        if (!m_GpuParams)
            m_GpuParams = UniformBuffer::Create(sizeof(GpuProxyParams), ShaderBindingLayout::UBO_RAY_TRACING);
        return m_GpuParams != nullptr;
    }

    GroomProxyRefusalReason GroomSurfaceCache::Refresh(Entry& entry, const GroomStrandRequest& request,
                                                       const GroomProxyDecision& decision, u64 shapeHash,
                                                       GroomProxyFrameBudget& budget)
    {
        // ── The one cap that can be tested before doing the work ─────────
        //
        // The vertex and triangle caps need a real size, which only the
        // conversion knows. The UPDATE cap does not — so it is tested here,
        // before a strand build and a ribbon conversion are paid for and
        // thrown away. Without it a scene past the budget burns a full
        // rebuild per coat per frame for nothing.
        if (budget.Exhausted())
        {
            return GroomProxyRefusalReason::BudgetExhausted;
        }

        // ── The strand set this tier asks for ────────────────────────────
        //
        // The request's OWN budget is the ceiling: a coat the entity already
        // thinned to 500 strands must not be traced at 6144 of them. The tier
        // lowers it and never raises it, which is the "coarser of the two wins"
        // rule #1258 applies to the LOD budgets, for the same reason.
        GroomStrandBuildSettings build = request.Build;
        build.MaxStrands = std::min(build.MaxStrands, decision.StrandBudget);
        // COOKED radii, as the conversion below assumes: the raster build
        // widens each thinned role in the stream (#1428), but the proxy's own
        // compensation is taken on this build's achieved fraction, which already
        // counts the LOD's thinning as well as the tier's. Leaving the raster cap
        // in would widen every thinned strand twice.
        build.MaxWidthCompensation = 1.0f;

        const bool deformed = IsDeformed(request);
        const GroomCoatContext coat{ &request.Coat, request.Groom->GetGroupCoats() };
        const GroomStrandSimulation simulation = request.Simulation();
        const u32 baseCurveCount = request.Groom->GetCurveCount();
        const bool simulated = simulation.IsUsable(baseCurveCount);

        GroomStrandMeshStats strandStats;
        // A bound coat builds on the GPU where it can (#1533); its frame is then
        // packed into a GPU-layout buffer the compute pass reads.
        const bool gpuBuild = deformed && request.Binding && GpuBuildAvailable();
        if (deformed && request.Binding)
        {
            // A BOUND COAT REFITS FROM ITS REST STREAM (#1533), the raster pass's
            // split: the root-local stream depends on nothing that moves, so it is
            // built when its shape changes, and a frame evaluates this stream's
            // roots alone, packs them with the guides and deforms the stream on
            // the CPU as GroomStrand.glsl deforms it on the GPU -- bit for bit the
            // points a rebuild produces (DeformGroomRestStream). The rebuild it
            // replaces walked every curve of the groom several times a frame:
            // ~90 ms on the showcase dog, and the RT shadow tier ran at 14 fps.
            if (entry.RestHash != shapeHash || entry.Rest.empty() || entry.RestGroom != request.Groom ||
                entry.RestBinding != request.Binding || entry.RestLevel != request.LodLevel)
            {
                OLO_PERF_SCOPE_AUTO("GroomProxy::RestBuild");
                std::vector<u32> restIndices; // the conversion reads corners, never indices
                entry.RestStats = BuildGroomStrandRestMesh(request.BuildSource(), build, *request.Binding, entry.Rest,
                                                           restIndices, entry.RootCurves, &coat);
                entry.RestHash = shapeHash;
                entry.RestGroom = request.Groom;
                entry.RestBinding = request.Binding;
                entry.RestLevel = request.LodLevel;
                entry.DeformRelayout = true;
            }
            if (entry.Rest.empty() || entry.RestStats.StrandsSelected == 0u)
            {
                return GroomProxyRefusalReason::GroomHasNoGeometry;
            }

            OLO_PERF_SCOPE_AUTO("GroomProxy::Deform");
            // Sized for the table it was laid out against, as the raster pass's
            // frame buffer is: a coat that starts being simulated, switches table
            // or outgrows the capacity gets a new layout.
            const u32 rootCount = static_cast<u32>(entry.RootCurves.size());
            const u32 displacements =
                simulated ? static_cast<u32>(simulation.Displacements.Displacements.size()) : 0u;
            const Ref<GroomGuideInfluenceTable> weightsFrom =
                simulated ? request.Influence : Ref<GroomGuideInfluenceTable>{};
            const GroomDeformBufferLayout& layout = entry.DeformCpu.GetLayout();
            if (entry.DeformRelayout || layout.RootCount != rootCount || weightsFrom != entry.DeformWeightsFrom ||
                displacements > layout.DisplacementCapacity || entry.DeformCpuOnly == gpuBuild)
            {
                const GroomGuideInfluenceTable* table = simulated ? simulation.Influence : nullptr;
                const u32 capacity = std::max(GroomDeformDisplacementCapacity(*request.Groom, table), displacements);
                // CPU-only for the CPU build (#1533): it reads the records, and no
                // GPU sees an image. The GPU build sends the image.
                entry.DeformCpu.Reset(
                    GroomDeformBufferLayout::Make(rootCount, table != nullptr ? table->GetGuideCount() : 0u, capacity),
                    entry.RootCurves, table, /*cpuOnly*/ !gpuBuild);
                entry.DeformCpuOnly = !gpuBuild;
                entry.DeformGpu.Reset(); // sized to the new layout below
                entry.DeformWeightsFrom = weightsFrom;
                entry.DeformRelayout = false;
            }
            // This stream's roots only: the producer left the drawn ones to the
            // raster pass's GPU evaluation (#1533 E1).
            std::span<const GroomRootTransform> roots;
            {
                OLO_PERF_SCOPE_AUTO("GroomProxy::Roots");
                roots = GroomCpuRootTransforms(request, std::span<const u32>{ entry.RootCurves }, m_RootScratch,
                                               m_SkinScratch, /*onlyCurvesDefined*/ true);
            }
            {
                OLO_PERF_SCOPE_AUTO("GroomProxy::Pack");
                (void)entry.DeformCpu.PackFrame(entry.RootCurves, *request.Binding, roots,
                                                simulated ? &simulation : nullptr, baseCurveCount);
            }
            if (!gpuBuild)
            {
                // The two points a segment's ribbons need, not DeformGroomRestStream's
                // twelve (#1533).
                OLO_PERF_SCOPE_AUTO("GroomProxy::DeformStream");
                DeformGroomRestStreamSegments(entry.DeformCpu, entry.Rest, m_ProxySegments);
            }
            strandStats = entry.RestStats;
        }
        else
        {
            OLO_PERF_SCOPE_AUTO("GroomProxy::StrandBuild");
            strandStats = BuildGroomStrandMesh(request.BuildSource(), build, m_StrandVertices, m_StrandIndices, nullptr,
                                               &coat, simulated ? &simulation : nullptr);
            if (m_StrandVertices.empty() || strandStats.StrandsSelected == 0u)
            {
                return GroomProxyRefusalReason::GroomHasNoGeometry;
            }
        }

        // ── The compensation, on the ACHIEVED fraction ───────────────────
        //
        // StrandsSelected / StrandsAvailable, never the fraction the tier asked
        // for: the budget is spent as an integer stride, so asking for 0.4 of
        // the curves retains 1/3 of them. See GroomProxyWidthCompensation, which
        // carries that warning, and GroomLod.h, which explains why the whole
        // engine compensates on the achieved number.
        const f32 achieved = strandStats.StrandsAvailable > 0u
                                 ? static_cast<f32>(strandStats.StrandsSelected) /
                                       static_cast<f32>(strandStats.StrandsAvailable)
                                 : 1.0f;
        const f32 compensation =
            GroomProxyWidthCompensation(achieved, GroomProxyPolicy::MaxWidthCompensation);

        GroomProxyConversionSettings conversion;
        // BOTH FACTORS, exactly as the raster draw composes them.
        // BuildGroomStrandMesh emits the COOKED radii here (the cap is 1 above)
        // and applies neither: the raster path carries its LOD compensation in
        // the stream and `request.WidthScale` in the shader, so a
        // proxy that applied only the compensation would be thinner in ray
        // space than on screen for every groom exported at another unit
        // scale — a coat whose shadow is too light with nothing to say why.
        //
        // The two compensations are INDEPENDENT and both belong here: the
        // raster one puts back what the LOD's thinning removed, this one
        // puts back what the PROXY's thinning removed, and the proxy is
        // built from the strand set the LOD budget already thinned.
        conversion.WidthScale = std::isfinite(request.WidthScale) && request.WidthScale > 0.0f
                                    ? request.WidthScale * compensation
                                    : compensation;
        if (gpuBuild)
        {
            return RefreshOnGpu(entry, shapeHash, compensation, conversion, budget);
        }

        GroomProxyMeshStats proxyStats;
        {
            OLO_PERF_SCOPE_AUTO("GroomProxy::Convert");
            proxyStats = deformed && request.Binding
                             ? ConvertGroomProxySegments(m_ProxySegments, conversion, m_ProxyVertices, m_ProxyIndices)
                             : ConvertGroomStrandMeshToProxy(m_StrandVertices, conversion, m_ProxyVertices, m_ProxyIndices);
        }
        if (proxyStats.VertexCount == 0u || proxyStats.IndexCount == 0u)
        {
            return GroomProxyRefusalReason::BuildFailed;
        }

        // ── The gates, in the order that keeps the budget transactional ──
        //
        // THE RESIDENT-BYTE GATE COMES FIRST and the reservation LAST. Both
        // need the real size, so neither can run before the conversion; but
        // reserving first and then failing the byte gate would spend frame
        // budget on a coat that was rejected anyway, and starve the coats
        // behind it — which is precisely the claim GroomProxyFrameBudget
        // makes about itself. The reservation is the last thing that can
        // fail before the buffers exist, so nothing consumes it and returns.
        const u64 bytes = proxyStats.VertexBytes + proxyStats.IndexBytes;
        const u64 residentWithout = m_Stats.ResidentBytes - entry.Bytes;
        if (bytes > GroomProxyPolicy::GeometryBytes - residentWithout)
        {
            return GroomProxyRefusalReason::ResidencyExhausted;
        }
        if (!budget.Reserve(proxyStats.VertexCount, proxyStats.TriangleCount()))
        {
            return GroomProxyRefusalReason::BudgetExhausted;
        }

        // ── The buffers ──────────────────────────────────────────────────
        //
        // GL refills a stable shape in place. Vulkan BLAS builds consume the
        // persistent device address, so even a same-shape refill publishes a
        // fresh vertex allocation while earlier frame builds may be in flight.
        //
        // Sized-then-filled, never Create(data, size): the latter mints an
        // IMMUTABLE buffer on the GL backend and SetData on it is a silent
        // no-op, so the coat's proxy would hold the pose it was first built in
        // forever, with nothing logged. The same trap GroomRenderPass documents.
        const bool reallocate = !entry.Vertices || !entry.Indices || entry.ShapeHash != shapeHash ||
                                entry.VertexCount != proxyStats.VertexCount ||
                                entry.IndexCount != proxyStats.IndexCount;
        Ref<IndexBuffer> replacementIndices;
        try
        {
            OLO_PERF_SCOPE_AUTO("GroomProxy::Upload");
            if (reallocate)
            {
                replacementIndices = IndexBuffer::Create(m_ProxyIndices.data(), proxyStats.IndexCount);
                if (!replacementIndices)
                    return GroomProxyRefusalReason::BuildFailed;
            }
            if (reallocate || RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
            {
                // Created WITH its data on Vulkan (#1533): a buffer created empty
                // and then written also keeps a CPU shadow copy of every byte as
                // a streamed buffer, 11 MB a frame for the showcase coat. GL keeps
                // sized-then-filled (above).
                const bool vulkan = RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
                auto replacement = vulkan ? VertexBuffer::Create(static_cast<const void*>(m_ProxyVertices.data()),
                                                                 static_cast<u32>(proxyStats.VertexBytes))
                                          : VertexBuffer::Create(static_cast<u32>(proxyStats.VertexBytes));
                if (!replacement)
                    return GroomProxyRefusalReason::BuildFailed;
                if (!vulkan)
                    replacement->SetData({ m_ProxyVertices.data(), static_cast<u32>(proxyStats.VertexBytes) });
                replacement->SetLayout(Vertex::GetLayout());
                // Publish the pair only after both allocations and the upload succeed.
                entry.Vertices = std::move(replacement);
                if (reallocate)
                    entry.Indices = std::move(replacementIndices);
            }
            else
            {
                entry.Vertices->SetData({ m_ProxyVertices.data(), static_cast<u32>(proxyStats.VertexBytes) });
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("GroomSurfaceCache: proxy buffer allocation failed: {}", e.what());
            return GroomProxyRefusalReason::BuildFailed;
        }

        // A buffer with no device address cannot back a BLAS. Refused here
        // rather than staged: an acceleration structure built over address zero
        // is a device loss with no validation message.
        if (entry.Vertices->GetDeviceAddress() == 0u || entry.Indices->GetDeviceAddress() == 0u)
        {
            // The buffers go, but `Bytes` STAYS at the value that is still
            // counted in m_Stats.ResidentBytes. Zeroing it here made the
            // caller's `ResidentBytes -= entry.Bytes` subtract nothing, so
            // the dead coat's bytes were counted for the rest of the session
            // — and since ResidentBytes carries across frames, enough of them
            // underflow `GeometryBytes - residentWithout` above and the
            // resident cap fails OPEN, which is the one direction a budget
            // must never fail.
            entry.Vertices.Reset();
            entry.Indices.Reset();
            entry.VertexCount = 0u;
            entry.IndexCount = 0u;
            return GroomProxyRefusalReason::BuildFailed;
        }

        m_Stats.ResidentBytes = residentWithout + bytes;
        entry.Bytes = bytes;
        entry.ShapeHash = shapeHash;
        entry.VertexCount = proxyStats.VertexCount;
        entry.IndexCount = proxyStats.IndexCount;
        entry.Compensation = compensation;
        entry.Deformed = deformed;
        // Saturating rather than wrapping: a revision that wrapped to a value
        // the consumer already saw reads as "this coat did not move", and the
        // structure would then never refit again. 2^32 refills is 2.3 years at
        // 60 Hz, so the saturation is a statement about correctness rather than
        // a case anyone reaches.
        entry.Revision = entry.Revision == std::numeric_limits<u32>::max() ? entry.Revision : entry.Revision + 1u;

        entry.LastRefreshed = m_Frame;
        ++m_Stats.Rebuilds;
        m_Stats.SegmentsConverted += proxyStats.SegmentCount;
        m_Stats.TrianglesBuilt += proxyStats.TriangleCount();
        return GroomProxyRefusalReason::None;
    }

    GroomProxyRefusalReason GroomSurfaceCache::RefreshOnGpu(Entry& entry, u64 shapeHash, f32 compensation,
                                                            const GroomProxyConversionSettings& conversion,
                                                            GroomProxyFrameBudget& budget)
    {
        OLO_PERF_SCOPE_AUTO("GroomProxy::GpuStage");
        const u32 segments = static_cast<u32>(entry.Rest.size() / 4u);
        const u32 ribbons = conversion.CrossedRibbons ? 2u : 1u;
        const u64 vertexCount = static_cast<u64>(segments) * ribbons * 4u;
        const u64 indexCount = static_cast<u64>(segments) * ribbons * 6u;
        if (segments == 0u || vertexCount > std::numeric_limits<u32>::max() || indexCount > std::numeric_limits<u32>::max())
            return GroomProxyRefusalReason::GroomHasNoGeometry;
        const u64 vertexBytes = vertexCount * sizeof(Vertex);
        const u64 bytes = vertexBytes + indexCount * sizeof(u32);

        // The same gates, in the same order, as the CPU build.
        const u64 residentWithout = m_Stats.ResidentBytes - entry.Bytes;
        if (bytes > GroomProxyPolicy::GeometryBytes - residentWithout)
            return GroomProxyRefusalReason::ResidencyExhausted;
        if (!budget.Reserve(static_cast<u32>(vertexCount), static_cast<u32>(segments * ribbons * 2u)))
            return GroomProxyRefusalReason::BudgetExhausted;

        try
        {
            // The deformation buffer's image on the GPU: whole after a relayout,
            // then the frame's regions alone, as GroomRenderPass sends its own.
            const GroomDeformBufferLayout& layout = entry.DeformCpu.GetLayout();
            const std::span<const u8> image = entry.DeformCpu.GetBytes();
            const bool relayout = !entry.DeformGpu;
            if (relayout)
            {
                entry.DeformGpu = StorageBuffer::Create(static_cast<u32>(layout.TotalBytes()),
                                                        ShaderBindingLayout::SSBO_GROOM_DEFORMATION,
                                                        StorageBufferUsage::StreamCommandOrdered);
                if (!entry.DeformGpu)
                    return GroomProxyRefusalReason::BuildFailed;
            }
            const u64 usedEnd = (static_cast<u64>(layout.DisplacementBase) +
                                 static_cast<u64>(entry.DeformCpu.GetFrameStats().DisplacementCount) * 2u) *
                                16u;
            const u64 begin = relayout ? 0u : layout.DynamicOffsetBytes();
            const u64 end = std::min<u64>(relayout ? image.size() : std::max(usedEnd, begin), image.size());
            if (end > begin)
                entry.DeformGpu->SetData(image.data() + begin, static_cast<u32>(end - begin), static_cast<u32>(begin));

            // The rest stream's segments, rebuilt with the rest stream: corners 0
            // and 2, the corners the CPU conversion reads.
            if (!entry.RestSegments || entry.RestSegmentsHash != entry.RestHash)
            {
                std::vector<glm::vec4> packed(static_cast<sizet>(segments) * 3u);
                for (u32 segment = 0u; segment < segments; ++segment)
                {
                    const GroomStrandVertex& c0 = entry.Rest[static_cast<sizet>(segment) * 4u + 0u];
                    const GroomStrandVertex& c2 = entry.Rest[static_cast<sizet>(segment) * 4u + 2u];
                    const u32 root0 = static_cast<u32>(c0.PrevPosition.x + 0.5f);
                    const u32 root2 = static_cast<u32>(c2.PrevPosition.x + 0.5f);
                    packed[segment * 3u + 0u] = glm::vec4(c0.Position, c0.Coords.x);
                    packed[segment * 3u + 1u] = glm::vec4(c2.Position, c2.Coords.x);
                    packed[segment * 3u + 2u] =
                        glm::vec4(std::bit_cast<f32>(root0), std::bit_cast<f32>(root2), c0.Radius, c2.Radius);
                }
                entry.RestSegments = VertexBuffer::Create(static_cast<const void*>(packed.data()),
                                                          static_cast<u32>(packed.size() * sizeof(glm::vec4)));
                if (!entry.RestSegments)
                    return GroomProxyRefusalReason::BuildFailed;
                entry.RestSegmentsHash = entry.RestHash;
            }

            // The ribbons' buffers, kept while the segment count holds: the
            // compute pass rewrites the vertices in place each frame. A fresh
            // vertex buffer starts as zeros, finite and degenerate, so a frame
            // whose dispatch fails cannot hand a BLAS build unwritten memory.
            if (!entry.Vertices || !entry.Indices || entry.GpuSegments != segments || entry.GpuRibbons != ribbons)
            {
                std::vector<Vertex> zeros(static_cast<sizet>(vertexCount));
                auto vertices = VertexBuffer::Create(static_cast<const void*>(zeros.data()), static_cast<u32>(vertexBytes));
                std::vector<u32> pattern(static_cast<sizet>(indexCount));
                for (u32 quad = 0u; quad < segments * ribbons; ++quad)
                {
                    const u32 base = quad * 4u;
                    u32* const i = pattern.data() + static_cast<sizet>(quad) * 6u;
                    i[0] = base;
                    i[1] = base + 1u;
                    i[2] = base + 2u;
                    i[3] = base;
                    i[4] = base + 2u;
                    i[5] = base + 3u;
                }
                auto indices = IndexBuffer::Create(pattern.data(), static_cast<u32>(indexCount));
                if (!vertices || !indices)
                    return GroomProxyRefusalReason::BuildFailed;
                vertices->SetLayout(Vertex::GetLayout());
                entry.Vertices = std::move(vertices);
                entry.Indices = std::move(indices);
                entry.GpuSegments = segments;
                entry.GpuRibbons = ribbons;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("GroomSurfaceCache: GPU proxy buffer allocation failed: {}", e.what());
            return GroomProxyRefusalReason::BuildFailed;
        }
        if (entry.Vertices->GetDeviceAddress() == 0u || entry.Indices->GetDeviceAddress() == 0u ||
            entry.RestSegments->GetDeviceAddress() == 0u)
        {
            // As the CPU build: `Bytes` stays at the value still counted.
            entry.Vertices.Reset();
            entry.Indices.Reset();
            entry.VertexCount = 0u;
            entry.IndexCount = 0u;
            entry.GpuSegments = 0u;
            return GroomProxyRefusalReason::BuildFailed;
        }

        const GroomDeformBufferLayout& layout = entry.DeformCpu.GetLayout();
        const GroomDeformFrameStats& frame = entry.DeformCpu.GetFrameStats();
        GpuJob job;
        job.Deform = entry.DeformGpu.Raw();
        job.RestAddress = entry.RestSegments->GetDeviceAddress();
        job.OutputAddress = entry.Vertices->GetDeviceAddress();
        job.SegmentCount = segments;
        job.Ribbons = ribbons;
        job.WidthScale = std::isfinite(conversion.WidthScale) && conversion.WidthScale > 0.0f ? conversion.WidthScale : 1.0f;
        job.DeformModes = glm::ivec4(1, frame.Simulated ? 1 : 0, static_cast<i32>(layout.RootCount),
                                     static_cast<i32>(layout.SlotCount));
        job.DeformBases = glm::ivec4(static_cast<i32>(layout.RootBase), static_cast<i32>(layout.SlotBase),
                                     static_cast<i32>(layout.DisplacementBase), static_cast<i32>(frame.DisplacementCount));
        m_GpuJobs.Add(job);

        m_Stats.ResidentBytes = residentWithout + bytes;
        entry.Bytes = bytes;
        entry.ShapeHash = shapeHash;
        entry.VertexCount = static_cast<u32>(vertexCount);
        entry.IndexCount = static_cast<u32>(indexCount);
        entry.Compensation = compensation;
        entry.Deformed = true;
        entry.Revision = entry.Revision == std::numeric_limits<u32>::max() ? entry.Revision : entry.Revision + 1u;
        entry.LastRefreshed = m_Frame;
        ++m_Stats.Rebuilds;
        m_Stats.SegmentsConverted += segments;
        m_Stats.TrianglesBuilt += segments * ribbons * 2u;
        return GroomProxyRefusalReason::None;
    }

    u32 GroomSurfaceCache::Dispatch()
    {
        OLO_PERF_SCOPE_AUTO("GroomProxy::Dispatch");
        if (m_GpuJobs.IsEmpty())
            return 0u;
        u32 dispatched = 0u;
        if (m_GpuShader && m_GpuParams)
        {
            // The deformation images just sent are visible to the kernel, and last
            // frame's BLAS refits have finished reading the vertices it rewrites.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
            for (const GpuJob& job : m_GpuJobs)
            {
                GpuProxyParams params;
                params.Rest = SplitAddress(job.RestAddress);
                params.Output = SplitAddress(job.OutputAddress);
                params.SegmentCount = job.SegmentCount;
                params.Ribbons = job.Ribbons;
                params.WidthScale = job.WidthScale;
                params.DeformModes = job.DeformModes;
                params.DeformBases = job.DeformBases;
                // Bound before every dispatch (#1437): both points are shared.
                m_GpuParams->Bind();
                m_GpuParams->SetData(&params, sizeof(params));
                job.Deform->Bind();
                m_GpuShader->Bind();
                const u64 recordedBefore = m_GpuShader->GetRecordedDispatchCount();
                RenderCommand::DispatchCompute((job.SegmentCount + 63u) / 64u, 1u, 1u);
                if (m_GpuShader->GetRecordedDispatchCount() != recordedBefore)
                    ++dispatched;
            }
        }
        if (dispatched != static_cast<u32>(m_GpuJobs.Num()))
        {
            // Last frame's ribbons stay (zeros for a fresh buffer): finite, so the
            // refit is stale for a frame, never undefined. Said, and counted.
            m_Stats.GpuDispatchFailures += static_cast<u32>(m_GpuJobs.Num()) - dispatched;
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("[RayTracing] {} of {} coat proxy builds were not recorded; their structures refit from "
                              "last frame's ribbons",
                              static_cast<u32>(m_GpuJobs.Num()) - dispatched, m_GpuJobs.Num());
            }
        }
        m_Stats.GpuDispatched += dispatched;
        m_GpuJobs.Reset();
        return dispatched;
    }

    void GroomSurfaceCache::Extract(GPUScene& scene, std::span<const GroomStrandRequest> requests, bool wanted)
    {
        const auto started = std::chrono::steady_clock::now();
        ++m_Frame;
        const u64 previousResident = m_Stats.ResidentBytes;
        m_Stats.Reset();
        m_Stats.ResidentBytes = previousResident;
        // Builds a frame queued and never dispatched (no ray-tracing pass ran)
        // point into entries this call may retire: dropped, and counted.
        m_Stats.GpuDispatchFailures += static_cast<u32>(m_GpuJobs.Num());
        m_GpuJobs.Reset();

        const bool active = m_Enabled && wanted && !GroomProxyDiagnostics::GetDisabled();
        const std::optional<GroomProxyTier> forcedTier = GroomProxyDiagnostics::GetForcedTier();

        // Entities the frame offered, whatever became of them. The tier
        // hysteresis is pruned against THIS rather than against the resident
        // set: a coat refused for one frame because the budget was spent is
        // still in the scene, and dropping its hold would let it re-decide
        // its tier from scratch on the frame it comes back.
        std::vector<i64> seen;
        seen.reserve(requests.size());

        // ── Oldest first ────────────────────────────────────────────────
        //
        // A scene with more animated coats than the per-frame budget covers
        // refreshes a SUBSET each frame. Walking the request vector in order
        // would make that subset the same one every frame — so the animals at
        // the back would hold a structure from the moment they appeared while
        // the ones at the front stayed current, which is a permanent artefact
        // on identifiable animals rather than a shared, moving one.
        //
        // Never-refreshed coats sort first (LastRefreshed 0), then the
        // stalest. The request INDEX breaks ties so the order cannot depend
        // on how the map happens to be laid out. The same rule, for the same
        // reason, that VegetationSurfaceCache applies to its own snapshots.
        std::vector<u32> order;
        order.reserve(requests.size());
        for (u32 index = 0; index < static_cast<u32>(requests.size()); ++index)
        {
            if (requests[index].Groom)
            {
                order.push_back(index);
            }
        }
        std::sort(order.begin(), order.end(),
                  [this, &requests](u32 lhs, u32 rhs)
                  {
                      const auto age = [this, &requests](u32 index) -> u64
                      {
                          const auto& r = requests[index];
                          const auto found = m_Entries.find(
                              Key{ static_cast<i64>(r.EntityID), static_cast<u64>(r.Handle) });
                          return found == m_Entries.end() ? 0u : found->second.LastRefreshed;
                      };
                      const u64 a = age(lhs), b = age(rhs);
                      return a != b ? a < b : lhs < rhs;
                  });

        GroomProxyFrameBudget budget;
        for (const u32 index : order)
        {
            const auto& request = requests[index];
            const Key key{ static_cast<i64>(request.EntityID), static_cast<u64>(request.Handle) };

            GroomProxyInputs inputs;
            inputs.PixelSize = request.ApparentPixelSize;
            inputs.StrandsAvailable = request.Groom->GetCurveCount();
            inputs.Requested = active;

            const i64 entity = static_cast<i64>(request.EntityID);
            seen.push_back(entity);
            GroomProxyState& state = m_TierState[entity];
            GroomProxyDecision decision = AdvanceGroomProxyTier(inputs, state);
            // AFTER the ladder advanced, never instead of it: the
            // hysteresis must keep tracking what the apparent size asks
            // for, or clearing the override would leave every coat on
            // whichever tier the diagnostic pinned it to until the camera
            // moved. The same reason #1258 applies its population budget
            // after AdvanceGroomLod rather than in place of it.
            if (forcedTier.has_value())
            {
                decision.Tier = *forcedTier;
                decision.StrandBudget = GroomProxyPolicy::StrandBudget(decision.Tier);
            }
            // A transform that is not a usable number would put this coat's
            // instance anywhere at all. Refused with a reason rather than
            // folded into NotRequested, which claims nobody asked — and
            // rather than staged, because a TLAS instance holding a NaN
            // transform is undefined at the device with nothing reported.
            if (!decision.IsRefused() && !IsFiniteTransform(request.Transform))
            {
                decision.Reason = GroomProxyRefusalReason::BuildFailed;
            }

            const auto found = m_Entries.find(key);
            const bool resident = found != m_Entries.end();
            // BEFORE the conversion, not after it: a coat that cannot have
            // a slot must not first pay for a strand build and a ribbon
            // conversion it is about to have thrown away.
            if (!resident && !decision.IsRefused() && m_Entries.size() >= GroomProxyPolicy::ResidentGrooms)
            {
                decision.Reason = GroomProxyRefusalReason::ResidencyExhausted;
            }
            if (decision.IsRefused())
            {
                // A refused coat loses its resident structure rather than
                // keeping a stale one. The alternative — hold what it has and
                // keep tracing it — is a coat whose ray-traced shadow is at a
                // pose the raster tier left several seconds ago, which is
                // strictly worse than no ray-traced shadow at all and is
                // invisible in every counter.
                if (resident)
                {
                    m_Stats.ResidentBytes -= found->second.Bytes;
                    m_Entries.erase(found);
                }
                m_Stats.Record(decision);
                continue;
            }

            // ── What would make this coat's geometry different ───────────
            //
            // ONE HASH, not a shape hash and a content hash, and that is a
            // fact about this producer rather than a simplification: every
            // input that changes the emitted BYTES for an UNDEFORMED groom
            // also changes how many there are. WidthScale is the near miss
            // — it only scales the radii — and it is folded in here for
            // exactly that reason.
            //
            // It has to stay that way. A refill that did NOT reallocate
            // would leave the GPU Scene geometry record identical, so an
            // undeformed coat classifies Static, its BLAS is built once
            // and compacted, and nothing would ever rebuild it — the coat
            // would cast the shadow of the shape it had when it was first
            // seen, for the rest of the session, with every counter
            // healthy. A DEFORMED groom is exempt because it carries the
            // animated flag and a revision, which is precisely how
            // RayTracingScene tells that it must refit.
            Hash shape;
            shape.Mix(static_cast<u64>(request.Handle));
            shape.Mix(request.Build.MaxStrands);
            shape.Mix(decision.StrandBudget);
            shape.Mix(request.Build.MaxSegments);
            shape.Mix(request.Build.GuidesOnly);
            shape.Mix(request.Build.CoatDigest);
            shape.Mix(request.LodLevel != nullptr);
            shape.Mix(std::to_underlying(decision.Tier));
            shape.Mix(request.WidthScale);
            shape.Mix(request.StreamingKey);

            const bool deformed = IsDeformed(request);
            Entry& entry = m_Entries[key];
            const bool changed = !entry.Vertices || entry.ShapeHash != shape.Value || deformed;
            // Captured BEFORE the refresh, because a failed one may drop the
            // buffers: the question below is whether this coat had a usable
            // structure to fall back ON, not whether it has one now.
            const bool wasResident = entry.Vertices && entry.Indices;
            const bool sameResidentShape = wasResident && entry.ShapeHash == shape.Value &&
                                           entry.StreamingKey == request.StreamingKey;
            if (wasResident && entry.StreamingKey != request.StreamingKey)
                ++m_Stats.StreamingInvalidations;
            if (changed)
            {
                const GroomProxyRefusalReason failure =
                    Refresh(entry, request, decision, shape.Value, budget);
                // A SPENT BUDGET IS NOT A REASON TO LEAVE THE SCENE. It is
                // transient by construction, and a coat that already has a
                // structure keeps occluding with it until its turn comes
                // round — which is exactly what BudgetExhausted's own
                // documentation promises. Erasing instead made an animal past
                // the budget vanish from the ray-traced world AND pay a full
                // rebuild every frame for the privilege.
                //
                // It is counted as REPRESENTED, because it is, and the frame
                // is marked incomplete, because a deforming coat held this way
                // is one frame behind its raster twin.
                if (failure == GroomProxyRefusalReason::BudgetExhausted && sameResidentShape)
                {
                    ++m_Stats.RefreshDeferred;
                    m_Stats.Complete = false;
                }
                else if (failure != GroomProxyRefusalReason::None)
                {
                    decision.Reason = failure;
                    m_Stats.ResidentBytes -= entry.Bytes;
                    m_Entries.erase(key);
                    m_Stats.Record(decision);
                    continue;
                }
            }

            else
            {
                ++m_Stats.Reused;
            }

            entry.StreamingKey = request.StreamingKey;

            entry.LastSeen = m_Frame;

            // ── The records ─────────────────────────────────────────────
            // Identity belongs to the coat, not a physical vertex version. A
            // fresh Vulkan address each frame must keep the same BLAS slot.
            // The reserved submesh index separates this logical key from mesh keys.
            const GPUSceneGeometryKey geometryKey{ static_cast<u64>(request.EntityID),
                                                   static_cast<u64>(request.Handle),
                                                   std::numeric_limits<u32>::max() };
            const GPUSceneMaterialKey materialKey{ static_cast<u64>(request.EntityID), 0u,
                                                   std::to_underlying(GPUSceneMaterialSource::Groom) };

            GPUSceneMaterialInput material;
            // The coat's neutral albedo. A fibre BCSDF is not a surface BRDF and
            // there is no closure here that could evaluate one, so what a
            // reflection ray sees is the coat's COLOUR at a plausible roughness
            // rather than #1247's lighting. Stated as the approximation it is:
            // criterion 2 asks for a credible silhouette in reflections, which
            // this delivers, and not for the fibre response to be re-derived in
            // ray space, which would be a second BCSDF implementation.
            material.m_BaseColorFactor = glm::vec4(request.Color, 1.0f);
            material.m_RoughnessFactor = 0.6f;
            material.m_MetallicFactor = 0.0f;
            material.m_AlphaMode = std::to_underlying(AlphaMode::Opaque);
            material.m_ClosureVersion = std::to_underlying(PBRModel::ClosureV2);
            material.m_MaterialKind = std::to_underlying(MaterialKind::Generic);
            // OPAQUE AND TWO-SIDED, and both halves matter. Two-sided because a
            // ribbon has no meaningful facing; opaque because the coverage the
            // coat would have got from an alpha test is already in the RADII —
            // that is what the compensation is — so an alpha-tested proxy would
            // remove it a second time. Opaque also keeps the instance off
            // GeometryClass::Masked, so a ray commits on intersection instead of
            // stopping on every strand as a candidate.
            material.m_Flags = GPUSceneMaterialFlagTwoSided | GPUSceneMaterialFlagDepthTest;
            scene.ExtractMaterial(materialKey, material);

            scene.ExtractGeometry(geometryKey,
                                  {
                                      .m_VertexBuffer = entry.Vertices->GetRHIHandle(),
                                      .m_IndexBuffer = entry.Indices->GetRHIHandle(),
                                      .m_VertexAddress = entry.Vertices->GetDeviceAddress(),
                                      .m_IndexAddress = entry.Indices->GetDeviceAddress(),
                                      .m_VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex),
                                      .m_IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32),
                                      .m_FirstIndex = 0u,
                                      .m_IndexCount = entry.IndexCount,
                                      .m_VertexCount = entry.VertexCount,
                                      .m_Flags = static_cast<u32>(GPUSceneGeometryFlagGroom) |
                                                 (deformed ? static_cast<u32>(GPUSceneGeometryFlagDeformed) : 0u),
                                  });

            // THE SAME TRANSFORM THE RASTER DRAW USES, unconditionally, and
            // that is the rule rather than a simplification: the two have to
            // agree about where this coat is, and the cheapest way to be sure
            // is for the proxy to carry whatever the draw carries.
            //
            // BuildGroomStrandMesh emits OBJECT-space points for a bound coat
            // and for an unbound one alike — GroomRenderPass sets
            // params.Model from request.Transform for every groom, with no
            // bound/unbound branch — so the binding deforms the coat WITHIN
            // the groom's own space and the entity transform still applies on
            // top. Passing an identity here for a bound coat would leave every
            // animal's ray-traced fur at the origin.
            //
            // The ABSOLUTE transform, not a render-relative one: GPU Scene
            // owns the camera-relative encoding, and RayTracingScene reads the
            // origin GPU Scene encoded against. Rebasing here would apply it
            // twice.
            scene.ExtractInstance({ static_cast<u64>(request.EntityID), geometryKey, 0u },
                                  {
                                      .m_WorldTransform = request.Transform,
                                      .m_Material = materialKey,
                                      .m_Flags = deformed ? static_cast<u32>(GPUSceneInstanceFlagAnimated) : 0u,
                                      .m_DeformedContentRevision = entry.Revision,
                                  });

            m_Stats.Record(decision);
        }

        // Retire everything that did not appear this frame. A coat that left
        // the scene, was hidden, or swapped its asset drops its structures here
        // rather than at some later eviction: a resident BLAS over a buffer
        // nothing refills is a coat frozen at the pose it last held.
        std::erase_if(m_Entries,
                      [this](const auto& item)
                      {
                          if (item.second.LastSeen == m_Frame)
                          {
                              return false;
                          }
                          m_Stats.ResidentBytes -= item.second.Bytes;
                          return true;
                      });
        // An entity that LEFT THE SCENE drops its hold, so that coming back
        // starts from the tier its apparent size selects rather than from
        // wherever the camera left it — #1252's reason for pruning
        // GroomLodState the same way.
        std::sort(seen.begin(), seen.end());
        std::erase_if(m_TierState,
                      [&seen](const auto& item)
                      { return !std::binary_search(seen.begin(), seen.end(), item.first); });

        // The frame-wide figures, summed over what is RESIDENT rather than
        // over what rebuilt. A still scene rebuilds nothing and must not
        // therefore report an empty ray-traced coat set — see the note on
        // GroomProxyStats::ResidentTriangles.
        for (const auto& [key, entry] : m_Entries)
        {
            static_cast<void>(key);
            m_Stats.ResidentTriangles += entry.IndexCount / 3u;
            m_Stats.MaxWidthCompensation = std::max(m_Stats.MaxWidthCompensation, entry.Compensation);
        }

        m_Stats.UpdateMicroseconds = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    }
} // namespace OloEngine::RayTracing
