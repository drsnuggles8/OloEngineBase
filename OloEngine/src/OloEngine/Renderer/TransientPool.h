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

        // **Debug:** per-bucket and per-acquisition detail behind the aggregate
        // PoolStats (issue #607). Root-causing the one-frame black-square artifact
        // needed exactly this — "which pool object did this plan entry acquire,
        // and how many same-descriptor siblings could it have been handed
        // instead" — and it took a rebuild with hand-rolled instrumentation to
        // learn, because nothing exposed the plan/pool layer where the aliasing
        // decisions actually live. `olo_render_transient_plan` reports both.
        struct BucketInfo
        {
            std::string Kind;    ///< "texture" | "framebuffer" | "buffer"
            u64 Key = 0;         ///< the bucket's descriptor hash (fb / buffer) or texture-key hash
            u32 Width = 0;       ///< texture buckets only (0 elsewhere)
            u32 Height = 0;      ///< texture buckets only
            u32 Format = 0;      ///< texture buckets only (ImageFormat as u32)
            u32 MipLevels = 0;   ///< texture buckets only
            u32 Samples = 0;     ///< texture buckets only
            u32 SizeBytes = 0;   ///< buffer buckets only
            u32 PooledCount = 0; ///< objects currently sitting FREE in this bucket
        };

        // One entry per object handed out during a frame, in acquisition order —
        // the order the alias-slot assigner consumed the pool. A plan entry's
        // physical id appears here; two entries sharing an id shared an object.
        struct AcquiredInfo
        {
            std::string Kind; ///< "texture" | "framebuffer" | "buffer"
            u32 RendererID = 0;
            u32 Width = 0;     ///< texture / framebuffer only
            u32 Height = 0;    ///< texture / framebuffer only
            u32 SizeBytes = 0; ///< buffer only
        };

        [[nodiscard]] std::vector<BucketInfo> GetBucketReport() const;

        // The acquisition order. ReleaseAll() empties the live acquired lists at
        // end of frame, so ANY caller that runs between frames — every MCP read,
        // which marshals onto the game thread at a frame boundary — would
        // otherwise always see an empty list and conclude nothing was acquired.
        // ReleaseAll() therefore snapshots the order before clearing, and this
        // returns the live list mid-frame or the last COMPLETED frame's snapshot
        // between frames. `IsLiveFrame` says which, so a reader never mistakes
        // last frame's layout for this one's.
        [[nodiscard]] std::vector<AcquiredInfo> GetAcquireOrder(bool* isLiveFrame = nullptr) const;

      private:
        // Descriptor key for texture/framebuffer pooling (format + dimensions + flags)
        struct TextureDescriptorKey
        {
            u32 Width;
            u32 Height;
            u32 Format; // ImageFormat as u32
            u32 MipLevels;
            u32 Samples;
            u32 Flags; // TextureWrapMode, TextureFilterMode, etc.

            bool operator==(const TextureDescriptorKey& other) const
            {
                return Width == other.Width && Height == other.Height && Format == other.Format &&
                       MipLevels == other.MipLevels && Samples == other.Samples && Flags == other.Flags;
            }
        };

        struct TextureDescriptorKeyHash
        {
            u64 operator()(const TextureDescriptorKey& key) const
            {
                u64 hash = 1469598103934665603ull;
                hash ^= key.Width;
                hash *= 1099511628211ull;
                hash ^= key.Height;
                hash *= 1099511628211ull;
                hash ^= key.Format;
                hash *= 1099511628211ull;
                hash ^= key.MipLevels;
                hash *= 1099511628211ull;
                hash ^= key.Samples;
                hash *= 1099511628211ull;
                hash ^= key.Flags;
                hash *= 1099511628211ull;
                return hash;
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

        [[nodiscard]] std::vector<AcquiredInfo> BuildAcquireOrder() const;

        // Track which objects are currently acquired (for debugging/validation)
        std::vector<Ref<Texture>> m_AcquiredTextures;
        std::vector<Ref<Framebuffer>> m_AcquiredFramebuffers;
        std::vector<Ref<StorageBuffer>> m_AcquiredBuffers;

        // Last COMPLETED frame's acquisition order, snapshotted by ReleaseAll()
        // just before it empties the lists above — see GetAcquireOrder().
        std::vector<AcquiredInfo> m_LastFrameAcquireOrder;
    };

} // namespace OloEngine
