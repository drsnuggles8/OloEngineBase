#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>

namespace OloEngine
{
    // @brief The two-place contract behind the GPU readback-stats channel (issue #721).
    //
    // GLSL twin: `OloEditor/assets/shaders/include/GPUReadbackStats.glsl`.
    // Pinned by `OloEngine/tests/Rendering/GPUReadbackStatsLayoutTest.cpp`, which
    // parses BOTH files and compares the name -> slot maps entry by entry.
    //
    // WHY A FLAT INDEXED ARRAY RATHER THAN A TYPED STRUCT. Timberdoodle's
    // `ReadbackValues` (the reference this feature is modelled on) is a struct
    // whose members each pass writes by name. Mirroring a *struct* between C++
    // and GLSL makes the contract a set of byte OFFSETS, and an offset contract
    // is the archetypal silent-drift bug in this repo: insert one `u32` on one
    // side and every counter after it reads a neighbour's value -- a plausible
    // number, which for a diagnostic channel is the worst possible output.
    //
    // A flat `uint[]` plus a shared name -> index registry makes the contract a
    // set of NAMES instead. Drift then means a name that exists on one side and
    // not the other, or the same name at two indices, and both are trivially
    // detectable by parsing the two files. That is what the layout test does, and
    // it is why the test cannot degenerate into the vacuous shape #847 had to fix
    // (`CrossShaderUBOMemberOffsetsAgree` compared `("", 0)` against `("", 0)`
    // for every member and structurally could not fail): here an empty parse is
    // itself a failure, because the registry below is never empty.
    //
    // ADDING A COUNTER: add one line to OLO_GPU_STAT_COUNTERS(X) here, one
    // `const uint OLO_STAT_<Name> = <n>u;` to the .glsl at the SAME index, and
    // nothing else. Indices are positional -- the X-list order IS the numbering,
    // so append rather than insert unless you renumber the .glsl to match (the
    // layout test fails loudly either way).

    // Per-frame counters. Any pass may `oloStatAdd(OLO_STAT_<Name>, n)` into one.
#define OLO_GPU_STAT_COUNTERS(X)                                                           \
    /* GPU per-instance cull (GPUFrustumCuller / InstanceFrustumCull.comp,                 \
       InstanceOcclusionCull.comp). Adopter #1. */                                         \
    X(InstanceCullInput, "Instances submitted to the GPU instance cull")                   \
    X(InstanceCullFrustumRejected, "Instances rejected by a frustum plane")                \
    X(InstanceCullOcclusionRejected, "Instances rejected by the Hi-Z occlusion test")      \
    X(InstanceCullDrawn, "Instances appended to the indirect draw")                        \
    X(InstanceCullDropped, "Survivors refused because the cull output ran out of room")    \
    X(InstanceCullPhase2Tested, "Phase-1 rejects re-tested against this frame's Hi-Z")     \
    X(InstanceCullPhase2Recovered, "Phase-2 survivors -- the disocclusion recovery")       \
    /* Virtual shadow map page cache (VirtualShadowMap / VSM_*.comp). Adopter #2. */       \
    X(VSMPagesRequested, "Page-allocation requests appended by the marker")                \
    X(VSMPagesRequestDropped, "Marker requests refused because the request ring was full") \
    X(VSMPagesAllocated, "Requests the physical pool satisfied")                           \
    /* NOT the same population as VSM::Statistics::PagesFreed, which also counts           \
       wraparound frees from VSM_FreeWrappedPages. This is evictions only -- the           \
       split the conflated counter could not show. */                                      \
    X(VSMPagesEvicted, "Resident pages evicted by the allocator to satisfy a request")     \
    X(VSMPagesAllocFailed, "Requests the physical pool could not satisfy")

    // Capacity-overflow flags -- one bit each in a single `atomicOr`ed word.
    //
    // THE POINT OF THE WHOLE FEATURE. A GPU pass that appends into a fixed-size
    // buffer either bound-checks the append (and silently drops the excess) or
    // does not (and corrupts memory). Both render *slightly* wrong with no
    // diagnostic. A flag turns "slightly wrong for no visible reason" into a
    // named condition the overlay and the MCP tool can show.
#define OLO_GPU_STAT_FLAGS(X)                                                \
    X(InstanceCullOutput, "The GPU instance cull's output buffer truncated") \
    X(VSMRequestRing, "The VSM page-request ring truncated")                 \
    X(VSMPhysicalPool, "The VSM physical page pool could not back every requested page")

    enum class GPUStatCounter : u32
    {
#define OLO_GPU_STAT_COUNTER_ENUM(name, desc) name,
        OLO_GPU_STAT_COUNTERS(OLO_GPU_STAT_COUNTER_ENUM)
#undef OLO_GPU_STAT_COUNTER_ENUM
            Count
    };

    enum class GPUStatFlag : u32
    {
#define OLO_GPU_STAT_FLAG_ENUM(name, desc) name,
        OLO_GPU_STAT_FLAGS(OLO_GPU_STAT_FLAG_ENUM)
#undef OLO_GPU_STAT_FLAG_ENUM
            Count
    };

    inline constexpr u32 kGPUStatCounterCount = static_cast<u32>(GPUStatCounter::Count);
    inline constexpr u32 kGPUStatFlagCount = static_cast<u32>(GPUStatFlag::Count);

    // Slots the SSBO actually reserves. Fixed and generous so that adding a
    // counter is a one-line change that does NOT resize a buffer every shader
    // declares -- a shader whose array length disagrees with the allocation is a
    // std430 layout mismatch, not a compile error, so the number is held still
    // on purpose and asserted against the registry below.
    inline constexpr u32 kGPUStatCounterSlots = 32;
    static_assert(kGPUStatCounterCount <= kGPUStatCounterSlots,
                  "OLO_GPU_STAT_COUNTERS outgrew the reserved slot count - raise kGPUStatCounterSlots "
                  "AND OLO_STAT_COUNTER_SLOTS in include/GPUReadbackStats.glsl together");
    // One `atomicOr` word, so 32 is the ceiling rather than a choice.
    static_assert(kGPUStatFlagCount <= 32, "OLO_GPU_STAT_FLAGS must fit one u32 bitmask word");

    [[nodiscard]] inline constexpr std::string_view GPUStatCounterName(GPUStatCounter counter)
    {
        constexpr std::array<std::string_view, kGPUStatCounterCount> kNames{
#define OLO_GPU_STAT_COUNTER_NAME(name, desc) #name,
            OLO_GPU_STAT_COUNTERS(OLO_GPU_STAT_COUNTER_NAME)
#undef OLO_GPU_STAT_COUNTER_NAME
        };
        const auto index = static_cast<u32>(counter);
        return index < kGPUStatCounterCount ? kNames[index] : std::string_view{};
    }

    [[nodiscard]] inline constexpr std::string_view GPUStatCounterDescription(GPUStatCounter counter)
    {
        constexpr std::array<std::string_view, kGPUStatCounterCount> kDescriptions{
#define OLO_GPU_STAT_COUNTER_DESC(name, desc) desc,
            OLO_GPU_STAT_COUNTERS(OLO_GPU_STAT_COUNTER_DESC)
#undef OLO_GPU_STAT_COUNTER_DESC
        };
        const auto index = static_cast<u32>(counter);
        return index < kGPUStatCounterCount ? kDescriptions[index] : std::string_view{};
    }

    [[nodiscard]] inline constexpr std::string_view GPUStatFlagName(GPUStatFlag flag)
    {
        constexpr std::array<std::string_view, kGPUStatFlagCount> kNames{
#define OLO_GPU_STAT_FLAG_NAME(name, desc) #name,
            OLO_GPU_STAT_FLAGS(OLO_GPU_STAT_FLAG_NAME)
#undef OLO_GPU_STAT_FLAG_NAME
        };
        const auto index = static_cast<u32>(flag);
        return index < kGPUStatFlagCount ? kNames[index] : std::string_view{};
    }

    [[nodiscard]] inline constexpr std::string_view GPUStatFlagDescription(GPUStatFlag flag)
    {
        constexpr std::array<std::string_view, kGPUStatFlagCount> kDescriptions{
#define OLO_GPU_STAT_FLAG_DESC(name, desc) desc,
            OLO_GPU_STAT_FLAGS(OLO_GPU_STAT_FLAG_DESC)
#undef OLO_GPU_STAT_FLAG_DESC
        };
        const auto index = static_cast<u32>(flag);
        return index < kGPUStatFlagCount ? kDescriptions[index] : std::string_view{};
    }

    // @brief One drained frame of the channel.
    //
    // `FrameIndex` is the engine frame the GPU counters belong to, NOT the frame
    // that read them -- the two differ by however many frames the ring took to
    // retire, which `Latency` reports. Quoting a counter without its frame index
    // is how a stats channel starts lying: a value that stopped updating looks
    // exactly like a value that is genuinely constant.
    struct GPUReadbackStatsFrame
    {
        bool Valid = false;
        u64 FrameIndex = 0;
        u32 Latency = 0; // frames between the counters being written and read
        u32 Flags = 0;   // bit i = GPUStatFlag(i) fired
        std::array<u32, kGPUStatCounterCount> Counters{};

        [[nodiscard]] u32 Get(GPUStatCounter counter) const
        {
            const auto index = static_cast<u32>(counter);
            return index < kGPUStatCounterCount ? Counters[index] : 0u;
        }

        [[nodiscard]] bool Overflowed(GPUStatFlag flag) const
        {
            return (Flags & (1u << static_cast<u32>(flag))) != 0u;
        }

        [[nodiscard]] bool AnyOverflow() const
        {
            return Flags != 0u;
        }
    };
} // namespace OloEngine
