// OLO_TEST_LAYER: L3
//
// The GPU compute BC6H encoder (#624 item 3) against the CPU one it ports.
//
// The shader runs the same fourteen-mode search as Renderer/BC6HEncoder.cpp, with the
// quantization, index selection and bit packing in integer arithmetic — identical to the
// CPU's — and only the PCA fit and least-squares refit in `float` where the CPU uses
// `double`. So the two are expected to agree on most blocks and to be equally good on the
// rest; what must NOT happen is the GPU quietly producing worse blocks, which an
// "it produced output" test would never notice.
//
// Every case therefore measures BOTH paths on the same source through the vendored bcdec
// decoder and compares them. The bit-identical fraction is reported rather than asserted:
// it is a useful number, but pinning it would turn a harmless float-precision difference
// into a red test.
//
// Needs a real GL 4.6 context; SKIPs cleanly without one.

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/BC6HGpuEncoder.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

using namespace OloEngine;
using namespace OloEngine::Tests;

namespace
{
    // The engine loads shaders by a path relative to OloEditor/ (ctest sets that as the
    // working directory). Running the binary straight from the repo root is also a
    // documented way to invoke it, and there the compute shader would simply not be
    // found — so pin the directory for the duration of the test rather than skipping and
    // reporting a green run that verified nothing.
    class ScopedEditorWorkingDirectory
    {
      public:
        ScopedEditorWorkingDirectory()
            : m_Previous(std::filesystem::current_path())
        {
            std::error_code ec;
            std::filesystem::current_path(std::filesystem::path{ OLO_TEST_EDITOR_ROOT }, ec);
        }
        ~ScopedEditorWorkingDirectory()
        {
            std::error_code ec;
            std::filesystem::current_path(m_Previous, ec);
        }
        ScopedEditorWorkingDirectory(const ScopedEditorWorkingDirectory&) = delete;
        ScopedEditorWorkingDirectory& operator=(const ScopedEditorWorkingDirectory&) = delete;
        ScopedEditorWorkingDirectory(ScopedEditorWorkingDirectory&&) = delete;
        ScopedEditorWorkingDirectory& operator=(ScopedEditorWorkingDirectory&&) = delete;

      private:
        std::filesystem::path m_Previous;
    };

    // Registering the hook is global state, so it has to come back off even when a test
    // leaves early — an ASSERT_* inside a helper returns from the helper, and a leaked
    // registration would then reroute every later test's BC6H encode through the GPU.
    class ScopedGpuHook
    {
      public:
        ScopedGpuHook()
        {
            BC6HGpu::RegisterWithTextureCompression();
        }
        ~ScopedGpuHook()
        {
            BC6HGpu::UnregisterFromTextureCompression();
        }
        ScopedGpuHook(const ScopedGpuHook&) = delete;
        ScopedGpuHook& operator=(const ScopedGpuHook&) = delete;
        ScopedGpuHook(ScopedGpuHook&&) = delete;
        ScopedGpuHook& operator=(ScopedGpuHook&&) = delete;
    };

    constexpr u32 kDim = 64;

    // A curved HDR gradient with a hard edge through every block: exercises the
    // one-subset high-precision modes and the two-subset ones in one image.
    std::vector<f32> MakeMixedHDR(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kDim - 1);
                const f32 fy = static_cast<f32>(y) / static_cast<f32>(kDim - 1);
                const bool upper = ((x % 4) + (y % 4)) < 3;
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = 0.05f + peak * fx * fx * (upper ? 1.0f : 0.35f);
                p[1] = 0.05f + peak * fy * (upper ? 0.4f : 1.0f);
                p[2] = 0.05f + peak * (fx * fy) * 0.5f;
            }
        }
        return pixels;
    }

    // Signed source: a field that swings through zero.
    std::vector<f32> MakeSignedHDR(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 fx = (static_cast<f32>(x) / static_cast<f32>(kDim - 1)) * 2.0f - 1.0f;
                const f32 fy = (static_cast<f32>(y) / static_cast<f32>(kDim - 1)) * 2.0f - 1.0f;
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = peak * fx;
                p[1] = peak * fy;
                p[2] = peak * fx * fy;
            }
        }
        return pixels;
    }

    // Peak-normalized PSNR of a block buffer against the source, decoded via bcdec.
    double DecodedPSNR(const std::vector<u8>& blocks, const std::vector<f32>& source, bool isSigned, f32 peak)
    {
        CompressedTextureImage image;
        image.Format = isSigned ? TextureCompressionFormat::BC6HSigned : TextureCompressionFormat::BC6H;
        image.Width = kDim;
        image.Height = kDim;
        image.Mips.push_back(blocks);

        std::vector<f32> decoded;
        u32 dw = 0;
        u32 dh = 0;
        if (!TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh))
            return 0.0;

        double mse = 0.0;
        sizet samples = 0;
        for (sizet texel = 0; texel < static_cast<sizet>(kDim) * kDim; ++texel)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                const double d = static_cast<double>(source[texel * 3 + c]) - static_cast<double>(decoded[texel * 4 + c]);
                mse += d * d;
                ++samples;
            }
        }
        if (samples == 0)
            return 0.0;
        mse /= static_cast<double>(samples);
        return mse < 1e-12 ? 99.0 : 10.0 * std::log10((static_cast<double>(peak) * peak) / mse);
    }

    std::vector<u8> EncodeOnCpu(const std::vector<f32>& source, bool isSigned)
    {
        const CompressedTextureImage image =
            TextureCompression::EncodeBC6H(source.data(), kDim, kDim, 3, isSigned, /*mips*/ false);
        EXPECT_TRUE(image.IsValid());
        return image.IsValid() ? image.Mips[0] : std::vector<u8>{};
    }

    void CompareGpuAgainstCpu(const std::vector<f32>& source, bool isSigned, f32 peak, const char* label)
    {
        std::vector<u8> gpuBlocks;
        ASSERT_TRUE(BC6HGpu::EncodeLevel(source.data(), kDim, kDim, isSigned, gpuBlocks))
            << "GPU encode failed for " << label;
        const std::vector<u8> cpuBlocks = EncodeOnCpu(source, isSigned);
        ASSERT_EQ(gpuBlocks.size(), cpuBlocks.size());
        ASSERT_FALSE(cpuBlocks.empty());

        sizet identicalBlocks = 0;
        const sizet blockCount = cpuBlocks.size() / 16;
        for (sizet b = 0; b < blockCount; ++b)
        {
            if (std::equal(cpuBlocks.begin() + static_cast<std::ptrdiff_t>(b * 16),
                           cpuBlocks.begin() + static_cast<std::ptrdiff_t>(b * 16 + 16),
                           gpuBlocks.begin() + static_cast<std::ptrdiff_t>(b * 16)))
            {
                ++identicalBlocks;
            }
        }

        const double cpuPSNR = DecodedPSNR(cpuBlocks, source, isSigned, peak);
        const double gpuPSNR = DecodedPSNR(gpuBlocks, source, isSigned, peak);
        const double identicalPct = blockCount == 0 ? 0.0
                                                    : 100.0 * static_cast<double>(identicalBlocks) /
                                                          static_cast<double>(blockCount);
        std::printf("[ BC6H GPU ] %-16s CPU %6.2f dB   GPU %6.2f dB   bit-identical blocks %5.1f %%\n",
                    label, cpuPSNR, gpuPSNR, identicalPct);
        ::testing::Test::RecordProperty(std::string(label) + "_GpuPSNR_dB", std::to_string(gpuPSNR));
        ::testing::Test::RecordProperty(std::string(label) + "_CpuPSNR_dB", std::to_string(cpuPSNR));
        ::testing::Test::RecordProperty(std::string(label) + "_IdenticalBlocks_pct", std::to_string(identicalPct));

        EXPECT_GT(gpuPSNR, 20.0) << label << ": GPU output is not a plausible encode";
        // The bar that matters: the fast path must not be the worse path. A small
        // negative delta is allowed for the float-vs-double endpoint fit; a real porting
        // bug (a wrong table, a mis-packed field) costs far more than this.
        EXPECT_GT(gpuPSNR, cpuPSNR - 0.5)
            << label << ": GPU encode is materially worse than the CPU encode (" << gpuPSNR << " vs " << cpuPSNR << " dB)";
    }
} // namespace

TEST(BC6HGpuEncoder, MatchesTheCpuEncoderOnUnsignedHDR)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    CompareGpuAgainstCpu(MakeMixedHDR(8.0f), /*isSigned*/ false, 8.0f, "unsigned-mixed");
}

TEST(BC6HGpuEncoder, MatchesTheCpuEncoderOnSignedHDR)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    CompareGpuAgainstCpu(MakeSignedHDR(4.0f), /*isSigned*/ true, 4.0f, "signed-swing");
}

TEST(BC6HGpuEncoder, HandlesAPartialEdgeBlock)
{
    // A non-multiple-of-4 level exercises the shader's edge clamp, which is the one place
    // it re-implements a CPU behaviour (GatherBlockRGBFloat) rather than porting it.
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    constexpr u32 kW = 13;
    constexpr u32 kH = 7;
    std::vector<f32> source(static_cast<sizet>(kW) * kH * 3);
    for (u32 y = 0; y < kH; ++y)
    {
        for (u32 x = 0; x < kW; ++x)
        {
            f32* p = &source[(static_cast<sizet>(y) * kW + x) * 3];
            p[0] = 0.1f + static_cast<f32>(x) * 0.3f;
            p[1] = 0.1f + static_cast<f32>(y) * 0.5f;
            p[2] = 0.7f;
        }
    }

    std::vector<u8> gpuBlocks;
    ASSERT_TRUE(BC6HGpu::EncodeLevel(source.data(), kW, kH, /*isSigned*/ false, gpuBlocks));
    EXPECT_EQ(gpuBlocks.size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC6H, kW, kH));

    const CompressedTextureImage cpu =
        TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, /*isSigned*/ false, /*mips*/ false);
    ASSERT_TRUE(cpu.IsValid());
    EXPECT_EQ(gpuBlocks.size(), cpu.Mips[0].size());
}

TEST(BC6HGpuEncoder, TheRegisteredHookRoutesEncodeBC6HThroughTheGpuAndIsCounted)
{
    // The hook is what makes a cook use the GPU, and the counters are what let a bake
    // prove it did rather than assume it — a silent fall back to the CPU would otherwise
    // only show up as a slow bake.
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    const std::vector<f32> source = MakeMixedHDR(8.0f);

    TextureCompression::ResetBC6HEncodeCounts();
    CompressedTextureImage image;
    {
        const ScopedGpuHook hook;
        EXPECT_TRUE(TextureCompression::HasGpuBC6HEncoder());
        image = TextureCompression::EncodeBC6H(source.data(), kDim, kDim, 3, /*isSigned*/ false, /*mips*/ true);
    }

    ASSERT_TRUE(image.IsValid());
    const auto counts = TextureCompression::GetBC6HEncodeCounts();
    std::printf("[ BC6H GPU ] mip chain: %llu level(s) on the GPU, %llu on the CPU\n",
                static_cast<unsigned long long>(counts.GpuLevels),
                static_cast<unsigned long long>(counts.CpuLevels));
    EXPECT_EQ(counts.GpuLevels, static_cast<u64>(image.MipLevels()))
        << "every mip level should have gone through the GPU encoder";
    EXPECT_EQ(counts.CpuLevels, 0u);

    // And the result must still be a decodable, good-quality chain.
    std::vector<f32> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh));
    EXPECT_GT(DecodedPSNR(image.Mips[0], source, false, 8.0f), 30.0);
}

TEST(BC6HGpuEncoder, ReportsTheEncodeTimeOfBothPaths)
{
    // Item 3 exists to speed large bakes, so the speed-up is measured rather than
    // asserted — a timing threshold on a shared workstation is a flake generator, and
    // this is a DEBUG build, where the CPU encoder pays a penalty the GPU dispatch does
    // not. The Debug ratio therefore OVERSTATES what a Release cook would see; the number
    // worth trusting from this test is that the GPU path is the faster one at all, and by
    // roughly what order.
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    constexpr u32 kSide = 256;
    std::vector<f32> source(static_cast<sizet>(kSide) * kSide * 3);
    for (u32 y = 0; y < kSide; ++y)
    {
        for (u32 x = 0; x < kSide; ++x)
        {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(kSide - 1);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(kSide - 1);
            f32* p = &source[(static_cast<sizet>(y) * kSide + x) * 3];
            p[0] = 0.05f + 8.0f * fx * fx;
            p[1] = 0.05f + 8.0f * fy;
            p[2] = 0.05f + 8.0f * fx * fy * 0.5f;
        }
    }

    // Warm the GPU path once so shader compilation and the first allocation are not
    // counted as encode time.
    std::vector<u8> warm;
    ASSERT_TRUE(BC6HGpu::EncodeLevel(source.data(), kSide, kSide, false, warm));

    using Clock = std::chrono::steady_clock;
    const auto gpuStart = Clock::now();
    std::vector<u8> gpuBlocks;
    ASSERT_TRUE(BC6HGpu::EncodeLevel(source.data(), kSide, kSide, false, gpuBlocks));
    const auto gpuMs = std::chrono::duration<double, std::milli>(Clock::now() - gpuStart).count();

    const auto cpuStart = Clock::now();
    const CompressedTextureImage cpuImage =
        TextureCompression::EncodeBC6H(source.data(), kSide, kSide, 3, false, /*mips*/ false);
    const auto cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - cpuStart).count();
    ASSERT_TRUE(cpuImage.IsValid());

    const u32 blocks = TextureCompression::BlockCount(kSide) * TextureCompression::BlockCount(kSide);
    std::printf("[ BC6H GPU ] %ux%u (%u blocks), Debug build: CPU %.1f ms, GPU %.1f ms (%.1fx)\n",
                kSide, kSide, blocks, cpuMs, gpuMs, cpuMs / std::max(gpuMs, 0.001));
    ::testing::Test::RecordProperty("EncodeCpu_ms", std::to_string(cpuMs));
    ::testing::Test::RecordProperty("EncodeGpu_ms", std::to_string(gpuMs));

    EXPECT_EQ(gpuBlocks.size(), cpuImage.Mips[0].size());
}

TEST(BC6HGpuEncoder, AWorkerThreadsEncodeIsMarshalledToTheContextThread)
{
    // This is the path a real cook takes. AssetPackBuilder runs the whole build on a
    // worker (Tasks::Launch / FThread), so the encode call does NOT arrive on the thread
    // holding the GL context — it is queued, and the context thread runs it.
    //
    // The test plays both parts: this thread is the context thread and drains the queue,
    // a spawned thread does what the cook's worker does. Without the marshalling the
    // worker's call refuses and every level silently finishes on the CPU, which is exactly
    // the failure this is here to catch — so the counters are asserted, not just the
    // pixels.
    OLO_ENSURE_GPU_OR_SKIP();
    ScopedEditorWorkingDirectory cwd;
    BC6HGpu::SetContextThreadToCurrent();
    if (!BC6HGpu::IsAvailable())
        GTEST_SKIP() << "BC6H compute shaders unavailable on this context.";

    const std::vector<f32> source = MakeMixedHDR(8.0f);

    TextureCompression::ResetBC6HEncodeCounts();
    const ScopedGpuHook hook;

    std::atomic<bool> finished{ false };
    CompressedTextureImage image;
    std::thread worker([&]
                       {
                           image = TextureCompression::EncodeBC6H(source.data(), kDim, kDim, 3,
                                                                  /*isSigned*/ false, /*mips*/ true);
                           finished.store(true, std::memory_order_release); });

    // Stand in for Application::Run's per-frame ProcessTasks drain. Bounded so a
    // regression that stops queuing fails the test instead of hanging the suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    u32 pumped = 0;
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        // Sleep when there was nothing to run: the worker spends most of its time in
        // CPU-side block gathering between levels, and spinning a core flat out for
        // that is a tax on every other test sharing this machine.
        if (BC6HGpu::PumpPendingJobs() > 0)
            ++pumped;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();

    ASSERT_TRUE(finished.load(std::memory_order_acquire)) << "the worker never finished encoding";
    ASSERT_TRUE(image.IsValid());

    const auto counts = TextureCompression::GetBC6HEncodeCounts();
    std::printf("[ BC6H GPU ] marshalled from a worker: %llu level(s) on the GPU, %llu on the CPU (%u batches)\n",
                static_cast<unsigned long long>(counts.GpuLevels),
                static_cast<unsigned long long>(counts.CpuLevels), pumped);
    EXPECT_EQ(counts.GpuLevels, static_cast<u64>(image.MipLevels()))
        << "a worker's levels must reach the GPU through the queue, not fall back to the CPU";
    EXPECT_EQ(counts.CpuLevels, 0u);
    EXPECT_GT(pumped, 0u) << "nothing was ever drained — the job never reached the queue";

    // And the marshalled result must be the same encode, not merely a valid one.
    std::vector<u8> direct;
    ASSERT_TRUE(BC6HGpu::EncodeLevel(source.data(), kDim, kDim, false, direct));
    EXPECT_EQ(image.Mips[0], direct) << "marshalled blocks differ from the same encode run inline";
}

TEST(BC6HGpuEncoder, AnUndrainedQueueGivesUpOnceInsteadOfHanging)
{
    // The safety valve. If nothing drains the queue — a host with no frame loop, a
    // context thread that never came up — a blocked worker must not wait forever, and it
    // must not pay the timeout again on the next level. One give-up, then the whole path
    // latches off and the bake finishes on the CPU encoder.
    //
    // No GPU needed: this exercises the queue and the latch, not the shader.
    BC6HGpu::Shutdown(); // clears the context thread, so nothing can drain
    TextureCompression::ResetBC6HEncodeCounts();
    const ScopedGpuHook hook;

    constexpr u32 kSmall = 8;
    const std::vector<f32> source(static_cast<sizet>(kSmall) * kSmall * 3, 1.0f);

    const auto start = std::chrono::steady_clock::now();
    const CompressedTextureImage image =
        TextureCompression::EncodeBC6H(source.data(), kSmall, kSmall, 3, /*isSigned*/ false, /*mips*/ true);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(image.IsValid()) << "the cook must still produce a chain";
    const auto counts = TextureCompression::GetBC6HEncodeCounts();
    EXPECT_EQ(counts.GpuLevels, 0u);
    EXPECT_EQ(counts.CpuLevels, static_cast<u64>(image.MipLevels()))
        << "every level should have finished on the CPU encoder";
    // With no context thread recorded the encoder refuses outright rather than queueing,
    // so this is fast; the assertion is that it is not N x the 5 s job timeout.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10)
        << "an undrained queue must not cost a timeout per mip level";
}
