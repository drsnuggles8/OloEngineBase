#include "OloEnginePCH.h"
#include "TransientPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"

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
            // TODO: use appropriate binding point for transient buffers
            result = StorageBuffer::Create(sizeBytes, 15, StorageBufferUsage::DynamicDraw);
        }

        m_AcquiredBuffers.push_back(result);
        return result;
    }

    void TransientPool::ReleaseAll()
    {
        // Return all acquired objects to their pools
        for (const auto& tex : m_AcquiredTextures)
        {
            if (tex)
            {
                m_TexturePool[BuildTextureKey(tex->GetSpecification())].push_back(tex);
            }
        }
        m_AcquiredTextures.clear();

        for (const auto& fb : m_AcquiredFramebuffers)
        {
            if (fb)
            {
                m_FramebufferPool[BuildFramebufferKey(fb->GetSpecification())].push_back(fb);
            }
        }
        m_AcquiredFramebuffers.clear();

        for (const auto& buf : m_AcquiredBuffers)
        {
            if (buf)
            {
                m_BufferPool[buf->GetSize()].push_back(buf);
            }
        }
        m_AcquiredBuffers.clear();
    }

    void TransientPool::Trim(u32 maxPerBucket)
    {
        for (auto it = m_TexturePool.begin(); it != m_TexturePool.end();)
        {
            if (it->second.size() > maxPerBucket)
                it->second.resize(maxPerBucket);

            if (it->second.empty())
                it = m_TexturePool.erase(it);
            else
                ++it;
        }

        for (auto it = m_FramebufferPool.begin(); it != m_FramebufferPool.end();)
        {
            if (it->second.size() > maxPerBucket)
                it->second.resize(maxPerBucket);

            if (it->second.empty())
                it = m_FramebufferPool.erase(it);
            else
                ++it;
        }

        for (auto it = m_BufferPool.begin(); it != m_BufferPool.end();)
        {
            if (it->second.size() > maxPerBucket)
                it->second.resize(maxPerBucket);

            if (it->second.empty())
                it = m_BufferPool.erase(it);
            else
                ++it;
        }
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
                report.TextureGroupsWithAliasPotential++;
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
                report.FramebufferGroupsWithAliasPotential++;
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
                report.BufferGroupsWithAliasPotential++;
                u64 itemBytes = static_cast<u64>(sizeBytes);
                report.PotentialAliasingBytes += itemBytes * (pool.size() - 1);
            }
        }

        return report;
    }

} // namespace OloEngine
