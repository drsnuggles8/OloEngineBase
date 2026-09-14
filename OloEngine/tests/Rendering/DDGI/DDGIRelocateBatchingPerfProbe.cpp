// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// DDGIRelocateBatchingPerfProbe.cpp — why DDGI relocation is one dispatch.
//
// Issue #846. `DDGIProbeUpdatePass::Execute` used to relocate the capture set
// in a tight loop: every iteration wrote `m_PassDataUBO` and then dispatched
// ONE 64-thread work group that read that same buffer. The previous iteration's
// dispatch had read it too, so every iteration was a write-after-read hazard on
// one buffer, and the issue named that hazard as the suspected cost — drivers
// usually rename around it but are not required to, and 64 threads is small
// enough that a stall would dominate.
//
// The issue's own instruction was "measure the cost, or batch", and the
// measurement had not been done. This is the measurement, kept so the change it
// justified stays falsifiable. IT DID NOT CONFIRM THE ISSUE'S DIAGNOSIS: the
// hazard is free on this driver and the DISPATCH BOUNDARY is what costs, which
// is why the fix is one dispatch rather than a double-buffered UBO.
//
// THREE ARMS. A and B differ in exactly one thing — how many uniform buffers
// the loop cycles through — and C changes only the dispatch count.
//
//   A. `serial-1ubo`  — the pre-#846 shape. N x (SetData(ubo) + Bind + dispatch)
//                       against ONE buffer, so dispatch k+1's write lands on the
//                       buffer dispatch k is reading.
//   B. `serial-Nubo`  — N distinct buffers, each written once and bound once.
//                       Identical GL call count, identical bytes uploaded,
//                       identical dispatches, identical probe set, identical
//                       texels touched — and no write-after-read hazard on any
//                       one buffer.
//   C. `batched`      — one SetData and ONE DispatchCompute(N, 1, 1). The
//                       shipping shape since #846.
//
// ALL THREE ARMS RUN THE SAME SHADER, which is what makes them comparable:
// DDGI_Relocate.comp takes its probe index from `u_DDGICaptureSet[wg]`, so a
// one-entry capture set per dispatch reproduces the pre-#846 loop exactly and
// an N-entry one is the batched form. There is no probe-only shader to drift
// from the engine's.
//
// The buffer the arms cycle is therefore DDGIRelocateParams (1 KB), which is
// what the shipping pass writes too. That it is a block of its OWN rather than
// part of DDGIPassData is a separate decision, made for upload volume during
// capture rather than for anything this probe measures — see the struct.
//
// Arm B is the control the issue asked for ("the same loop with the UBO write
// hoisted"), fixed so it is not also a different measurement: hoisting the
// write outright would have pointed every dispatch at the SAME probe, turning
// N scattered hit-atlas tiles into one cached tile and charging arm A for a
// texture-locality difference the change would not actually remove. Cycling
// buffers keeps the memory traffic identical and isolates the hazard alone.
//
// A - B is what the UBO hazard costs. A - C is what batching would actually
// recover, hazard and dispatch boundaries together. Splitting them matters: the
// issue names the hazard as the mechanism, and the two numbers are free to
// disagree about whether that diagnosis was right.
//
// Arm C is checked for EQUIVALENCE, not just speed: before the sweep, arms A
// and C each run once from a cleared probe-data image and the resulting images
// are compared texel for texel. A batched arm that quietly relocated a
// different probe set would otherwise look like a free speed-up — and since
// arm A is the pre-#846 call shape, that check is also a regression test for
// the change itself: batching must not move a single probe.
//
// BOTH A GPU AND A WALL-CLOCK NUMBER, because they answer different questions.
// GL_TIME_ELAPSED brackets GPU work only; a driver that stalls the CPU inside
// glNamedBufferSubData waiting for the previous dispatch would be invisible to
// it and obvious in the wall clock. The submit column is the loop alone; the
// total column is the loop plus glFinish.
//
// DISABLED_ ON PURPOSE, for the reasons GPUPrefixSumPerfProbe states: this is
// an instrument, not a gate. It asserts no timing, so it cannot fail for
// environmental reasons, and L6 perf baselines are dev-workstation-only here.
//
//   OloEngine-Tests.exe --gtest_also_run_disabled_tests \
//                       --gtest_filter='*DDGIRelocateBatchingPerfProbe*'
//
// WHAT IT DOES NOT COVER. It drives DDGI_Relocate.comp directly against a
// synthetic hit atlas rather than running the real pass, so the absolute
// milliseconds are not a frame budget — they are the relocation loop's cost in
// isolation, which is the quantity the issue is about. The synthetic atlas is
// procedural (a mix of sky, frontface and backface texels at varying distance)
// so the shader's branches are exercised rather than early-outed, but it is not
// a particular scene's geometry.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The cascaded default the issue names: 16^3 probes per cascade, four
        // cascades, 16x16 hit tiles, and a capture budget that reaches 32
        // probes per frame at DDGIBudgetScale 2.
        constexpr glm::ivec3 kResolution{ 16, 16, 16 };
        constexpr i32 kCascadeCount = 4;
        constexpr i32 kHitCacheTexels = 16;
        constexpr u32 kRelocateGroupSize = 64;

        // One GL_TIME_ELAPSED sample around whatever `work` dispatches, plus the
        // wall clock for the submission itself and for submission + drain. The
        // GPU bracket cannot see a driver that blocks the CPU inside a buffer
        // update, which is precisely the failure mode under test, so the wall
        // clock is not decoration here.
        struct Sample
        {
            f64 GpuMs = 0.0;
            f64 SubmitMs = 0.0;
            f64 TotalMs = 0.0;
        };

        template<typename Fn>
        Sample TimeArm(Fn&& work)
        {
            using Clock = std::chrono::steady_clock;

            GLuint query = 0;
            ::glGenQueries(1, &query);

            const auto begin = Clock::now();
            ::glBeginQuery(GL_TIME_ELAPSED, query);
            work();
            ::glEndQuery(GL_TIME_ELAPSED);
            const auto submitted = Clock::now();
            ::glFinish();
            const auto drained = Clock::now();

            GLuint64 elapsedNs = 0;
            ::glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsedNs);
            ::glDeleteQueries(1, &query);

            Sample s;
            s.GpuMs = static_cast<f64>(elapsedNs) / 1.0e6;
            s.SubmitMs = std::chrono::duration<f64, std::milli>(submitted - begin).count();
            s.TotalMs = std::chrono::duration<f64, std::milli>(drained - begin).count();
            return s;
        }

        // Median rather than mean: this box is never idle and a single
        // scheduling hiccup moves a mean and leaves a median alone.
        f64 Median(std::vector<f64> values)
        {
            if (values.empty())
                return 0.0;
            std::ranges::sort(values);
            return values[values.size() / 2];
        }

        // A synthetic hit atlas with the branch mix the real one has: roughly a
        // fifth sky (dist < 0), a tenth backface (flag 0.5), the rest frontface
        // hits spread over the cascade's ray range, and a handful crowded close
        // enough to drive the spring. Deterministic — no RNG, so two runs of the
        // probe feed the shader byte-identical input.
        std::vector<f32> MakeHitGeo(const glm::ivec2& atlasTexels, f32 maxRayDistance)
        {
            const sizet texels = static_cast<sizet>(atlasTexels.x) * static_cast<sizet>(atlasTexels.y);
            std::vector<f32> rgba(texels * 4u, 0.0f);
            for (sizet i = 0; i < texels; ++i)
            {
                const u32 h = static_cast<u32>(i) * 2654435761u; // Knuth, deterministic
                const u32 bucket = (h >> 8) % 100u;
                f32* texel = rgba.data() + i * 4u;
                // rg = octahedrally encoded normal; the relocation shader reads
                // only b (distance) and a (frontface flag), so a plausible
                // constant pair is enough and keeps the input reproducible.
                texel[0] = 0.5f;
                texel[1] = 0.5f;
                if (bucket < 20u)
                {
                    texel[2] = -1.0f; // sky
                    texel[3] = 0.0f;
                    continue;
                }
                const f32 t = static_cast<f32>((h >> 16) % 1024u) / 1023.0f;
                if (bucket < 30u)
                {
                    texel[2] = 0.05f * maxRayDistance + 0.45f * t * maxRayDistance;
                    texel[3] = 0.5f; // backface
                    continue;
                }
                // Frontface. The lowest decile lands inside minFrontfaceDistance
                // so the crowding term is live rather than always zero.
                texel[2] = (bucket < 37u) ? (0.01f * maxRayDistance + 0.03f * t * maxRayDistance)
                                          : (0.1f * maxRayDistance + 0.9f * t * maxRayDistance);
                texel[3] = 1.0f; // frontface
            }
            return rgba;
        }

        UBOStructures::DDGIVolumeUBO MakeVolumeUBO(i32 totalProbes, f32 baseSpacing)
        {
            UBOStructures::DDGIVolumeUBO ubo{};
            ubo.BoundsMin = glm::vec4(-64.0f, -64.0f, -64.0f, 0.0f);
            ubo.BoundsMax = glm::vec4(64.0f, 64.0f, 64.0f, 0.0f);
            ubo.GridDimensions = glm::ivec4(kResolution, totalProbes);
            ubo.ProbeSpacing = glm::vec4(baseSpacing, baseSpacing, baseSpacing, baseSpacing);
            ubo.Enabled = 1;
            ubo.Intensity = 1.0f;
            ubo.Hysteresis = 0.9f;
            ubo.SelfShadowBias = 0.3f;
            ubo.HitCacheTexels = kHitCacheTexels;
            ubo.FrameIndex = 1;
            ubo.HybridBlend = 1.0f;
            ubo.EnergyConservation = 0.9f;
            ubo.MaxRayDistance = DDGI::kMaxRayDistanceSpacingScale * baseSpacing * std::sqrt(3.0f);
            ubo.BounceMarginScale = 1.0f;
            ubo.CascadeCount = kCascadeCount;
            ubo.CascadeBlendBand = DDGI::kDefaultCascadeBlendBand;
            ubo.UpdateRateDivisor = 1;
            ubo.RequestLifetime = static_cast<i32>(DDGI::kProbeRequestLifetimeFrames);
            ubo.SparsityEnabled = 0;

            for (i32 level = 0; level < DDGI::kMaxCascades; ++level)
            {
                const glm::vec3 spacing = DDGI::CascadeSpacing(glm::vec3(baseSpacing), std::min(level, kCascadeCount - 1));
                const f32 minAxial = std::min(std::min(spacing.x, spacing.y), spacing.z);
                const f32 maxRay = DDGI::kMaxRayDistanceSpacingScale * glm::length(spacing);
                ubo.CascadeOrigin[level] = glm::vec4(-0.5f * spacing * glm::vec3(kResolution), maxRay);
                ubo.CascadeSpacing[level] = glm::vec4(spacing, minAxial);
                ubo.CascadeLattice[level] = glm::ivec4(0);
            }
            return ubo;
        }

        // The only per-arm input: which probes this dispatch relocates. Every
        // other thing DDGI_Relocate.comp reads lives in the volume UBO and is
        // identical across the arms.
        UBOStructures::DDGIRelocateParamsUBO MakeRelocateParams(std::span<const i32> probes)
        {
            UBOStructures::DDGIRelocateParamsUBO params{};
            // Never a silent truncation: the engine chunks a capture set longer
            // than the array, and a probe that quietly measured a shorter one
            // would report the batched arm as faster than it is.
            EXPECT_LE(probes.size(), sizet{ UBOStructures::DDGIRelocateParamsUBO::MaxRelocationBatch });
            for (sizet i = 0; i < probes.size(); ++i)
                params.CaptureSet[i] = glm::ivec4(probes[i], 0, 0, 0);
            return params;
        }

        // The capture set the scheduler would hand the relocation loop: a linear
        // cursor over the global probe index space, which spreads the probes
        // across cascades and therefore across atlas tile columns — the same
        // scatter both arms must pay for.
        std::vector<i32> MakeCaptureSet(u32 count, i32 totalProbes)
        {
            std::vector<i32> probes;
            probes.reserve(count);
            const i32 stride = std::max(totalProbes / static_cast<i32>(count), 1);
            for (u32 i = 0; i < count; ++i)
                probes.push_back(static_cast<i32>(i) * stride % totalProbes);
            return probes;
        }
    } // namespace

    TEST(DDGIRelocateBatchingPerfProbe, DISABLED_PerProbeUboWriteVersusDistinctBuffers)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto relocate = ComputeShader::Create("assets/shaders/compute/DDGI_Relocate.comp");
        ASSERT_TRUE(relocate && relocate->IsValid()) << "DDGI_Relocate.comp failed to compile";

        const i32 probesPerCascade = DDGI::ProbesPerCascade(kResolution);
        const i32 totalProbes = probesPerCascade * kCascadeCount;
        const glm::ivec2 tileDims = DDGI::CascadedAtlasTileDimensions(kResolution, kCascadeCount);
        const glm::ivec2 hitTexels = tileDims * kHitCacheTexels;

        // Volume UBO (binding 51) — constant across every arm and every sweep
        // row, so it is written once and never touched again.
        constexpr f32 kBaseSpacing = DDGI::kDefaultBaseProbeSpacing;
        const UBOStructures::DDGIVolumeUBO volume = MakeVolumeUBO(totalProbes, kBaseSpacing);
        auto volumeUBO = UniformBuffer::Create(UBOStructures::DDGIVolumeUBO::GetSize(), ShaderBindingLayout::UBO_DDGI);
        volumeUBO->SetData(&volume, sizeof(volume));
        volumeUBO->Bind();

        // Aux SSBO (binding 6): 32 B header then one 32 B record per probe.
        constexpr u32 kHeaderBytes = 32u;
        constexpr u32 kRecordBytes = 32u;
        const u32 auxBytes = kHeaderBytes + static_cast<u32>(totalProbes) * kRecordBytes;
        auto auxSSBO = StorageBuffer::Create(auxBytes, ShaderBindingLayout::SSBO_DDGI_PROBE_AUX,
                                             StorageBufferUsage::DynamicCopy);
        const std::vector<u8> zeroed(auxBytes, 0u);
        auxSSBO->SetData(zeroed.data(), auxBytes, 0);

        // Probe data image (rgba16f, one texel per probe) and the hit atlas the
        // group reduces. Both are the real pass's shapes.
        const RHI::ResourceHandle probeData = RenderCommand::CreateTexture2DHandle(
            static_cast<u32>(tileDims.x), static_cast<u32>(tileDims.y), RHI::Format::RGBA16Float);
        ASSERT_TRUE(probeData.IsValid());
        RenderCommand::ClearTextureFloat(probeData, 0, glm::vec4(0.0f));
        RenderCommand::SetTextureFilter(probeData, RHI::Filter::Nearest, RHI::Filter::Nearest);

        const RHI::ResourceHandle hitGeo = RenderCommand::CreateTexture2DHandle(
            static_cast<u32>(hitTexels.x), static_cast<u32>(hitTexels.y), RHI::Format::RGBA16Float);
        ASSERT_TRUE(hitGeo.IsValid());
        RenderCommand::SetTextureFilter(hitGeo, RHI::Filter::Nearest, RHI::Filter::Nearest);
        {
            const f32 maxRay = DDGI::kMaxRayDistanceSpacingScale * kBaseSpacing * std::sqrt(3.0f);
            const std::vector<f32> geo = MakeHitGeo(hitTexels, maxRay);
            RenderCommand::UploadTextureSubImage2D(hitGeo, 0, 0, static_cast<u32>(hitTexels.x),
                                                   static_cast<u32>(hitTexels.y),
                                                   RHI::Format::RGBA32Float, geo.data());
        }

        constexpr u32 kMaxProbes = 64;
        constexpr u32 kWarmupMs = 250; // warm up by DURATION, not by iteration count
        constexpr u32 kSamples = 31;

        // Arm B's buffer pool, allocated once for the widest sweep row so that
        // allocation cost never lands inside a timed region.
        std::vector<Ref<UniformBuffer>> pool;
        pool.reserve(kMaxProbes);
        for (u32 i = 0; i < kMaxProbes; ++i)
            pool.push_back(UniformBuffer::Create(UBOStructures::DDGIRelocateParamsUBO::GetSize(),
                                                 ShaderBindingLayout::UBO_USER_0));
        Ref<UniformBuffer> single = UniformBuffer::Create(UBOStructures::DDGIRelocateParamsUBO::GetSize(),
                                                          ShaderBindingLayout::UBO_USER_0);

        // The three arms, parameterised by capture-set size. Declared here so the
        // equivalence check below and the sweep drive byte-identical code.
        const auto dispatchOne = [&](i32 probeIdx, UniformBuffer* params)
        {
            const UBOStructures::DDGIRelocateParamsUBO data = MakeRelocateParams({ &probeIdx, 1 });
            params->SetData(&data, sizeof(data));
            params->Bind();
            HeapBinding::BindImageOrOffset(0, probeData, 0, false, 0, RHI::Access::StorageReadWrite,
                                           RHI::Format::RGBA16Float, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindTextureOrOffset(1, hitGeo, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(1, 1, 1);
        };

        // A: every dispatch writes and reads the SAME buffer.
        const auto runSerialOne = [&](const std::vector<i32>& captureSet)
        {
            relocate->Bind();
            auxSSBO->Bind();
            for (const i32 probeIdx : captureSet)
                dispatchOne(probeIdx, single.Raw());
            // TextureUpdate as well as ShaderImageAccess: glFinish orders the
            // COMMANDS, it does not make an imageStore visible to
            // glGetTextureImage. Without it the equivalence readback below can
            // compare two stale buffers and agree for the wrong reason.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess |
                                         MemoryBarrierFlags::ShaderStorage |
                                         MemoryBarrierFlags::TextureUpdate);
        };

        // B: identical calls, identical bytes, identical probes — but each
        // buffer is written exactly once, so no dispatch's read races the next
        // dispatch's write.
        const auto runSerialMany = [&](const std::vector<i32>& captureSet)
        {
            relocate->Bind();
            auxSSBO->Bind();
            for (sizet i = 0; i < captureSet.size(); ++i)
                dispatchOne(captureSet[i], pool[i].Raw());
            // TextureUpdate as well as ShaderImageAccess: glFinish orders the
            // COMMANDS, it does not make an imageStore visible to
            // glGetTextureImage. Without it the equivalence readback below can
            // compare two stale buffers and agree for the wrong reason.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess |
                                         MemoryBarrierFlags::ShaderStorage |
                                         MemoryBarrierFlags::TextureUpdate);
        };

        // C: one write, one dispatch, N work groups.
        const auto runBatched = [&](const std::vector<i32>& captureSet)
        {
            relocate->Bind();
            auxSSBO->Bind();
            const UBOStructures::DDGIRelocateParamsUBO data = MakeRelocateParams(captureSet);
            single->SetData(&data, sizeof(data));
            single->Bind();
            HeapBinding::BindImageOrOffset(0, probeData, 0, false, 0, RHI::Access::StorageReadWrite,
                                           RHI::Format::RGBA16Float, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindTextureOrOffset(1, hitGeo, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(static_cast<u32>(captureSet.size()), 1, 1);
            // TextureUpdate as well as ShaderImageAccess: glFinish orders the
            // COMMANDS, it does not make an imageStore visible to
            // glGetTextureImage. Without it the equivalence readback below can
            // compare two stale buffers and agree for the wrong reason.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess |
                                         MemoryBarrierFlags::ShaderStorage |
                                         MemoryBarrierFlags::TextureUpdate);
        };

        // Equivalence: arm C must relocate the SAME probes to the SAME places.
        // Both arms start from a cleared image so neither inherits the other's
        // spring state, and the comparison is exact — the two paths run the same
        // arithmetic on the same inputs, so anything but bit equality means they
        // disagree about which probe a work group is.
        {
            constexpr u32 kCheckProbes = 32;
            const std::vector<i32> captureSet = MakeCaptureSet(kCheckProbes, totalProbes);
            const sizet imageFloats = static_cast<sizet>(tileDims.x) * static_cast<sizet>(tileDims.y) * 4u;
            std::vector<f32> serialImage(imageFloats, 0.0f);
            std::vector<f32> batchedImage(imageFloats, 0.0f);

            RenderCommand::ClearTextureFloat(probeData, 0, glm::vec4(0.0f));
            runSerialOne(captureSet);
            ::glFinish();
            ASSERT_TRUE(RenderCommand::ReadTextureImage(probeData, 0, RHI::Format::RGBA32Float,
                                                        serialImage.size() * sizeof(f32), serialImage.data()));

            RenderCommand::ClearTextureFloat(probeData, 0, glm::vec4(0.0f));
            runBatched(captureSet);
            ::glFinish();
            ASSERT_TRUE(RenderCommand::ReadTextureImage(probeData, 0, RHI::Format::RGBA32Float,
                                                        batchedImage.size() * sizeof(f32), batchedImage.data()));

            // Counted rather than ASSERT_EQ on the vectors: the images are
            // 64x256x4 floats and gtest would print both of them.
            sizet mismatches = 0;
            for (sizet i = 0; i < imageFloats; ++i)
            {
                if (serialImage[i] != batchedImage[i])
                    ++mismatches;
            }
            ASSERT_EQ(mismatches, 0u)
                << mismatches << " of " << imageFloats
                << " probe-data floats differ — the batched arm relocated a different probe set, so its"
                   " timings are not comparable with the serial arms'";
        }

        std::printf("\n  DDGI relocation, %d probes/cascade x %d cascades, %dx%d hit tiles\n",
                    kResolution.x, kCascadeCount, kHitCacheTexels, kHitCacheTexels);
        std::printf("    A = one UBO rewritten per dispatch (pre-#846)   B = one UBO per dispatch (no WAR hazard)\n");
        std::printf("    C = one UBO, one DispatchCompute(N,1,1) (shipping)\n\n");
        std::printf("  probes  A gpu(ms)  B gpu(ms)  C gpu(ms)   A submit  C submit   A total   C total   A-B gpu   A-C gpu\n");
        std::printf("  ------ ---------- ---------- ---------- ---------- --------- --------- --------- --------- ---------\n");

        // Up to MaxRelocationBatch, which is what one batched dispatch covers.
        for (const u32 probeCount : { 4u, 8u, 16u, 32u, 64u })
        {
            const std::vector<i32> captureSet = MakeCaptureSet(probeCount, totalProbes);

            // Warm up by duration and with EVERY arm, so none pays the
            // first-touch cost of its shader, the atlas or the pool.
            {
                const auto warmupStart = std::chrono::steady_clock::now();
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - warmupStart)
                           .count() < kWarmupMs)
                {
                    runSerialOne(captureSet);
                    runSerialMany(captureSet);
                    runBatched(captureSet);
                    ::glFinish();
                }
            }

            // INTERLEAVED, not all-of-A-then-all-of-B: a thermal or contention
            // drift over the sample window then hits every arm equally instead
            // of being attributed to whichever ran last.
            std::vector<f64> aGpu, bGpu, cGpu, aSubmit, cSubmit, aTotal, cTotal;
            for (u32 i = 0; i < kSamples; ++i)
            {
                const Sample a = TimeArm([&]()
                                         { runSerialOne(captureSet); });
                const Sample b = TimeArm([&]()
                                         { runSerialMany(captureSet); });
                const Sample c = TimeArm([&]()
                                         { runBatched(captureSet); });
                aGpu.push_back(a.GpuMs);
                bGpu.push_back(b.GpuMs);
                cGpu.push_back(c.GpuMs);
                aSubmit.push_back(a.SubmitMs);
                cSubmit.push_back(c.SubmitMs);
                aTotal.push_back(a.TotalMs);
                cTotal.push_back(c.TotalMs);
            }

            const f64 aGpuMs = Median(std::move(aGpu));
            const f64 bGpuMs = Median(std::move(bGpu));
            const f64 cGpuMs = Median(std::move(cGpu));
            std::printf("  %6u %10.4f %10.4f %10.4f %10.4f %9.4f %9.4f %9.4f %9.4f %9.4f\n", probeCount,
                        aGpuMs, bGpuMs, cGpuMs,
                        Median(std::move(aSubmit)), Median(std::move(cSubmit)),
                        Median(std::move(aTotal)), Median(std::move(cTotal)),
                        aGpuMs - bGpuMs, aGpuMs - cGpuMs);
            std::fflush(stdout);
        }

        static_assert(kRelocateGroupSize == 64u, "DDGI_RELOCATE_GROUP mirror drifted");

        RenderCommand::DeleteTexture(probeData);
        RenderCommand::DeleteTexture(hitGeo);
        SUCCEED() << "measurement only — see the table above";
    }
} // namespace OloEngine::Tests
