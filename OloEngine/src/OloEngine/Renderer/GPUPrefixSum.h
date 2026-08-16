#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class StorageBuffer;
    class UniformBuffer;

    // @brief Device-level GPU exclusive prefix sum (parallel scan) — issue #713.
    //
    // The dispatch half of the primitive whose per-work-group half lives in
    // `OloEditor/assets/shaders/include/PrefixSum.glsl`. Scans an arbitrary
    // `u32[]` in a storage buffer, so a compaction pass can compute each
    // survivor's output slot instead of racing for one with `atomicAdd`.
    //
    // WHY A SCAN AND NOT AN ATOMIC. Two reasons, and the first is the one that
    // matters for tests: `atomicAdd` hands out slots in the order invocations
    // happen to arrive, so the compacted output is in a different order every
    // frame even when the input is identical. A scan makes slot(i) = "how many
    // survivors are at indices below i", a pure function of the input — same
    // set, defined order, reproducible. The second is contention: one atomic per
    // survivor serializes on a single cache line, which the scan replaces with
    // per-work-group arithmetic and one dispatch per level.
    //
    // THE COST, stated plainly so callers can judge: a scan is 2-5 dispatches
    // with barriers between them, against the atomic's one. At small element
    // counts the atomic wins on latency and the scan buys only determinism; the
    // crossover is a function of how contended the counter is. This is why the
    // engine converts compaction passes case by case rather than wholesale —
    // see the retrofit note in `LightCulling.comp`.
    //
    // USAGE
    //
    //     GPUPrefixSum scan;
    //     scan.EnsureInitialised();
    //     // flags[i] = 1 for survivors, 0 otherwise
    //     scan.ExclusiveScanInPlace(flags, count, totalBuffer);
    //     // flags[i] is now survivor i's output slot; totalBuffer[0] the count
    //
    // The scan is IN PLACE. That is safe — and not the hazard it looks like —
    // because every invocation reads and writes exactly its own index, so no
    // invocation ever observes another's element. It is what lets the recursion
    // over block totals avoid a ping-pong buffer per level.
    class GPUPrefixSum : public RefCounted
    {
      public:
        // Work-group size of `PrefixSum_Scan.comp`. Mirrored there as
        // `OLO_PREFIX_SUM_GROUP_SIZE` / `local_size_x` and in
        // `PrefixSum_AddBlockOffsets.comp` as `kPrefixSumBlockSize`; all four
        // must agree.
        static constexpr u32 kGroupSize = 256;

        // GL 4.6 guarantees at least 65535 work groups per dispatch dimension,
        // and the scan dispatches 1D. One level therefore covers this many
        // elements, which bounds the recursion at three levels
        // (16.7M -> 65535 -> 256 -> 1 work group).
        static constexpr u32 kMaxWorkGroups = 65535;
        static constexpr u32 kMaxElements = kGroupSize * kMaxWorkGroups; // 16,776,960

        GPUPrefixSum();
        ~GPUPrefixSum();

        // Load the two compute shaders. Lazy so construction order does not
        // depend on the renderer having been initialised, exactly like
        // GPUFrustumCuller::EnsureInitialised.
        void EnsureInitialised();

        // True once both shaders compiled.
        //
        // A false here means the caller has NO compaction, not a degraded one:
        // #713 replaced `Particle_Compact.comp`'s `atomicAdd` path outright
        // rather than keeping two, so there is nothing to fall back to and
        // `GPUParticleSystem::Init` treats it exactly like a failed shader load
        // (log + Shutdown). A future caller that wants graceful degradation has
        // to keep its own atomic path and branch on this — the primitive does
        // not provide one.
        [[nodiscard]] bool IsAvailable() const;

        // Exclusive-scan `buffer[0 .. count)` in place: element i becomes the
        // sum of elements [0, i), and element 0 becomes 0.
        //
        // `totalOut`, when supplied, receives the grand total (the sum of ALL
        // `count` input elements) in its first `u32`. That is the value an
        // exclusive scan cannot leave in the buffer itself, and it is what a
        // compaction pass writes into its indirect draw / counter.
        //
        // Returns false without dispatching anything if the shaders are
        // unavailable or `count` exceeds `kMaxElements`. `count == 0` is legal
        // and writes a total of 0 — it goes through the same single-work-group
        // dispatch as any other small scan, where every lane is out of range.
        bool ExclusiveScanInPlace(const Ref<StorageBuffer>& buffer, u32 count,
                                  const Ref<StorageBuffer>& totalOut = {});

      private:
        // One scratch buffer per recursion level, grown monotonically. Not a
        // per-call pool: successive calls are separated by the shader-storage
        // barriers each dispatch already emits, so reuse within a frame is
        // ordered, and a level's scratch is fully consumed before the call
        // returns.
        RHI::ResourceHandle AcquireBlockSums(u32 depth, u32 elementCount);
        RHI::ResourceHandle DummyBuffer();

        void UploadParams(u32 count, bool writeBlockSums, bool writeTotal);

        // Scan `values[0 .. count)` in place, folding in block offsets via a
        // recursive scan of the per-work-group totals. `totalOut` is threaded
        // down to the single-work-group bottom level, where the group total is
        // by construction the grand total.
        void ScanRecursive(RHI::ResourceHandle values, u32 count,
                           RHI::ResourceHandle totalOut, u32 depth);

        Ref<ComputeShader> m_ScanShader;
        Ref<ComputeShader> m_AddOffsetsShader;
        Ref<UniformBuffer> m_ParamsUBO;

        // Indexed by recursion depth; `kMaxDepth` covers kMaxElements.
        static constexpr u32 kMaxDepth = 3;
        std::vector<Ref<StorageBuffer>> m_BlockSums;

        // A valid 1-uint buffer for the slots a given dispatch does not write.
        // Leaving a declared SSBO binding dangling is legal-but-undefined
        // territory that varies by driver; binding a real object costs one
        // allocation and removes the question.
        Ref<StorageBuffer> m_Dummy;

        bool m_Initialised = false;
    };
} // namespace OloEngine
