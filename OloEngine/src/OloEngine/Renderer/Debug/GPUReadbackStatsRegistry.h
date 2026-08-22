#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>
#include <utility>

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
    // itself a failure, because the tables below are never empty.
    //
    // WHY A TABLE RATHER THAN AN X-MACRO. The enumerator and its name/description
    // must not drift, which is the classic argument for an X-macro list -- but an
    // X-macro cannot parenthesise its parameters (an enumerator `(name),` does
    // not parse), so it trips cpp:S963/S960 on every expansion and buys a
    // per-path suppression in `sonar-project.properties`. A table plus the
    // `static_assert` below gets the same guarantee at compile time with no
    // macros and no config debt: entry `i` must describe counter `i`, so a name
    // added out of order, duplicated, or forgotten is a build error.
    //
    // ADDING A COUNTER: append one row to `kGPUStatCounters`, add one
    // enumerator in the same position, and add one
    // `const uint OLO_STAT_<Name> = <n>u;` to the .glsl at the SAME index.
    // Indices are positional -- append rather than insert unless you renumber the
    // .glsl to match (the layout test fails loudly either way).

    // Per-frame counters. Any pass may `oloStatAdd(OLO_STAT_<Name>, n)` into one.
    enum class GPUStatCounter : u32
    {
        // GPU per-instance cull (GPUFrustumCuller / InstanceFrustumCull.comp,
        // InstanceOcclusionCull.comp). Adopter #1.
        InstanceCullInput,
        InstanceCullFrustumRejected,
        InstanceCullOcclusionRejected,
        InstanceCullDrawn,
        InstanceCullDropped,
        InstanceCullPhase2Tested,
        InstanceCullPhase2Recovered,
        // Virtual shadow map page cache (VirtualShadowMap / VSM_*.comp). Adopter #2.
        VSMPagesRequested,
        VSMPagesRequestDropped,
        VSMPagesAllocated,
        VSMPagesEvicted,
        VSMPagesAllocFailed,
        Count
    };

    // Capacity-overflow flags -- one bit each in a single `atomicOr`ed word.
    //
    // THE POINT OF THE WHOLE FEATURE. A GPU pass that appends into a fixed-size
    // buffer either bound-checks the append (and silently drops the excess) or
    // does not (and corrupts memory). Both render *slightly* wrong with no
    // diagnostic. A flag turns "slightly wrong for no visible reason" into a
    // named condition the overlay and the MCP tool can show.
    enum class GPUStatFlag : u32
    {
        InstanceCullOutput,
        VSMRequestRing,
        VSMPhysicalPool,
        Count
    };

    inline constexpr u32 kGPUStatCounterCount = std::to_underlying(GPUStatCounter::Count);
    inline constexpr u32 kGPUStatFlagCount = std::to_underlying(GPUStatFlag::Count);

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
    static_assert(kGPUStatFlagCount <= 32, "GPUStatFlag must fit one u32 bitmask word");

    struct GPUStatEntry
    {
        // The enumerator this row describes. Present ONLY so the static_assert
        // below can prove row i describes counter i -- nothing reads it at
        // runtime, and that is the point: it is a compile-time drift guard, not
        // data.
        u32 Id;
        std::string_view Name;
        std::string_view Description;
    };

    inline constexpr std::array<GPUStatEntry, kGPUStatCounterCount> kGPUStatCounters{ {
        { std::to_underlying(GPUStatCounter::InstanceCullInput), "InstanceCullInput",
          "Instances submitted to the GPU instance cull" },
        { std::to_underlying(GPUStatCounter::InstanceCullFrustumRejected), "InstanceCullFrustumRejected",
          "Instances rejected by a frustum plane" },
        { std::to_underlying(GPUStatCounter::InstanceCullOcclusionRejected), "InstanceCullOcclusionRejected",
          "Instances rejected by the Hi-Z occlusion test" },
        { std::to_underlying(GPUStatCounter::InstanceCullDrawn), "InstanceCullDrawn",
          "Instances appended to the indirect draw" },
        { std::to_underlying(GPUStatCounter::InstanceCullDropped), "InstanceCullDropped",
          "Survivors refused because the cull output ran out of room" },
        { std::to_underlying(GPUStatCounter::InstanceCullPhase2Tested), "InstanceCullPhase2Tested",
          "Phase-1 rejects re-tested against this frame's Hi-Z" },
        { std::to_underlying(GPUStatCounter::InstanceCullPhase2Recovered), "InstanceCullPhase2Recovered",
          "Phase-2 survivors -- the disocclusion recovery" },
        { std::to_underlying(GPUStatCounter::VSMPagesRequested), "VSMPagesRequested",
          "Page-allocation requests appended by the marker" },
        { std::to_underlying(GPUStatCounter::VSMPagesRequestDropped), "VSMPagesRequestDropped",
          "Marker requests refused because the request ring was full" },
        { std::to_underlying(GPUStatCounter::VSMPagesAllocated), "VSMPagesAllocated",
          "Requests the physical pool satisfied" },
        // NOT the same population as VSM::Statistics::PagesFreed, which also
        // counts wraparound frees from VSM_FreeWrappedPages. This is evictions
        // only -- the split the conflated counter could not show.
        { std::to_underlying(GPUStatCounter::VSMPagesEvicted), "VSMPagesEvicted",
          "Resident pages evicted by the allocator to satisfy a request" },
        { std::to_underlying(GPUStatCounter::VSMPagesAllocFailed), "VSMPagesAllocFailed",
          "Requests the physical pool could not satisfy" },
    } };

    inline constexpr std::array<GPUStatEntry, kGPUStatFlagCount> kGPUStatFlags{ {
        { std::to_underlying(GPUStatFlag::InstanceCullOutput), "InstanceCullOutput",
          "The GPU instance cull's output buffer truncated" },
        { std::to_underlying(GPUStatFlag::VSMRequestRing), "VSMRequestRing",
          "The VSM page-request ring truncated" },
        { std::to_underlying(GPUStatFlag::VSMPhysicalPool), "VSMPhysicalPool",
          "The VSM physical page pool could not back every requested page" },
    } };

    // THE DRIFT GUARD, and the whole reason `GPUStatEntry::Id` exists. A row
    // added out of order, duplicated, or inserted without a matching enumerator
    // makes `Id != i` and fails the build. Without it the table would be exactly
    // the two-list arrangement this header spends its opening paragraphs warning
    // about.
    template<sizet N>
    [[nodiscard]] consteval bool GPUStatTableIsPositional(const std::array<GPUStatEntry, N>& table)
    {
        for (sizet i = 0; i < N; ++i)
        {
            if (table[i].Id != static_cast<u32>(i) || table[i].Name.empty() || table[i].Description.empty())
            {
                return false;
            }
        }
        return true;
    }

    static_assert(GPUStatTableIsPositional(kGPUStatCounters),
                  "kGPUStatCounters row i must describe GPUStatCounter(i) and carry a name + description");
    static_assert(GPUStatTableIsPositional(kGPUStatFlags),
                  "kGPUStatFlags row i must describe GPUStatFlag(i) and carry a name + description");

    [[nodiscard("the counter's name is the caller's only handle on which counter this is")]]
    constexpr std::string_view GPUStatCounterName(GPUStatCounter counter)
    {
        const auto index = std::to_underlying(counter);
        return index < kGPUStatCounterCount ? kGPUStatCounters[index].Name : std::string_view{};
    }

    [[nodiscard("returns the description rather than printing it")]]
    constexpr std::string_view GPUStatCounterDescription(GPUStatCounter counter)
    {
        const auto index = std::to_underlying(counter);
        return index < kGPUStatCounterCount ? kGPUStatCounters[index].Description : std::string_view{};
    }

    [[nodiscard("the flag's name is the caller's only handle on which condition fired")]]
    constexpr std::string_view GPUStatFlagName(GPUStatFlag flag)
    {
        const auto index = std::to_underlying(flag);
        return index < kGPUStatFlagCount ? kGPUStatFlags[index].Name : std::string_view{};
    }

    [[nodiscard("returns the description rather than printing it")]]
    constexpr std::string_view GPUStatFlagDescription(GPUStatFlag flag)
    {
        const auto index = std::to_underlying(flag);
        return index < kGPUStatFlagCount ? kGPUStatFlags[index].Description : std::string_view{};
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

        [[nodiscard("reading a counter has no side effect")]]
        u32 Get(GPUStatCounter counter) const
        {
            const auto index = std::to_underlying(counter);
            return index < kGPUStatCounterCount ? Counters[index] : 0u;
        }

        [[nodiscard("this is the overflow QUESTION, not a way to clear the flag")]]
        bool Overflowed(GPUStatFlag flag) const
        {
            return (Flags & (1u << std::to_underlying(flag))) != 0u;
        }

        [[nodiscard("this is the overflow QUESTION, not a way to clear the flags")]]
        bool AnyOverflow() const
        {
            return Flags != 0u;
        }
    };
} // namespace OloEngine
