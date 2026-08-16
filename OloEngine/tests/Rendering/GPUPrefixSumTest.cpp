// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUPrefixSumTest.cpp — acceptance test for the GPU prefix-sum / parallel-scan
// primitive (issue #713).
//
// Covers both halves of the primitive:
//   * the WORK-GROUP scan in `assets/shaders/include/PrefixSum.glsl`, driven
//     through the two L2 probes under `assets/shaders/tests/` so the subgroup
//     path and the portable shared-memory path are each checked against the
//     same CPU reference AND against each other;
//   * the DEVICE-LEVEL scan (`GPUPrefixSum` + `compute/PrefixSum_Scan.comp` +
//     `compute/PrefixSum_AddBlockOffsets.comp`) over randomized inputs at sizes
//     that exercise all three recursion levels.
//
// WHY THE SIZE LIST LOOKS LIKE THAT. An exclusive scan is trivially right on a
// full block and wrong in exactly two places: the tail of a partial block, and
// the seam where one block's offset is folded into the next. So the sizes are
// chosen to straddle every block boundary the implementation has — 255/256/257
// around the work group, 65535/65536/65537 around the second level — plus
// primes and awkward values that land nowhere near one. A power-of-two-only
// test passes on an implementation that silently rounds its count up.
//
// EXACT EQUALITY, NOT A TOLERANCE. This is integer arithmetic with one correct
// answer; a scan that is off by one anywhere is a compaction that overwrites a
// neighbour's slot. Nothing here is allowed to be approximately right.
//
// Classification: shaderpipe (real compute shaders dispatched on the GPU),
// matching GPUOcclusionCullGPUTest and VirtualClusterCullParityTest. SKIPs
// cleanly with no GL 4.6 context, so headless CI is a no-op.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUPrefixSum.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <random>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // CPU reference. Deliberately the dumbest possible loop — the whole
        // point of a reference is that it is obviously right, so it must not
        // share any structure (blocking, recursion) with the thing under test.
        std::vector<u32> CpuExclusiveScan(const std::vector<u32>& input, u32& totalOut)
        {
            std::vector<u32> out(input.size());
            u32 running = 0;
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                out[i] = running;
                running += input[i];
            }
            totalOut = running;
            return out;
        }

        // Values stay small so the running total cannot wrap u32 even at the
        // largest size tested. Wrap-around would be *defined* and identical on
        // both sides, but a failure message reading "expected 4294967290" is a
        // worse diagnostic than one reading "expected 12".
        std::vector<u32> MakeRandomValues(u32 count, u32 seed, u32 maxValue)
        {
            std::mt19937 rng(seed);
            std::uniform_int_distribution<u32> dist(0, maxValue);
            std::vector<u32> values(count);
            for (u32& v : values)
                v = dist(rng);
            return values;
        }

        // Upload `values`, scan in place on the GPU, read back. Returns the
        // scanned array; `totalOut` receives the grand total the scan reported
        // (which an exclusive scan cannot leave in the buffer itself).
        std::vector<u32> GpuExclusiveScan(GPUPrefixSum& scan, const std::vector<u32>& values, u32& totalOut)
        {
            const auto count = static_cast<u32>(values.size());
            const u32 bytes = std::max(count, 1u) * static_cast<u32>(sizeof(u32));

            auto buffer = StorageBuffer::Create(bytes, ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES,
                                                StorageBufferUsage::DynamicCopy);
            if (count > 0)
                buffer->SetData(values.data(), bytes, 0);

            auto totalBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(u32)),
                                                     ShaderBindingLayout::SSBO_PREFIX_SUM_TOTAL,
                                                     StorageBufferUsage::DynamicCopy);
            totalBuffer->ClearData();

            EXPECT_TRUE(scan.ExclusiveScanInPlace(buffer, count, totalBuffer));

            std::vector<u32> result(count);
            if (count > 0)
                buffer->GetData(result.data(), bytes, 0);
            totalBuffer->GetData(&totalOut, static_cast<u32>(sizeof(u32)), 0);

            // Leave no SSBO of ours dangling on a shared binding point for the
            // next test in this process — §6.4 of testing-architecture.md.
            buffer->Unbind();
            totalBuffer->Unbind();
            return result;
        }

        // Layout of the L2 probe shaders' output block: a 4-uint header
        // followed by the per-lane exclusive prefixes.
        constexpr u32 kProbeGroupSize = 256;
        constexpr u32 kProbeHeaderUints = 4;

        struct ProbeResult
        {
            bool Compiled = false;
            u32 GroupTotal = 0;
            bool TookSubgroupPath = false;
            std::vector<u32> Exclusive;
        };

        // Does this driver expose subgroup ops at all?
        //
        // This must be asked BEFORE the subgroup probe is created, not inferred
        // from a failed create. `OpenGLComputeShader` raises
        // `OLO_CORE_ASSERT(..., "Compute shader compilation failure!")` on a
        // compile error, and in a Debug build that is a MODAL DIALOG — which in
        // an unattended run (CI, an agent session) blocks the whole process
        // forever rather than failing. Observed, not theorised: the first run of
        // this test sat at 0% CPU behind an "OloEngine Assert" window.
        bool DriverHasSubgroupOps()
        {
            GLint count = 0;
            ::glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i)
            {
                const auto* name = reinterpret_cast<const char*>(::glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (name != nullptr && std::strcmp(name, "GL_KHR_shader_subgroup") == 0)
                    return true;
            }
            return false;
        }

        // A compile failure is REPORTED, not asserted — but see DriverHasSubgroupOps
        // above for why the caller must avoid reaching one in the first place.
        ProbeResult RunWorkGroupProbe(const char* shaderPath, const std::vector<u32>& values)
        {
            ProbeResult result;

            auto cs = ComputeShader::Create(shaderPath);
            if (!cs || !cs->IsValid())
                return result;
            result.Compiled = true;

            const u32 inBytes = kProbeGroupSize * static_cast<u32>(sizeof(u32));
            const u32 outBytes = (kProbeHeaderUints + kProbeGroupSize) * static_cast<u32>(sizeof(u32));

            auto inBuffer = StorageBuffer::Create(inBytes, 0, StorageBufferUsage::DynamicDraw);
            inBuffer->SetData(values.data(), inBytes, 0);
            auto outBuffer = StorageBuffer::Create(outBytes, 1, StorageBufferUsage::DynamicCopy);
            outBuffer->ClearData();

            inBuffer->Bind();
            outBuffer->Bind();
            cs->Bind();
            RenderCommand::DispatchCompute(1, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            std::vector<u32> raw(kProbeHeaderUints + kProbeGroupSize);
            outBuffer->GetData(raw.data(), outBytes, 0);

            result.GroupTotal = raw[0];
            result.TookSubgroupPath = (raw[1] != 0u);
            result.Exclusive.assign(raw.begin() + kProbeHeaderUints, raw.end());

            // Leave nothing of ours bound: this process shares one GL context
            // across every test, and a program left current when its object is
            // deleted is the deferred-deletion landmine of
            // testing-architecture.md §6.6.
            cs->Unbind();
            inBuffer->Unbind();
            outBuffer->Unbind();
            return result;
        }
    } // namespace

    // =========================================================================
    // Acceptance criterion #1a — the device-level scan over randomized inputs
    // at non-power-of-two sizes.
    //
    // The sizes span all three recursion levels: <= 256 is a single work group
    // (no block sums at all), <= 65536 adds one fold-back, above that adds a
    // second. 65537 is the smallest input that makes the recursion three deep,
    // and it is the one a two-level implementation gets silently wrong.
    // =========================================================================
    TEST(GPUPrefixSumTest, ScanMatchesCpuReferenceAcrossSizes)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUPrefixSum scan;
        scan.EnsureInitialised();
        ASSERT_TRUE(scan.IsAvailable()) << "PrefixSum_Scan.comp / PrefixSum_AddBlockOffsets.comp failed to compile";

        const std::vector<u32> sizes{
            1,
            2,
            3,
            7,
            31,
            63,
            64,
            100,
            255,
            256,
            257, // straddles the work group
            511,
            512,
            513,
            777,
            1000,
            4095,
            4096,
            4097,
            65535,
            65536,
            65537,  // straddles the second recursion level
            100003, // prime, three levels deep
        };

        u32 seed = 0x513u;
        for (const u32 count : sizes)
        {
            const std::vector<u32> input = MakeRandomValues(count, seed++, 15u);

            u32 cpuTotal = 0;
            const std::vector<u32> expected = CpuExclusiveScan(input, cpuTotal);

            u32 gpuTotal = 0;
            const std::vector<u32> actual = GpuExclusiveScan(scan, input, gpuTotal);

            ASSERT_EQ(actual.size(), expected.size()) << "count = " << count;
            EXPECT_EQ(gpuTotal, cpuTotal) << "grand total wrong at count = " << count;

            // Report the FIRST divergence rather than 100k failures: the index
            // is the diagnostic (a block-tail bug and a seam bug look identical
            // in a pass/fail, and completely different in an index).
            for (u32 i = 0; i < count; ++i)
            {
                ASSERT_EQ(actual[i], expected[i])
                    << "count = " << count << ", first divergence at index " << i
                    << " (block " << (i / GPUPrefixSum::kGroupSize)
                    << ", lane " << (i % GPUPrefixSum::kGroupSize) << ")";
            }
        }
    }

    // =========================================================================
    // Acceptance criterion #1b — an all-ones input, where the exclusive scan of
    // n ones must be exactly 0,1,2,...,n-1.
    //
    // This is the compaction case in its purest form (every element survives),
    // and it is the one where an off-by-one is unambiguous: the expected values
    // are derivable by inspection rather than by a reference implementation
    // that could share a bug with the thing it checks.
    // =========================================================================
    TEST(GPUPrefixSumTest, AllOnesScanIsTheIdentityRamp)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUPrefixSum scan;
        scan.EnsureInitialised();
        ASSERT_TRUE(scan.IsAvailable());

        for (const u32 count : { 1u, 255u, 256u, 257u, 65537u })
        {
            const std::vector<u32> input(count, 1u);

            u32 total = 0;
            const std::vector<u32> actual = GpuExclusiveScan(scan, input, total);

            ASSERT_EQ(actual.size(), count);
            EXPECT_EQ(total, count) << "count = " << count;
            for (u32 i = 0; i < count; ++i)
                ASSERT_EQ(actual[i], i) << "count = " << count << ", index " << i;
        }
    }

    // =========================================================================
    // Acceptance criterion #2 (the property that motivates the whole issue) —
    // the scan is DETERMINISTIC.
    //
    // This is the test that would pass vacuously on the `atomicAdd` compaction
    // being replaced: an atomic produces the same SET every run and a different
    // ORDER, so a membership check proves nothing. Here the same input is
    // scanned repeatedly and every run must be element-for-element identical.
    //
    // A sparse 0/1 flag array is used deliberately: it is exactly a compaction's
    // survivor mask, so `result[i]` is the output slot survivor `i` would be
    // given, and identical results across runs means identical output ORDER.
    // =========================================================================
    TEST(GPUPrefixSumTest, ScanIsDeterministicAcrossRepeatedRuns)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUPrefixSum scan;
        scan.EnsureInitialised();
        ASSERT_TRUE(scan.IsAvailable());

        constexpr u32 kCount = 20'000; // spans 79 work groups: plenty of seams
        std::mt19937 rng(0x713u);
        std::bernoulli_distribution keep(0.37);
        std::vector<u32> flags(kCount);
        for (u32& f : flags)
            f = keep(rng) ? 1u : 0u;

        u32 firstTotal = 0;
        const std::vector<u32> first = GpuExclusiveScan(scan, flags, firstTotal);

        // Cross-check the first run against the CPU before asserting that the
        // other runs match it — otherwise "deterministic" could mean
        // "consistently wrong".
        u32 cpuTotal = 0;
        const std::vector<u32> expected = CpuExclusiveScan(flags, cpuTotal);
        ASSERT_EQ(first, expected);
        ASSERT_EQ(firstTotal, cpuTotal);

        for (u32 run = 1; run < 8; ++run)
        {
            u32 total = 0;
            const std::vector<u32> again = GpuExclusiveScan(scan, flags, total);
            ASSERT_EQ(again, first) << "run " << run << " differed from run 0 — the scan is not deterministic";
            ASSERT_EQ(total, firstTotal) << "run " << run;
        }

        // And the property a compaction actually depends on: slots are strictly
        // increasing in input index, i.e. survivors keep their input order.
        u32 previousSlot = 0;
        bool sawFirst = false;
        for (u32 i = 0; i < kCount; ++i)
        {
            if (flags[i] == 0u)
                continue;
            if (sawFirst)
                ASSERT_GT(first[i], previousSlot) << "survivor at index " << i << " did not get a later slot";
            previousSlot = first[i];
            sawFirst = true;
        }
        ASSERT_TRUE(sawFirst) << "the random mask kept nothing — the ordering assertion above ran vacuously";
    }

    // =========================================================================
    // Degenerate inputs. `count == 0` must not dispatch anything and must still
    // leave a defined total, because a caller reading a stale total from a
    // previous scan is a silent wrong-sized draw.
    // =========================================================================
    TEST(GPUPrefixSumTest, EmptyAndSingleElementScans)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUPrefixSum scan;
        scan.EnsureInitialised();
        ASSERT_TRUE(scan.IsAvailable());

        // Prime the total buffer with a non-zero scan first, so a zero read
        // afterwards proves the empty case WROTE zero rather than that nothing
        // ever wrote anything.
        u32 primedTotal = 0;
        GpuExclusiveScan(scan, std::vector<u32>{ 5u, 6u, 7u }, primedTotal);
        ASSERT_EQ(primedTotal, 18u);

        u32 emptyTotal = 123u;
        const std::vector<u32> empty = GpuExclusiveScan(scan, {}, emptyTotal);
        EXPECT_TRUE(empty.empty());
        EXPECT_EQ(emptyTotal, 0u);

        u32 singleTotal = 0;
        const std::vector<u32> single = GpuExclusiveScan(scan, std::vector<u32>{ 42u }, singleTotal);
        ASSERT_EQ(single.size(), 1u);
        EXPECT_EQ(single[0], 0u) << "the first element of an exclusive scan is always 0";
        EXPECT_EQ(singleTotal, 42u);
    }

    // =========================================================================
    // The work-group primitive itself, both paths, against the CPU reference
    // AND against each other.
    //
    // The cross-check is the valuable half: `PrefixSum.glsl` is one file with
    // two implementations behind an `#ifdef`, and the failure mode that costs
    // real time is the two silently disagreeing on hardware that picks the path
    // the developer did not test.
    // =========================================================================
    TEST(GPUPrefixSumTest, WorkGroupScanAgreesAcrossBothPaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::vector<u32> values = MakeRandomValues(kProbeGroupSize, 0xA5u, 255u);

        u32 cpuTotal = 0;
        const std::vector<u32> expected = CpuExclusiveScan(values, cpuTotal);

        // The portable path is mandatory — it is what every production shader
        // compiles, so a failure here is a real failure on any hardware.
        const ProbeResult shared = RunWorkGroupProbe("assets/shaders/tests/ShaderUnit_PrefixSumShared.comp", values);
        ASSERT_TRUE(shared.Compiled) << "the portable shared-memory probe must compile on every driver";
        ASSERT_EQ(shared.Exclusive.size(), kProbeGroupSize);
        EXPECT_FALSE(shared.TookSubgroupPath)
            << "the shared probe does not define OLO_PREFIX_SUM_USE_SUBGROUP, so it must not be on the subgroup path";
        EXPECT_EQ(shared.GroupTotal, cpuTotal);
        for (u32 i = 0; i < kProbeGroupSize; ++i)
            ASSERT_EQ(shared.Exclusive[i], expected[i]) << "shared path, lane " << i;

        // The subgroup path is hardware-dependent. Its probe declares
        // `#extension ... : require`, so it cannot compile without driver
        // support — and a failed compile is an assert dialog, not a return
        // value, so ask the driver first and never build the shader at all when
        // the answer is no.
        if (!DriverHasSubgroupOps())
        {
            GTEST_LOG_(WARNING) << "GL_KHR_shader_subgroup unavailable on this driver — "
                                   "the subgroup path was NOT exercised by this run";
            return;
        }

        const ProbeResult subgroup = RunWorkGroupProbe("assets/shaders/tests/ShaderUnit_PrefixSumSubgroup.comp", values);
        ASSERT_TRUE(subgroup.Compiled)
            << "the driver advertises GL_KHR_shader_subgroup but the subgroup probe did not compile";
        ASSERT_EQ(subgroup.Exclusive.size(), kProbeGroupSize);
        EXPECT_TRUE(subgroup.TookSubgroupPath)
            << "the subgroup probe compiled but reported the shared path — OLO_PREFIX_SUM_USE_SUBGROUP is not reaching PrefixSum.glsl";
        EXPECT_EQ(subgroup.GroupTotal, cpuTotal);
        for (u32 i = 0; i < kProbeGroupSize; ++i)
            ASSERT_EQ(subgroup.Exclusive[i], expected[i]) << "subgroup path, lane " << i;
    }
} // namespace OloEngine::Tests
