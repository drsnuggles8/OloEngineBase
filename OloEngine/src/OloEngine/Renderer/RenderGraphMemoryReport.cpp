#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/RenderGraphTransientPlanner.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

// RenderGraph's contribution to the renderer memory report (issue #1342): capacity versus
// active demand for the transient pool and the temporal histories it owns. Kept out of
// RenderGraph.cpp, which is large and shared by several workstreams.
namespace OloEngine
{
    namespace
    {
        [[nodiscard]] const char* HistoryCategory(const TemporalHistoryEffect effect)
        {
            switch (effect)
            {
                case TemporalHistoryEffect::TAA:
                    return "TAA history";
                case TemporalHistoryEffect::SSGI:
                    return "SSGI history";
                case TemporalHistoryEffect::SSR:
                    return "SSR history";
                case TemporalHistoryEffect::Cloudscape:
                    return "Cloudscape history";
                case TemporalHistoryEffect::RayTracedShadow:
                    return "Ray-traced shadow history";
                case TemporalHistoryEffect::PathTracer:
                    return "Path tracer accumulation";
                case TemporalHistoryEffect::ReSTIRDI:
                    return "ReSTIR DI reservoir history";
                case TemporalHistoryEffect::ReSTIRGI:
                    return "ReSTIR GI reservoir history";
                case TemporalHistoryEffect::ReSTIRPT:
                    return "ReSTIR PT path-record lineage";
                case TemporalHistoryEffect::VolumetricFog:
                    return "Froxel fog scatter lineage";
            }
            return "Temporal history";
        }
    } // namespace

    RenderGraph::TransientAliasBytes RenderGraph::ComputeTransientAliasBytes() const
    {
        TransientAliasBytes result;
        result.AliasingDisabled = Levers::DisableTransientAliasing();

        // One physical object per (alias group, slot); every entry in a group shares one
        // descriptor, so a slot's physical size is any of its entries' size.
        std::map<std::pair<std::string, u32>, u64> slotBytes;
        for (const auto& entry : m_TransientPlan)
        {
            if (!entry.WillAllocate)
                continue;
            ++result.Entries;
            result.LogicalBytes += entry.EstimatedBytes;
            if (result.AliasingDisabled)
            {
                // OLO_RG_DISABLE_ALIASING gives every entry its own object.
                result.PhysicalBytes += entry.EstimatedBytes;
                ++result.Slots;
                continue;
            }
            slotBytes.try_emplace({ entry.AliasGroup.ToStdString(), entry.AliasSlot }, entry.EstimatedBytes);
        }
        for (const auto& [slot, bytes] : slotBytes)
        {
            result.PhysicalBytes += bytes;
            ++result.Slots;
        }
        return result;
    }

    void RenderGraph::AppendMemoryCapacityRows(TArray<MemoryCapacityRow>& rows) const
    {
        // --- Transient pool -------------------------------------------------------------
        {
            const auto pool = m_TransientPool.GetMemoryUsage();
            const auto alias = ComputeTransientAliasBytes();

            MemoryCapacityRow row;
            row.Owner = "TransientPool";
            row.Category = "Render-graph transients";
            row.Lifetime = MemoryLifetime::Pooled;
            row.Source = MemorySizeSource::FormatEstimate;
            row.IsGpu = true;
            row.CapacityBytes = pool.CapacityBytes;
            row.ActiveDemandBytes = pool.LastFrameDemandBytes;
            row.AliasSavingsBytes = alias.LogicalBytes >= alias.PhysicalBytes ? alias.LogicalBytes - alias.PhysicalBytes : 0;
            if (!pool.Complete)
            {
                row.UnknownReason = "a pooled object's format has no known size; capacity and demand undercount";
            }
            rows.Add(std::move(row));
        }

        // --- Attachment views: aliases by construction ----------------------------------
        // A view (CreateFramebufferAttachmentView / ...DepthAttachmentView) resolves to its
        // parent framebuffer's attachment and allocates nothing, so its bytes are inside the
        // parent's row above. Reported as alias bytes, never as capacity of their own.
        //
        // The skin hand-off (#1241) gets its own row because its cost is the interesting
        // one: attachment 4 of the SceneColor MRT is allocated with SceneColor on every
        // frame, but only demanded when a reachable pass (SSS) reads it.
        {
            std::unordered_map<std::string, bool> readByName;
            for (const auto& lifetime : GetResourceLifetimes())
                readByName[lifetime.ResourceName.ToStdString()] = lifetime.LastReadPassIndex != std::numeric_limits<u32>::max();

            u64 viewLogical = 0;
            u64 viewDemand = 0;
            std::optional<u64> skinBytes;
            bool skinRead = false;
            for (const auto& [name, definition] : m_TextureViewDefinitions)
            {
                const auto descIt = m_TextureViewResourceDescs.find(name);
                if (descIt == m_TextureViewResourceDescs.end())
                    continue;
                const u64 bytes = RenderGraphTransientPlanner::EstimateBytes(descIt->second);
                const auto readIt = readByName.find(name);
                const bool read = readIt != readByName.end() && readIt->second;
                if (name == ResourceNames::SceneSkinDiffuse)
                {
                    skinBytes = bytes;
                    skinRead = read;
                    continue;
                }
                viewLogical += bytes;
                if (read)
                    viewDemand += bytes;
            }

            if (skinBytes)
            {
                MemoryCapacityRow row;
                row.Owner = "RenderGraph";
                row.Category = "Skin hand-off (SceneColor attachment 4; bytes inside the TransientPool row)";
                row.Lifetime = MemoryLifetime::Pooled;
                row.Source = MemorySizeSource::FormatEstimate;
                row.CapacityBytes = *skinBytes;
                row.ActiveDemandBytes = skinRead ? *skinBytes : 0;
                row.AliasSavingsBytes = *skinBytes;
                rows.Add(std::move(row));
            }

            MemoryCapacityRow row;
            row.Owner = "RenderGraph";
            row.Category = "Attachment views (no backing of their own)";
            row.Lifetime = MemoryLifetime::Pooled;
            row.Source = MemorySizeSource::FormatEstimate;
            row.CapacityBytes = 0;
            row.ActiveDemandBytes = viewDemand;
            row.AliasSavingsBytes = viewLogical;
            rows.Add(std::move(row));
        }

        // --- Temporal histories, one row per effect ------------------------------------
        // Capacity: every registry entry that holds a texture. Demand: the ones the CURRENT
        // topology declares (m_HistoryTextureSinks survives cached frames, unlike a
        // per-acquire stamp would). An entry is never removed except by Clear(), so an
        // effect switched off keeps its history — that shows up as capacity above demand.
        std::set<std::pair<u32, u32>> declared;
        for (const auto& [name, sink] : m_HistoryTextureSinks)
        {
            if (sink.Token.IsValid())
                declared.emplace(sink.Token.Index, sink.Token.Generation);
        }

        struct EffectTotals
        {
            u64 Capacity = 0;
            u64 Demand = 0;
            bool Complete = true;
        };
        std::map<TemporalHistoryEffect, EffectTotals> byEffect;
        for (const auto& snapshot : m_TemporalHistoryRegistry.Snapshot())
        {
            if (!snapshot.HasTexture)
                continue;
            EffectTotals& totals = byEffect[snapshot.Key.Effect];
            const auto& descriptor = snapshot.Descriptor;
            const auto bytes = RendererMemoryFormat::ImageBytes(descriptor.Format, descriptor.Width, descriptor.Height,
                                                                descriptor.MipLevels, 1u, descriptor.Samples);
            if (!bytes)
            {
                totals.Complete = false;
                continue;
            }
            totals.Capacity += *bytes;
            // Snapshot tokens are at the current generation, and RefreshHistorySinkTokens
            // keeps the sinks' in step; a sink left on an older generation (its texture was
            // recreated) is not demand for the texture in the slot now.
            if (declared.contains({ snapshot.Token.Index, snapshot.Token.Generation }))
                totals.Demand += *bytes;
        }
        for (const auto& [effect, totals] : byEffect)
        {
            MemoryCapacityRow row;
            row.Owner = "TemporalHistory";
            row.Category = HistoryCategory(effect);
            row.Lifetime = MemoryLifetime::History;
            row.Source = MemorySizeSource::FormatEstimate;
            row.IsGpu = true;
            row.CapacityBytes = totals.Capacity;
            row.ActiveDemandBytes = totals.Demand;
            if (!totals.Complete)
            {
                row.UnknownReason = "a history's format has no known size; capacity undercounts";
            }
            rows.Add(std::move(row));
        }
    }
} // namespace OloEngine
