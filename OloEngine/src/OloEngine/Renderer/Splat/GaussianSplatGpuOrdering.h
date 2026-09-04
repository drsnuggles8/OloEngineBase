#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
// Included rather than forward-declared: Ref<T>::DecRef needs the complete type
// to reach RefCounted, so a forward declaration compiles the header and fails
// at every destructor.
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUPrefixSum.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"
#include "OloEngine/Renderer/Splat/GaussianSplatView.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace OloEngine::GaussianSplat
{
    // Which GPU sort orders the splats. The radix is the pass; the bitonic
    // network is kept only as the A/B control the measurement interleaves
    // against, exactly as SplatSpike_OpaqueBaseline.glsl is kept as the
    // raster pass's control (ADR 0018 section 5.3).
    //
    // KEEPING THE CONTROL IS NOT INDECISION ABOUT WHICH SORT SHIPS. GPU timings
    // on a workstation move by up to 4x run to run with clock state and with
    // whatever else is on the box, so "the radix is faster" is only a claim if
    // both paths can be timed in one process, alternating. A before-then-after
    // measurement charges all of that drift to whichever ran second.
    enum class SortAlgorithm : u8
    {
        Radix,   // four-pass LSD radix over GPUPrefixSum (issue #1043)
        Bitonic, // the #1038 network, retained as the measurement control
    };

    // The per-view ordering pass on the GPU (issues #1038, #1043): the cull, the
    // LOD test and the back-to-front sort that `BuildViewOrdering` does on the
    // CPU, as compute dispatches feeding an indirect draw.
    //
    // WHY THIS EXISTS. The CPU pass is the one measurement that decided
    // ADR 0018 against making splats a runtime asset type: 16.6 ms at 500 k
    // splats on one thread, a whole 60 Hz frame. This is that decision's
    // precondition, so the numbers it produces are the numbers that reopen the
    // question. ADR 0018 section 5.2 records them, and they are what reversed that decision.
    //
    // NOTHING IS READ BACK ON THE FRAME PATH. The survivor count reaches the
    // draw through a GPU-written `DrawArraysIndirectCommand`, never through the
    // CPU. `ReadbackOrdering` exists only so the parity test can compare
    // against the CPU reference, and it stalls the pipeline by design.
    //
    // THE SHADERS LIVE UNDER assets/shaders/tests/. That is the same deliberate
    // choice the rasteriser made (ADR 0018 section 4): a splat pass claims no
    // binding in the engine's production UBO/SSBO namespace while it is a
    // prototype, and shaders under tests/ are excluded from the binding
    // validator that enforces that namespace. Promoting this to a real pass
    // means claiming bindings, which is a decision the ADR reserves.
    class GpuViewOrdering
    {
      public:
        GpuViewOrdering() = default;
        ~GpuViewOrdering();

        GpuViewOrdering(const GpuViewOrdering&) = delete;
        GpuViewOrdering& operator=(const GpuViewOrdering&) = delete;

        // Compiles the cull and radix compute shaders and brings up the shared
        // `GPUPrefixSum`. Returns false (and leaves the object unusable) if any
        // of them fails, so a caller without a GL 4.6 context skips rather than
        // asserting. The bitonic control's shader is NOT compiled here -- it is
        // loaded lazily by `SetSortAlgorithm`, so the pass that ships pays
        // nothing for a shader only the measurement uses.
        [[nodiscard]] auto Initialize() -> bool;

        [[nodiscard]] auto IsReady() const -> bool
        {
            return m_Ready;
        }

        // Picks the sort. Safe at any time: the two algorithms pad the sort
        // array differently, so this re-sizes the key, payload and histogram
        // buffers, but it does NOT touch the splat records -- which is the
        // point, because an A/B that re-uploaded a 96 MB cloud between samples
        // would be measuring the upload. Returns false only if the bitonic
        // control's shader could not be loaded.
        //
        // IT DOES INVALIDATE THE LAST ORDERING, because the order buffer it
        // reallocates is the one the draw reads. The indirect command's
        // instance count is zeroed to match, so a `DrawIndirect` issued before
        // the next `BuildOrdering` draws nothing rather than garbage.
        [[nodiscard]] auto SetSortAlgorithm(SortAlgorithm algorithm) -> bool;

        [[nodiscard]] auto GetSortAlgorithm() const -> SortAlgorithm
        {
            return m_Algorithm;
        }

        // Uploads `cloud` and sizes the per-view buffers for the current
        // algorithm's padding.
        void SetCloud(const SplatCloud& cloud);

        // One frame's ordering: cull + LOD + sort. Leaves the draw order in the
        // order buffer and the instance count in the indirect buffer.
        void BuildOrdering(const glm::mat4& view,
                           const glm::mat4& projection,
                           const glm::vec2& viewportPixels,
                           const ViewSettings& settings);

        // Binds the splat records, the order buffer and the shared view UBO,
        // then issues `glDrawArraysIndirect`. The caller owns blend and depth
        // state and must have the splat shader bound.
        void DrawIndirect();

        // TEST ONLY: stalls the GPU and returns what BuildOrdering produced, in
        // the same shape BuildViewOrdering fills on the CPU.
        void ReadbackOrdering(ViewOrdering& out) const;

        [[nodiscard]] auto PaddedCapacity() const -> u32
        {
            return m_PaddedCapacity;
        }
        [[nodiscard]] auto SplatCount() const -> u32
        {
            return m_SplatCount;
        }

        // Number of compute dispatches the last BuildOrdering issued. Reported
        // by the measurement so the dispatch count is observed rather than
        // recomputed from a formula that might not match the loop.
        struct DispatchCounts
        {
            u32 Cull = 0;
            // Radix: one of each per digit, four digits, so four and four.
            u32 RadixHistogram = 0;
            u32 RadixScatter = 0;
            // Calls to `GPUPrefixSum::ExclusiveScanInPlace`, NOT dispatches:
            // each call expands to between two and five of its own, and this
            // class does not get to see how many. Counted separately rather
            // than folded in so the number stays something that was observed.
            u32 RadixScanCalls = 0;
            // Non-zero only under the bitonic control.
            u32 SortGlobal = 0;
            u32 SortLocal = 0;
        };
        [[nodiscard]] auto LastDispatchCounts() const -> const DispatchCounts&
        {
            return m_Dispatches;
        }

        // `count` rounded up to a whole number of radix tiles, and at least one
        // tile. The slack is filled with the maximum sort key, which sorts
        // behind every real splat, so the sort's length never depends on a
        // survivor count the CPU would have to read back.
        //
        // THIS IS WHAT ISSUE #1043 CHANGED, AND THE POINT IS THE BOUND ON IT.
        // The bitonic network this replaced padded to a power of two, so the
        // ordering cost was a STEP FUNCTION of the padding rather than a curve
        // in `count`: measured on an RTX 4090 with both sorts interleaved in one
        // process, 2,000,000 splats padded to 2^21 and ordered in 1.24 ms while
        // 2,100,000 padded to 2^22 and took 2.16 ms -- five per cent more
        // splats, 1.7x the time (2.7x on the worst of four runs), and no visible
        // change to the scene. A radix needs no power of two, so the overhead
        // here is at most kSortTile-1 elements however large the cloud is, and
        // the same pair costs 0.69 and 0.72 ms. ADR 0018 section 5.2 has the
        // sweep, and the reason its earlier figure for that step was 6.1x.
        [[nodiscard]] static auto PaddedCapacityFor(u32 count) -> u32;

        // The bitonic control's padding: the smallest power of two that is at
        // least `count` and at least its 512-element shared-memory tile. Kept
        // so the A/B can size its buffers, and so the step function the issue
        // removed stays expressible in a test.
        [[nodiscard]] static auto BitonicPaddedCapacityFor(u32 count) -> u32;

        // Hard ceiling on a cloud this pass will accept, and the reason it is
        // this number rather than a round one: every size in the pass is a u32,
        // and each of them overflows at a different count.
        //
        //   * `GpuBytes()` is 32 bytes per splat, so it reaches 1<<32 at
        //     134,217,728 splats -- the narrowing cast in SetCloud would wrap
        //     and allocate a 32-byte buffer for the whole cloud;
        //   * the padded slot size (4 bytes each) wraps at 536,870,913;
        //   * at 1,073,741,825 the bitonic control's padded capacity is 1<<31
        //     and its `for (u32 k = 2; k <= capacity; k <<= 1)` wraps k to zero
        //     and never terminates;
        //   * above 1<<31, std::bit_ceil is asked for an unrepresentable 1<<32,
        //     which is undefined.
        //
        // 1<<26 is 67 million splats -- 2 GB of records, an order of magnitude
        // past the largest published capture -- and leaves every one of those
        // computations with a factor of two or more in hand. The radix adds one
        // more constraint, that its transposed histogram fit `GPUPrefixSum`;
        // a static_assert in the .cpp pins that this ceiling satisfies it.
        static constexpr u32 kMaxSplats = 1u << 26;

      private:
        void ReleaseBindings() const;
        // Zeroes the indirect draw's instance count. Called on every path that
        // invalidates the order buffer, not just per frame.
        // NOT const: Ref<T> propagates const, so a const method cannot reach
        // StorageBuffer::SetData through the member (gpu-scan-compaction.md 7).
        void ResetIndirectCommand();
        // Sizes the key/payload/scratch/histogram buffers from m_SplatCount and
        // the current algorithm. Separate from SetCloud so switching algorithms
        // during an A/B does not re-upload the whole cloud.
        void ResizeSortBuffers();
        void SortRadix();
        void SortBitonic();

        bool m_Ready = false;
        u32 m_SplatCount = 0;
        u32 m_PaddedCapacity = 0;
        SortAlgorithm m_Algorithm = SortAlgorithm::Radix;
        DispatchCounts m_Dispatches;

        Ref<ComputeShader> m_CullShader;
        Ref<ComputeShader> m_HistogramShader;
        Ref<ComputeShader> m_ScatterShader;
        Ref<ComputeShader> m_BitonicShader; // lazy; control only
        Ref<GPUPrefixSum> m_PrefixSum;
        Ref<StorageBuffer> m_SplatBuffer;
        Ref<StorageBuffer> m_OrderBuffer;
        Ref<StorageBuffer> m_KeyBuffer;
        // The radix scatter cannot write in place, so each pass ping-pongs into
        // these and the four passes land the result back in the pair above.
        Ref<StorageBuffer> m_OrderScratch;
        Ref<StorageBuffer> m_KeyScratch;
        Ref<StorageBuffer> m_HistogramBuffer;
        Ref<StorageBuffer> m_StatsBuffer;
        Ref<StorageBuffer> m_IndirectBuffer;
        Ref<UniformBuffer> m_CullUniforms;
        Ref<UniformBuffer> m_SortUniforms;
        Ref<VertexArray> m_EmptyVertexArray;
    };
} // namespace OloEngine::GaussianSplat
