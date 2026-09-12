#include "OloEnginePCH.h"
#include "TransientPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] u64 BytesPerPixel(const ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                case ImageFormat::R8UI:
                    return 1;
                case ImageFormat::R16UI:
                    return 2;
                case ImageFormat::RG16UI:
                    return 4;
                case ImageFormat::RG16F:
                    return 4;
                case ImageFormat::RGB8:
                    return 3;
                case ImageFormat::RGBA8:
                case ImageFormat::R32F:
                case ImageFormat::R32I:
                case ImageFormat::R32UI:
                case ImageFormat::DEPTH24STENCIL8:
                    return 4;
                case ImageFormat::RGBA16F:
                case ImageFormat::RG32F:
                    return 8;
                case ImageFormat::RGBA32F:
                    return 16;
                case ImageFormat::RGB32F:
                    return 12;
                case ImageFormat::None:
                default:
                    return 0;
            }
        }
    } // namespace

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

        if (!pool.empty())
        {
            result = pool.back();
            pool.pop_back();
        }
        else
        {
            // Create new texture if pool is empty
            result = Texture2D::Create(spec);
        }

        m_AcquiredTextures.push_back(result);
        return result;
    }

    Ref<Framebuffer> TransientPool::AcquireFramebuffer(const FramebufferSpecification& spec)
    {
        const auto key = BuildFramebufferKey(spec);

        auto& pool = m_FramebufferPool[key];
        Ref<Framebuffer> result;

        if (!pool.empty())
        {
            result = pool.back();
            pool.pop_back();
        }
        else
        {
            result = Framebuffer::Create(spec);
        }

        m_AcquiredFramebuffers.push_back(result);
        return result;
    }

    Ref<StorageBuffer> TransientPool::AcquireBuffer(u32 sizeBytes)
    {
        auto& pool = m_BufferPool[sizeBytes];
        Ref<StorageBuffer> result;

        if (!pool.empty())
        {
            result = pool.back();
            pool.pop_back();
        }
        else
        {
            // TODO(olbu): use appropriate binding point for transient buffers
            result = StorageBuffer::Create(sizeBytes, 15, StorageBufferUsage::DynamicDraw);
        }

        m_AcquiredBuffers.push_back(result);
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
            !m_AcquiredTextures.empty() || !m_AcquiredFramebuffers.empty() || !m_AcquiredBuffers.empty();
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
                m_TexturePool[key].push_back(tex);
                ++m_LastFrameTextureDemand[key];
            }
        }
        m_AcquiredTextures.clear();

        m_LastFrameFramebufferDemand.clear();
        for (const auto& fb : m_AcquiredFramebuffers)
        {
            if (fb)
            {
                const auto key = BuildFramebufferKey(fb->GetSpecification());
                m_FramebufferPool[key].push_back(fb);
                ++m_LastFrameFramebufferDemand[key];
            }
        }
        m_AcquiredFramebuffers.clear();

        m_LastFrameBufferDemand.clear();
        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
            {
                const auto key = buf->GetSize();
                m_BufferPool[key].push_back(buf);
                ++m_LastFrameBufferDemand[key];
            }
        }
        m_AcquiredBuffers.clear();
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
                if (bucket.size() > keep)
                    bucket.erase(bucket.begin(), bucket.begin() + static_cast<std::ptrdiff_t>(bucket.size() - keep));

                if (bucket.empty())
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

    u64 TransientPool::EstimateTextureBytes(const TextureSpecification& spec)
    {
        return static_cast<u64>(spec.Width) * static_cast<u64>(spec.Height) *
               BytesPerPixel(spec.Format) * static_cast<u64>(std::max(spec.Samples, 1u));
    }

    void TransientPool::Clear()
    {
        m_TexturePool.clear();
        m_FramebufferPool.clear();
        m_BufferPool.clear();
        m_AcquiredTextures.clear();
        m_AcquiredFramebuffers.clear();
        m_AcquiredBuffers.clear();
        // The snapshot describes objects that no longer exist after a Clear
        // (context loss, shutdown, a debug-flag flip evicting the pool), so drop
        // it rather than report stale GL ids — and the demand it implies.
        m_LastFrameAcquireOrder.clear();
        m_LastFrameTextureDemand.clear();
        m_LastFrameFramebufferDemand.clear();
        m_LastFrameBufferDemand.clear();
    }

    TransientPool::PoolStats TransientPool::GetStats() const
    {
        PoolStats stats{};
        stats.TexturePoolSize = 0;
        stats.TextureAliasGroups = static_cast<u32>(m_TexturePool.size());
        for (const auto& [key, pool] : m_TexturePool)
        {
            stats.TexturePoolSize += static_cast<u32>(pool.size());
        }

        stats.FramebufferPoolSize = 0;
        stats.FramebufferAliasGroups = static_cast<u32>(m_FramebufferPool.size());
        for (const auto& [key, pool] : m_FramebufferPool)
        {
            stats.FramebufferPoolSize += static_cast<u32>(pool.size());
        }

        stats.BufferPoolSize = 0;
        stats.BufferAliasGroups = static_cast<u32>(m_BufferPool.size());
        for (const auto& [key, pool] : m_BufferPool)
        {
            stats.BufferPoolSize += static_cast<u32>(pool.size());
        }

        return stats;
    }

    std::vector<TransientPool::BucketInfo> TransientPool::GetBucketReport() const
    {
        std::vector<BucketInfo> buckets;
        buckets.reserve(m_TexturePool.size() + m_FramebufferPool.size() + m_BufferPool.size());

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
            info.PooledCount = static_cast<u32>(pool.size());
            buckets.push_back(std::move(info));
        }

        for (const auto& [key, pool] : m_FramebufferPool)
        {
            BucketInfo info;
            info.Kind = "framebuffer";
            info.Key = key;
            info.PooledCount = static_cast<u32>(pool.size());
            buckets.push_back(std::move(info));
        }

        for (const auto& [key, pool] : m_BufferPool)
        {
            BucketInfo info;
            info.Kind = "buffer";
            info.Key = key;
            info.SizeBytes = key;
            info.PooledCount = static_cast<u32>(pool.size());
            buckets.push_back(std::move(info));
        }

        // The pool maps are unordered, so iteration order is implementation-
        // defined and can differ run-to-run. Sort so two captures of an
        // unchanged pool are diffable — the same determinism reasoning the
        // generated container serializers follow.
        std::sort(buckets.begin(), buckets.end(),
                  [](const BucketInfo& a, const BucketInfo& b)
                  {
                      if (a.Kind != b.Kind)
                          return a.Kind < b.Kind;
                      return a.Key < b.Key;
                  });
        return buckets;
    }

    std::vector<TransientPool::AcquiredInfo> TransientPool::GetAcquireOrder(bool* isLiveFrame) const
    {
        // Mid-frame there are live acquisitions; between frames ReleaseAll() has
        // already emptied the lists, so fall back to its snapshot of the last
        // completed frame. Without this every MCP read (which marshals at a frame
        // boundary) would report "nothing was acquired" — a confidently wrong
        // answer, which is worse than no tool at all.
        const bool live = !m_AcquiredTextures.empty() || !m_AcquiredFramebuffers.empty() ||
                          !m_AcquiredBuffers.empty();
        if (isLiveFrame != nullptr)
            *isLiveFrame = live;
        return live ? BuildAcquireOrder() : m_LastFrameAcquireOrder;
    }

    std::vector<TransientPool::AcquiredInfo> TransientPool::BuildAcquireOrder() const
    {
        std::vector<AcquiredInfo> acquired;
        acquired.reserve(m_AcquiredTextures.size() + m_AcquiredFramebuffers.size() + m_AcquiredBuffers.size());

        // Deliberately NOT sorted: acquisition order is the whole point — it is
        // the order the alias-slot assigner consumed the pool this frame, and a
        // LIFO pool's reuse pattern is only readable in that order.
        for (const auto& texture : m_AcquiredTextures)
        {
            if (!texture)
                continue;
            const auto& spec = texture->GetSpecification();
            acquired.push_back(AcquiredInfo{ "texture", texture->GetRendererID(), texture->GetRHIHandle(),
                                             spec.Width, spec.Height, 0u });
        }
        for (const auto& framebuffer : m_AcquiredFramebuffers)
        {
            if (!framebuffer)
                continue;
            const auto& spec = framebuffer->GetSpecification();
            acquired.push_back(AcquiredInfo{ "framebuffer", framebuffer->GetRendererID(), framebuffer->GetRHIHandle(),
                                             spec.Width, spec.Height, 0u });
        }
        for (const auto& buffer : m_AcquiredBuffers)
        {
            if (!buffer)
                continue;
            acquired.push_back(AcquiredInfo{ "buffer", buffer->GetRendererID(), buffer->GetRHIHandle(),
                                             0u, 0u, buffer->GetSize() });
        }
        return acquired;
    }

    void TransientPool::LogStats() const
    {
        const auto stats = GetStats();
        const auto aliasReport = ComputeAliasReport();

        OLO_CORE_INFO("=== TransientPool Statistics ===");
        OLO_CORE_INFO("  Texture pool: {} objects in {} groups",
                      stats.TexturePoolSize, stats.TextureAliasGroups);
        OLO_CORE_INFO("  Framebuffer pool: {} objects in {} groups",
                      stats.FramebufferPoolSize, stats.FramebufferAliasGroups);
        OLO_CORE_INFO("  Buffer pool: {} objects in {} groups",
                      stats.BufferPoolSize, stats.BufferAliasGroups);
        OLO_CORE_INFO("  In flight: {} textures, {} framebuffers, {} buffers",
                      m_AcquiredTextures.size(), m_AcquiredFramebuffers.size(), m_AcquiredBuffers.size());
        OLO_CORE_INFO("  Total pooled objects: {}",
                      stats.TexturePoolSize + stats.FramebufferPoolSize + stats.BufferPoolSize);

        OLO_CORE_INFO("=== Transient Lifetime & Aliasing Analysis ===");
        OLO_CORE_INFO("  Currently acquired: {} bytes", aliasReport.TotalAcquiredBytes);
        OLO_CORE_INFO("  Potential aliasing savings: {} bytes", aliasReport.PotentialAliasingBytes);
        OLO_CORE_INFO("  Texture groups with alias potential: {}", aliasReport.TextureGroupsWithAliasPotential);
        OLO_CORE_INFO("  Framebuffer groups with alias potential: {}", aliasReport.FramebufferGroupsWithAliasPotential);
        OLO_CORE_INFO("  Buffer groups with alias potential: {}", aliasReport.BufferGroupsWithAliasPotential);
    }

    u64 TransientPool::EstimateMemoryUsage() const
    {
        u64 totalBytes = 0;

        for (const auto& [key, pool] : m_TexturePool)
        {
            TextureSpecification spec;
            spec.Width = key.Width;
            spec.Height = key.Height;
            spec.Format = static_cast<ImageFormat>(key.Format);
            spec.MipLevels = key.MipLevels;
            spec.Samples = key.Samples;
            spec.GenerateMips = (key.Flags & 1u) != 0u;
            totalBytes += EstimateTextureBytes(spec) * pool.size();
        }

        for (const auto& tex : m_AcquiredTextures)
        {
            if (tex)
                totalBytes += EstimateTextureBytes(tex->GetSpecification());
        }

        for (const auto& [sizeBytes, pool] : m_BufferPool)
        {
            totalBytes += static_cast<u64>(sizeBytes) * pool.size();
        }

        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
                totalBytes += buf->GetSize();
        }

        return totalBytes;
    }

    TransientPool::AliasReport TransientPool::ComputeAliasReport() const
    {
        AliasReport report{};

        // Compute total currently-acquired bytes
        for (const auto& tex : m_AcquiredTextures)
        {
            if (tex)
                report.TotalAcquiredBytes += EstimateTextureBytes(tex->GetSpecification());
        }

        for (const auto& fb : m_AcquiredFramebuffers)
        {
            if (fb)
            {
                const auto& spec = fb->GetSpecification();
                const auto& attachSpec = spec.Attachments.Attachments;
                if (!attachSpec.empty())
                {
                    // Estimate framebuffer size from first attachment format
                    const auto& firstAttach = attachSpec[0];
                    if (firstAttach.TextureFormat != FramebufferTextureFormat::None)
                    {
                        // Map FramebufferTextureFormat to ImageFormat for byte calculation
                        ImageFormat imgFormat = ImageFormat::RGBA8; // default
                        if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA8)
                            imgFormat = ImageFormat::RGBA8;
                        else if (firstAttach.TextureFormat == FramebufferTextureFormat::RED_INTEGER)
                            imgFormat = ImageFormat::R32I;
                        else if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA16F)
                            imgFormat = ImageFormat::RGBA16F;
                        else if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA32F)
                            imgFormat = ImageFormat::RGBA32F;
                        else
                        {
                            // No additional handling required.
                        }

                        u64 bytesPerPixel = BytesPerPixel(imgFormat);
                        report.TotalAcquiredBytes += spec.Width * spec.Height * bytesPerPixel;
                    }
                }
            }
        }

        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
                report.TotalAcquiredBytes += buf->GetSize();
        }

        // Analyze alias potential: groups with 2+ items can theoretically share memory
        // assuming sequential use (first pool item released before second acquired)
        for (const auto& [key, pool] : m_TexturePool)
        {
            if (pool.size() > 1)
            {
                ++report.TextureGroupsWithAliasPotential;
                // Estimate savings as (count-1) * sizeof(one item)
                TextureSpecification spec;
                spec.Width = key.Width;
                spec.Height = key.Height;
                spec.Format = static_cast<ImageFormat>(key.Format);
                spec.MipLevels = key.MipLevels;
                spec.Samples = key.Samples;
                spec.GenerateMips = (key.Flags & 1u) != 0u;
                u64 itemBytes = EstimateTextureBytes(spec);
                report.PotentialAliasingBytes += itemBytes * (pool.size() - 1);
            }
        }

        for (const auto& [key, pool] : m_FramebufferPool)
        {
            if (pool.size() > 1)
            {
                ++report.FramebufferGroupsWithAliasPotential;
                // Estimate based on first framebuffer in pool
                if (pool[0])
                {
                    const auto& spec = pool[0]->GetSpecification();
                    const auto& attachSpec = spec.Attachments.Attachments;
                    if (!attachSpec.empty())
                    {
                        const auto& firstAttach = attachSpec[0];
                        if (firstAttach.TextureFormat != FramebufferTextureFormat::None)
                        {
                            ImageFormat imgFormat = ImageFormat::RGBA8; // default
                            if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA8)
                                imgFormat = ImageFormat::RGBA8;
                            else if (firstAttach.TextureFormat == FramebufferTextureFormat::RED_INTEGER)
                                imgFormat = ImageFormat::R32I;
                            else if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA16F)
                                imgFormat = ImageFormat::RGBA16F;
                            else if (firstAttach.TextureFormat == FramebufferTextureFormat::RGBA32F)
                                imgFormat = ImageFormat::RGBA32F;
                            else
                            {
                                // No additional handling required.
                            }

                            u64 bytesPerPixel = BytesPerPixel(imgFormat);
                            u64 itemBytes = spec.Width * spec.Height * bytesPerPixel;
                            report.PotentialAliasingBytes += itemBytes * (pool.size() - 1);
                        }
                    }
                }
            }
        }

        for (const auto& [sizeBytes, pool] : m_BufferPool)
        {
            if (pool.size() > 1)
            {
                ++report.BufferGroupsWithAliasPotential;
                u64 itemBytes = static_cast<u64>(sizeBytes);
                report.PotentialAliasingBytes += itemBytes * (pool.size() - 1);
            }
        }

        return report;
    }

} // namespace OloEngine
