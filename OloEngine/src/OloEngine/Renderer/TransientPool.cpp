#include "OloEnginePCH.h"
#include "TransientPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <algorithm>

namespace OloEngine
{
    TransientPool::TransientPool()
    {
        // Initialize empty pool state
    }

    TransientPool::~TransientPool()
    {
        Clear();
    }

    Ref<Texture> TransientPool::AcquireTexture(const TextureSpecification& spec)
    {
        const auto key = BuildTextureKey(spec);

        // Check if we have a pooled object available
        auto& pool = m_TexturePool[key];
        Ref<Texture> result;

        if (!pool.IsEmpty())
        {
            result = pool.Last();
            pool.Pop(EAllowShrinking::No);
        }
        else
        {
            // Create new texture if pool is empty
            const RendererMemoryOwnerScope memoryOwner("TransientPool", MemoryLifetime::Pooled);
            result = Texture2D::Create(spec);
        }

        m_AcquiredTextures.Add(result);
        return result;
    }

    Ref<Framebuffer> TransientPool::AcquireFramebuffer(const FramebufferSpecification& spec)
    {
        const auto key = BuildFramebufferKey(spec);

        auto& pool = m_FramebufferPool[key];
        Ref<Framebuffer> result;

        if (!pool.IsEmpty())
        {
            result = pool.Last();
            pool.Pop(EAllowShrinking::No);
        }
        else
        {
            const RendererMemoryOwnerScope memoryOwner("TransientPool", MemoryLifetime::Pooled);
            result = Framebuffer::Create(spec);
        }
        m_FramebufferBucketBytes.try_emplace(key, EstimateFramebufferBytes(spec));

        m_AcquiredFramebuffers.Add(result);
        return result;
    }

    Ref<StorageBuffer> TransientPool::AcquireBuffer(u32 sizeBytes)
    {
        auto& pool = m_BufferPool[sizeBytes];
        Ref<StorageBuffer> result;

        if (!pool.IsEmpty())
        {
            result = pool.Last();
            pool.Pop(EAllowShrinking::No);
        }
        else
        {
            // TODO(olbu): use appropriate binding point for transient buffers
            const RendererMemoryOwnerScope memoryOwner("TransientPool", MemoryLifetime::Pooled);
            result = StorageBuffer::Create(sizeBytes, 15, StorageBufferUsage::DynamicDraw);
        }

        m_AcquiredBuffers.Add(result);
        return result;
    }

    void TransientPool::ReleaseAll()
    {
        // Snapshot the frame's acquisition order BEFORE the lists are emptied
        // (issue #607). Every MCP read marshals onto the game thread at a frame
        // boundary, i.e. after this call, so without the snapshot
        // olo_render_transient_plan would always report an empty acquire order —
        // a confidently wrong "nothing was acquired", which is worse than no tool.
        m_LastFrameAcquireOrder = BuildAcquireOrder();

        // A release with nothing acquired is not a frame: BuildFrameGraph makes
        // one defensively at the START of every frame, right before its Trim.
        // Resetting the demand there would let that trim evict to the cap what
        // the end-of-frame trim had just kept, and the churn this exists to
        // stop would simply move to the other end of the frame (every one of
        // the four GTAO textures then re-created per frame, not two).
        const bool acquiredAnything =
            !m_AcquiredTextures.IsEmpty() || !m_AcquiredFramebuffers.IsEmpty() || !m_AcquiredBuffers.IsEmpty();
        if (!acquiredAnything)
            return;

        // Return all acquired objects to their pools, counting the frame's
        // demand per bucket on the way so Trim() knows what it must keep.
        m_LastFrameTextureDemand.clear();
        for (const auto& tex : m_AcquiredTextures)
        {
            if (tex)
            {
                const auto key = BuildTextureKey(tex->GetSpecification());
                m_TexturePool[key].Add(tex);
                ++m_LastFrameTextureDemand[key];
            }
        }
        m_AcquiredTextures.Reset();

        m_LastFrameFramebufferDemand.clear();
        for (const auto& fb : m_AcquiredFramebuffers)
        {
            if (fb)
            {
                const auto key = BuildFramebufferKey(fb->GetSpecification());
                m_FramebufferPool[key].Add(fb);
                ++m_LastFrameFramebufferDemand[key];
            }
        }
        m_AcquiredFramebuffers.Reset();

        m_LastFrameBufferDemand.clear();
        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
            {
                const auto key = buf->GetSize();
                m_BufferPool[key].Add(buf);
                ++m_LastFrameBufferDemand[key];
            }
        }
        m_AcquiredBuffers.Reset();
    }

    void TransientPool::Trim(u32 maxPerBucket)
    {
        // Keep max(cap, last frame's demand); evict from the front, where the
        // objects nobody acquired this frame sit (see the header).
        const auto trimBuckets = [maxPerBucket](auto& buckets, const auto& demandByKey)
        {
            for (auto it = buckets.begin(); it != buckets.end();)
            {
                auto& bucket = it->second;
                const auto demandIt = demandByKey.find(it->first);
                const u32 keep = std::max(maxPerBucket, demandIt != demandByKey.end() ? demandIt->second : 0u);
                if (static_cast<sizet>(bucket.Num()) > keep)
                    bucket.RemoveAt(0, bucket.Num() - static_cast<i64>(keep), EAllowShrinking::No);

                if (bucket.IsEmpty())
                    it = buckets.erase(it);
                else
                    ++it;
            }
        };

        trimBuckets(m_TexturePool, m_LastFrameTextureDemand);
        trimBuckets(m_FramebufferPool, m_LastFrameFramebufferDemand);
        trimBuckets(m_BufferPool, m_LastFrameBufferDemand);
    }

    TransientPool::TextureDescriptorKey TransientPool::BuildTextureKey(const TextureSpecification& spec)
    {
        return TextureDescriptorKey{
            .Width = spec.Width,
            .Height = spec.Height,
            .Format = static_cast<u32>(std::to_underlying(spec.Format)),
            .MipLevels = spec.MipLevels,
            .Samples = spec.Samples,
            .Flags = spec.GenerateMips ? 1u : 0u,
        };
    }

    u64 TransientPool::BuildFramebufferKey(const FramebufferSpecification& spec)
    {
        u64 key = 1469598103934665603ull;
        key ^= spec.Width;
        key *= 1099511628211ull;
        key ^= spec.Height;
        key *= 1099511628211ull;
        key ^= spec.Samples;
        key *= 1099511628211ull;
        key ^= spec.SwapChainTarget ? 1ull : 0ull;
        key *= 1099511628211ull;

        for (const auto& attach : spec.Attachments.Attachments)
        {
            key ^= static_cast<u64>(std::to_underlying(attach.TextureFormat));
            key *= 1099511628211ull;
        }

        return key;
    }

    std::optional<u64> TransientPool::EstimateTextureBytes(const TextureSpecification& spec)
    {
        // The mip count the texture ACTUALLY allocates (OpenGLTexture2D's rule): an MSAA
        // texture has one level, an explicit count wins, GenerateMips means the full chain.
        u32 mipLevels = 1u;
        if (std::max(spec.Samples, 1u) == 1u)
        {
            if (spec.MipLevels > 0u)
                mipLevels = spec.MipLevels;
            else if (spec.GenerateMips)
                mipLevels = RendererMemoryFormat::FullMipCount(spec.Width, spec.Height);
        }
        return RendererMemoryFormat::ImageBytes(spec.Format, spec.Width, spec.Height, mipLevels, 1u, spec.Samples);
    }

    std::optional<u64> TransientPool::EstimateFramebufferBytes(const FramebufferSpecification& spec)
    {
        // Every attachment, not the first one only: the SceneColor MRT is six attachments
        // and a ReSTIR reservoir framebuffer five RGBA32F ones.
        return RendererMemoryFormat::FramebufferBytes(spec);
    }

    void TransientPool::Clear()
    {
        m_TexturePool.clear();
        m_FramebufferPool.clear();
        m_BufferPool.clear();
        m_AcquiredTextures.Reset();
        m_AcquiredFramebuffers.Reset();
        m_AcquiredBuffers.Reset();
        // The snapshot describes objects that no longer exist after a Clear
        // (context loss, shutdown, a debug-flag flip evicting the pool), so drop
        // it rather than report stale GL ids — and the demand it implies.
        m_LastFrameAcquireOrder.Reset();
        m_LastFrameTextureDemand.clear();
        m_LastFrameFramebufferDemand.clear();
        m_LastFrameBufferDemand.clear();
        m_FramebufferBucketBytes.clear();
    }

    TransientPool::PoolStats TransientPool::GetStats() const
    {
        PoolStats stats{};
        stats.TexturePoolSize = 0;
        stats.TextureAliasGroups = static_cast<u32>(m_TexturePool.size());
        for (const auto& [key, pool] : m_TexturePool)
        {
            stats.TexturePoolSize += static_cast<u32>(static_cast<sizet>(pool.Num()));
        }

        stats.FramebufferPoolSize = 0;
        stats.FramebufferAliasGroups = static_cast<u32>(m_FramebufferPool.size());
        for (const auto& [key, pool] : m_FramebufferPool)
        {
            stats.FramebufferPoolSize += static_cast<u32>(static_cast<sizet>(pool.Num()));
        }

        stats.BufferPoolSize = 0;
        stats.BufferAliasGroups = static_cast<u32>(m_BufferPool.size());
        for (const auto& [key, pool] : m_BufferPool)
        {
            stats.BufferPoolSize += static_cast<u32>(static_cast<sizet>(pool.Num()));
        }

        return stats;
    }

    TArray64<TransientPool::BucketInfo> TransientPool::GetBucketReport() const
    {
        TArray64<BucketInfo> buckets;
        buckets.Reserve(m_TexturePool.size() + m_FramebufferPool.size() + m_BufferPool.size());

        for (const auto& [key, pool] : m_TexturePool)
        {
            BucketInfo info;
            info.Kind = "texture";
            info.Key = TextureDescriptorKeyHash{}(key);
            info.Width = key.Width;
            info.Height = key.Height;
            info.Format = key.Format;
            info.MipLevels = key.MipLevels;
            info.Samples = key.Samples;
            info.PooledCount = static_cast<u32>(static_cast<sizet>(pool.Num()));
            buckets.Add(std::move(info));
        }

        for (const auto& [key, pool] : m_FramebufferPool)
        {
            BucketInfo info;
            info.Kind = "framebuffer";
            info.Key = key;
            info.PooledCount = static_cast<u32>(static_cast<sizet>(pool.Num()));
            buckets.Add(std::move(info));
        }

        for (const auto& [key, pool] : m_BufferPool)
        {
            BucketInfo info;
            info.Kind = "buffer";
            info.Key = key;
            info.SizeBytes = key;
            info.PooledCount = static_cast<u32>(static_cast<sizet>(pool.Num()));
            buckets.Add(std::move(info));
        }

        // The pool maps are unordered, so iteration order is implementation-
        // defined and can differ run-to-run. Sort so two captures of an
        // unchanged pool are diffable — the same determinism reasoning the
        // generated container serializers follow.
        buckets.Sort(
            [](const BucketInfo& a, const BucketInfo& b)
            {
                if (a.Kind != b.Kind)
                    return a.Kind < b.Kind;
                return a.Key < b.Key;
            });
        return buckets;
    }

    TArray64<TransientPool::AcquiredInfo> TransientPool::GetAcquireOrder(bool* isLiveFrame) const
    {
        // Mid-frame there are live acquisitions; between frames ReleaseAll() has
        // already emptied the lists, so fall back to its snapshot of the last
        // completed frame. Without this every MCP read (which marshals at a frame
        // boundary) would report "nothing was acquired" — a confidently wrong
        // answer, which is worse than no tool at all.
        const bool live = !m_AcquiredTextures.IsEmpty() || !m_AcquiredFramebuffers.IsEmpty() ||
                          !m_AcquiredBuffers.IsEmpty();
        if (isLiveFrame != nullptr)
            *isLiveFrame = live;
        return live ? BuildAcquireOrder() : m_LastFrameAcquireOrder;
    }

    TArray64<TransientPool::AcquiredInfo> TransientPool::BuildAcquireOrder() const
    {
        TArray64<AcquiredInfo> acquired;
        acquired.Reserve(static_cast<sizet>(m_AcquiredTextures.Num()) + static_cast<sizet>(m_AcquiredFramebuffers.Num()) + static_cast<sizet>(m_AcquiredBuffers.Num()));

        // Deliberately NOT sorted: acquisition order is the whole point — it is
        // the order the alias-slot assigner consumed the pool this frame, and a
        // LIFO pool's reuse pattern is only readable in that order.
        for (const auto& texture : m_AcquiredTextures)
        {
            if (!texture)
                continue;
            const auto& spec = texture->GetSpecification();
            acquired.Add(AcquiredInfo{ "texture", texture->GetRendererID(), texture->GetRHIHandle(),
                                       spec.Width, spec.Height, 0u });
        }
        for (const auto& framebuffer : m_AcquiredFramebuffers)
        {
            if (!framebuffer)
                continue;
            const auto& spec = framebuffer->GetSpecification();
            acquired.Add(AcquiredInfo{ "framebuffer", framebuffer->GetRendererID(), framebuffer->GetRHIHandle(),
                                       spec.Width, spec.Height, 0u });
        }
        for (const auto& buffer : m_AcquiredBuffers)
        {
            if (!buffer)
                continue;
            acquired.Add(AcquiredInfo{ "buffer", buffer->GetRendererID(), buffer->GetRHIHandle(),
                                       0u, 0u, buffer->GetSize() });
        }
        return acquired;
    }

    void TransientPool::LogStats() const
    {
        const auto stats = GetStats();
        const auto memory = GetMemoryUsage();

        OLO_CORE_INFO("=== TransientPool Statistics ===");
        OLO_CORE_INFO("  Texture pool: {} objects in {} groups",
                      stats.TexturePoolSize, stats.TextureAliasGroups);
        OLO_CORE_INFO("  Framebuffer pool: {} objects in {} groups",
                      stats.FramebufferPoolSize, stats.FramebufferAliasGroups);
        OLO_CORE_INFO("  Buffer pool: {} objects in {} groups",
                      stats.BufferPoolSize, stats.BufferAliasGroups);
        OLO_CORE_INFO("  In flight: {} textures, {} framebuffers, {} buffers",
                      static_cast<sizet>(m_AcquiredTextures.Num()), static_cast<sizet>(m_AcquiredFramebuffers.Num()), static_cast<sizet>(m_AcquiredBuffers.Num()));
        OLO_CORE_INFO("  Capacity: {} bytes, acquired now: {} bytes, last frame's demand: {} bytes{}",
                      memory.CapacityBytes, memory.AcquiredBytes, memory.LastFrameDemandBytes,
                      memory.Complete ? "" : " (INCOMPLETE: an object's format has no known size)");
    }

    u64 TransientPool::EstimateMemoryUsage() const
    {
        return GetMemoryUsage().CapacityBytes;
    }

    TransientPool::MemoryUsage TransientPool::GetMemoryUsage() const
    {
        MemoryUsage usage;
        const auto add = [&usage](u64& total, const std::optional<u64>& bytes, const u64 count)
        {
            if (bytes)
                total += *bytes * count;
            else if (count > 0)
                usage.Complete = false;
        };

        const auto textureKeyBytes = [](const TextureDescriptorKey& key)
        {
            TextureSpecification spec;
            spec.Width = key.Width;
            spec.Height = key.Height;
            spec.Format = static_cast<ImageFormat>(key.Format);
            spec.MipLevels = key.MipLevels;
            spec.Samples = key.Samples;
            spec.GenerateMips = (key.Flags & 1u) != 0u;
            return EstimateTextureBytes(spec);
        };
        const auto framebufferKeyBytes = [this](const u64 key) -> std::optional<u64>
        {
            const auto it = m_FramebufferBucketBytes.find(key);
            return it != m_FramebufferBucketBytes.end() ? it->second : std::nullopt;
        };

        // Capacity: free objects in the buckets, plus whatever is acquired right now.
        for (const auto& [key, pool] : m_TexturePool)
            add(usage.CapacityBytes, textureKeyBytes(key), static_cast<u64>(pool.Num()));
        for (const auto& [key, pool] : m_FramebufferPool)
            add(usage.CapacityBytes, framebufferKeyBytes(key), static_cast<u64>(pool.Num()));
        for (const auto& [sizeBytes, pool] : m_BufferPool)
            add(usage.CapacityBytes, std::optional<u64>(sizeBytes), static_cast<u64>(pool.Num()));

        for (const auto& tex : m_AcquiredTextures)
        {
            if (tex)
                add(usage.AcquiredBytes, EstimateTextureBytes(tex->GetSpecification()), 1u);
        }
        for (const auto& fb : m_AcquiredFramebuffers)
        {
            if (fb)
                add(usage.AcquiredBytes, EstimateFramebufferBytes(fb->GetSpecification()), 1u);
        }
        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
                usage.AcquiredBytes += buf->GetSize();
        }
        usage.CapacityBytes += usage.AcquiredBytes;

        // Demand: what the last completed frame acquired, bucket by bucket.
        for (const auto& [key, count] : m_LastFrameTextureDemand)
            add(usage.LastFrameDemandBytes, textureKeyBytes(key), count);
        for (const auto& [key, count] : m_LastFrameFramebufferDemand)
            add(usage.LastFrameDemandBytes, framebufferKeyBytes(key), count);
        for (const auto& [sizeBytes, count] : m_LastFrameBufferDemand)
            add(usage.LastFrameDemandBytes, std::optional<u64>(sizeBytes), count);

        return usage;
    }

} // namespace OloEngine
