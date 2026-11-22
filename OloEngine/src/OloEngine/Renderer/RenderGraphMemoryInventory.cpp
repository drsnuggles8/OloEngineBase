#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"

#include <utility>

// RenderGraph's contribution to memory admission (issue #1343): the screen-sized bytes a
// graph holds that no formula derives from a quality configuration. Kept out of
// RenderGraph.cpp, which is large and shared by several workstreams, and beside
// RenderGraphMemoryReport.cpp, which publishes the same owners as report rows.
namespace OloEngine
{
    static_assert(std::to_underlying(TemporalHistoryEffect::VolumetricFog) + 1u ==
                      std::tuple_size_v<decltype(RenderGraph::ScreenMemoryUsage::HistoryBytesByEffect)>,
                  "a TemporalHistoryEffect was added: size ScreenMemoryUsage::HistoryBytesByEffect for it");

    RenderGraph::ScreenMemoryUsage RenderGraph::MeasureScreenMemory() const
    {
        ScreenMemoryUsage usage;

        // --- Persistent pass targets ----------------------------------------------------
        // A pass's own framebuffer, once per framebuffer: several passes can render into
        // one. A target the transient pool created is the pool's, counted below.
        TArray<const Framebuffer*> seen;
        for (const auto& [name, node] : m_NodeLookup)
        {
            if (!node)
                continue;
            const Ref<Framebuffer> target = node->GetTarget();
            if (!target || seen.Contains(target.Raw()) || m_TransientPool.IsStalePooledFramebuffer(target.Raw()))
                continue;
            seen.Add(target.Raw());
            usage.NodeTargetBytes += RendererMemoryFormat::FramebufferBytes(target->GetSpecification()).value_or(0u);
        }

        // --- Transient pool ---------------------------------------------------------------
        const TransientPool::MemoryUsage pool = m_TransientPool.GetMemoryUsage();
        usage.TransientDemandBytes = pool.LastFrameDemandBytes;
        usage.TransientSlackBytes = pool.CapacityBytes > pool.LastFrameDemandBytes ? pool.CapacityBytes - pool.LastFrameDemandBytes : 0;

        // --- Temporal histories -------------------------------------------------------
        m_TemporalHistoryRegistry.ForEachTextureBackedHistory(
            [&usage](const TemporalHistoryKey& key, const TemporalHistoryDescriptor& descriptor)
            {
                const auto effect = static_cast<sizet>(std::to_underlying(key.Effect));
                if (effect >= usage.HistoryBytesByEffect.size())
                    return;
                usage.HistoryBytesByEffect[effect] += RendererMemoryFormat::ImageBytes(descriptor.Format, descriptor.Width, descriptor.Height,
                                                                                       descriptor.MipLevels, 1u, descriptor.Samples)
                                                          .value_or(0u);
            });
        return usage;
    }
} // namespace OloEngine
