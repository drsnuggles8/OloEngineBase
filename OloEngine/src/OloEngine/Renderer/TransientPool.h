#pragma once

#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace OloEngine
{
    // ==========================================================================
    // TransientPool
    //
    // GL 4.6 pool of reusable GPU objects (textures, framebuffers, buffers) for
    // transient render targets. Pool is keyed by descriptor compatibility:
    // textures/framebuffers group by format+dimensions+flags, buffers by size.
    //
    // Lifetime-based aliasing: transient resources with non-overlapping lifetimes
    // can reuse the same backing GPU object. This reduces VRAM usage for
    // temporary render targets (bloom pyramid, SSAO scratch, post-process ping-pong).
    //
    // **Thread safety:** Acquire/Release are NOT thread-safe. All operations
    // must occur during frame compilation and execution on the render thread.
    //
    // **Per-frame lifecycle:**
    //   1. BuildFrameGraph() compiles the pass graph
    //   2. ComputeTransientLifetimes() analyzes each transient's first write
    //      and last read based on execution order and declarations
    //   3. Acquire() assigns physical GPU objects from the pool
    //   4. After frame execution, call ReleaseAll() to return all acquired
    //      objects to the pool (preparation for next frame)
    // ==========================================================================
    class TransientPool
    {
      public:
        TransientPool();
        ~TransientPool();

        // Acquire a reusable texture matching the specification.
        // Returns the same backing object for non-overlapping transients.
        [[nodiscard]] Ref<Texture> AcquireTexture(const TextureSpecification& spec);

        // Acquire a reusable framebuffer matching the specification.
        [[nodiscard]] Ref<Framebuffer> AcquireFramebuffer(const FramebufferSpecification& spec);

        // Acquire a reusable storage buffer of the given byte size.
        [[nodiscard]] Ref<StorageBuffer> AcquireBuffer(u32 sizeBytes);

        // Release all acquired objects back to the pool.
        // Called each frame after rendering completes.
        void ReleaseAll();

        // Trim each descriptor bucket to at most maxPerBucket objects, evicting
        // any excess from the back of each pool vector. Call after ReleaseAll()
        // to prevent VRAM bloat from high-watermark frames where a feature was
        // temporarily enabled (e.g., bloom on → off leaves bloom FBs in the pool).
        // Default maxPerBucket = 2 tolerates one extra slot from same-descriptor
        // overlapping transients; use 1 for the most aggressive trim.
        void Trim(u32 maxPerBucket);

        // Clear all pooled objects (called during shutdown or context loss).
        void Clear();

        // **Debug & Validation**
        // Dump pool statistics and current state to console.
        void LogStats() const;

        // Get total memory usage of pooled + currently acquired objects (estimated).
        [[nodiscard]] u64 EstimateMemoryUsage() const;

        // **Debug:** Get pool statistics (size, utilization, alias groups).
        struct PoolStats
        {
            u32 TexturePoolSize;
            u32 TextureAliasGroups;
            u32 FramebufferPoolSize;
            u32 FramebufferAliasGroups;
            u32 BufferPoolSize;
            u32 BufferAliasGroups;
        };
        [[nodiscard]] PoolStats GetStats() const;

        // **Phase D Exit Criterion:** Report potential aliasing opportunities.
        // Returns estimated memory savings if lifetime-based aliasing were applied.
        // For GL, aliasing is a forward-looking optimization; this reports the
        // analysis for debugging and future transient allocation decisions.
        struct AliasReport
        {
            u64 TotalAcquiredBytes;              // Sum of all currently-acquired transient sizes
            u64 PotentialAliasingBytes;          // Estimated savings from sequential reuse
            u32 TextureGroupsWithAliasPotential; // Descriptor buckets with multiple items
            u32 FramebufferGroupsWithAliasPotential;
            u32 BufferGroupsWithAliasPotential;
        };
        [[nodiscard]] AliasReport ComputeAliasReport() const;

      private:
        // Descriptor key for texture/framebuffer pooling (format + dimensions + flags)
        struct TextureDescriptorKey
        {
            u32 Width;
            u32 Height;
            u32 Format; // ImageFormat as u32
            u32 Flags;  // TextureWrapMode, TextureFilterMode, etc.

            bool operator==(const TextureDescriptorKey& other) const
            {
                return Width == other.Width && Height == other.Height && Format == other.Format &&
                       Flags == other.Flags;
            }
        };

        struct TextureDescriptorKeyHash
        {
            u64 operator()(const TextureDescriptorKey& key) const
            {
                return (static_cast<u64>(key.Width) << 48) | (static_cast<u64>(key.Height) << 32) |
                       (static_cast<u64>(key.Format) << 16) | key.Flags;
            }
        };

        [[nodiscard]] static TextureDescriptorKey BuildTextureKey(const TextureSpecification& spec);
        [[nodiscard]] static u64 BuildFramebufferKey(const FramebufferSpecification& spec);
        [[nodiscard]] static u64 EstimateTextureBytes(const TextureSpecification& spec);

        // Pool entries for each resource type
        std::unordered_map<TextureDescriptorKey, std::vector<Ref<Texture>>, TextureDescriptorKeyHash>
            m_TexturePool;
        std::unordered_map<u64, std::vector<Ref<Framebuffer>>> m_FramebufferPool;
        std::unordered_map<u32, std::vector<Ref<StorageBuffer>>> m_BufferPool;

        // Track which objects are currently acquired (for debugging/validation)
        std::vector<Ref<Texture>> m_AcquiredTextures;
        std::vector<Ref<Framebuffer>> m_AcquiredFramebuffers;
        std::vector<Ref<StorageBuffer>> m_AcquiredBuffers;
    };

} // namespace OloEngine
