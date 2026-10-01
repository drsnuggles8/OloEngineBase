#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"

// Complete types for the Ref<> members of the cache entries read below: GroomRenderPass.h
// only forward-declares them, and a build without the PCH (the Linux sanitizer jobs) would
// otherwise fail to instantiate their destructors in this TU.
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <optional>

// GroomRenderPass's memory-report rows (issue #1342, the 2026-09-26 groom amendment).
//
// A separate file on purpose: GroomRenderPass.cpp is under heavy change in the groom
// workstream, and every row here is a READ of state the pass already keeps. Nothing
// here allocates, frees or changes a cache decision.
//
// The rows split what the pass's single `CachedBytes` counter lumps together into capacity
// and this frame's demand, and add what it never counted:
//   * shared rest streams, ONCE each however many entities draw them — and the bytes a
//     per-entity charge would have added, as alias savings;
//   * per-entity geometry and deformation state;
//   * every slot of every coat-volume ring as capacity (the pass counts them too), with
//     only the slots bound this frame as demand;
//   * the CPU pose and shadow storage (CPU bytes, reported separately from GPU bytes).
// Old buffers a rebuild or a ring-slot resize replaced are NOT here: they are retiring
// allocations in the physical totals, which is where in-flight bytes belong.
namespace OloEngine
{
    namespace
    {
        template<typename T>
        [[nodiscard]] u64 VectorBytes(const std::vector<T>& v)
        {
            return static_cast<u64>(v.capacity()) * sizeof(T);
        }

        [[nodiscard]] std::optional<u64> Texture3DBytes(const Texture3D& texture)
        {
            u64 texel = 0;
            switch (texture.GetSpecification().Format)
            {
                case Texture3DFormat::RGBA8:
                case Texture3DFormat::R32F:
                    texel = 4;
                    break;
                case Texture3DFormat::RGBA16F:
                    texel = 8;
                    break;
                case Texture3DFormat::RGBA32F:
                    texel = 16;
                    break;
                default:
                    return std::nullopt;
            }
            return static_cast<u64>(texture.GetWidth()) * texture.GetHeight() * texture.GetDepth() * texel;
        }
    } // namespace

    void GroomRenderPass::AppendMemoryCapacityRows(TArray<MemoryCapacityRow>& rows) const
    {
        // Two clocks, as the pass keeps them: entries are stamped with the cache's own tick,
        // coat slots with the frame index every camera shares.
        const auto cacheTick = static_cast<u32>(m_CacheTick);
        const u32 frame = m_FrameState.FrameIndex;

        // --- Shared rest streams: counted once ------------------------------------------
        {
            u64 capacity = 0;
            u64 demand = 0;
            u64 perEntityChargeAvoided = 0;
            for (const auto& [key, stream] : m_RestStreams)
            {
                if (!stream)
                    continue;
                capacity += stream->Bytes;
                // Demanded when a draw this tick read it: an entry the cache retains for an
                // entity that stopped drawing still HOLDS the stream, but does not demand it.
                if (stream->BytesCountedTick == m_CacheTick)
                    demand += stream->Bytes;
                // The map holds one reference; every other is an entity entry wearing it.
                const u64 holders = stream->GetRefCount() > 1 ? stream->GetRefCount() - 1 : 0;
                if (holders > 1)
                    perEntityChargeAvoided += (holders - 1) * stream->Bytes;
            }
            MemoryCapacityRow row;
            row.Owner = "GroomRenderPass";
            row.Category = "Groom shared rest streams (once per stream)";
            row.Lifetime = MemoryLifetime::Persistent;
            row.Source = MemorySizeSource::FormatEstimate;
            row.CapacityBytes = capacity;
            row.ActiveDemandBytes = demand;
            row.AliasSavingsBytes = perEntityChargeAvoided;
            rows.Add(std::move(row));
        }

        // --- Per-entity geometry / deformation state and coat-volume rings --------------
        u64 entryCapacity = 0;
        u64 entryDemand = 0;
        u64 ringCapacity = 0;
        u64 ringDemand = 0;
        bool ringComplete = true;
        u64 cpuBytes = 0;
        for (const auto& [key, entry] : m_Cache)
        {
            entryCapacity += entry.Bytes;
            if (entry.LastUsedFrame == cacheTick)
                entryDemand += entry.Bytes;

            for (const auto& slot : entry.CoatRing)
            {
                if (!slot.Texture)
                    continue;
                const auto bytes = Texture3DBytes(*slot.Texture);
                if (!bytes)
                {
                    ringComplete = false;
                    continue;
                }
                ringCapacity += *bytes;
                if (slot.Bound && slot.LastBoundFrame == frame)
                    ringDemand += *bytes;
            }

            cpuBytes += entry.DeformCpu.GetCpuBytes();
            cpuBytes += VectorBytes(entry.CoatBakedPose) + VectorBytes(entry.CoatFibreScales) + VectorBytes(entry.CoatPoseSubset);
        }
        {
            MemoryCapacityRow row;
            row.Owner = "GroomRenderPass";
            row.Category = "Groom per-entity strand geometry and deformation";
            row.Lifetime = MemoryLifetime::Persistent;
            row.Source = MemorySizeSource::FormatEstimate;
            row.CapacityBytes = entryCapacity;
            row.ActiveDemandBytes = entryDemand;
            rows.Add(std::move(row));
        }
        {
            MemoryCapacityRow row;
            row.Owner = "GroomRenderPass";
            row.Category = "Groom coat-volume rings (every slot)";
            row.Lifetime = MemoryLifetime::Persistent;
            row.Source = MemorySizeSource::FormatEstimate;
            row.CapacityBytes = ringCapacity;
            row.ActiveDemandBytes = ringDemand;
            if (!ringComplete)
                row.UnknownReason = "a coat volume's format has no known size; capacity undercounts";
            rows.Add(std::move(row));
        }

        // --- CPU pose and shadow storage -----------------------------------------------
        for (const auto& [key, stream] : m_RestStreams)
        {
            if (stream)
                cpuBytes += VectorBytes(stream->RootCurves) + VectorBytes(stream->PoseSegments);
        }
        cpuBytes += VectorBytes(m_DeformedVertices) + VectorBytes(m_DrawnPose) + VectorBytes(m_DrawnPoseFull) +
                    VectorBytes(m_CoatSegments) + VectorBytes(m_RestCentrelines) + VectorBytes(m_CoatVolumeScratch.Density) +
                    VectorBytes(m_CoatVolumeScratch.Direction) + VectorBytes(m_CoatPackHalf) + VectorBytes(m_CoatPackFloat) +
                    static_cast<u64>(m_CpuRootScratch.GetAllocatedSize());
        for (const auto& table : m_CardFibreTables)
            cpuBytes += VectorBytes(table.ByGroup);
        {
            MemoryCapacityRow row;
            row.Owner = "GroomRenderPass";
            row.Category = "Groom CPU pose, deformation mirror and coat-bake storage";
            row.Lifetime = MemoryLifetime::Persistent;
            row.Source = MemorySizeSource::FormatEstimate;
            row.IsGpu = false;
            row.CapacityBytes = cpuBytes;
            row.UnknownReason = "active demand: reused scratch and retained poses have no per-frame demand figure";
            rows.Add(std::move(row));
        }
    }
} // namespace OloEngine
